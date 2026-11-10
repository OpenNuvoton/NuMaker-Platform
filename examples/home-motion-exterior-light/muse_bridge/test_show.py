import base64
from pathlib import Path
import unittest
from unittest.mock import patch

from lxml import etree
from .executor import NumakerExecutor
from .graphics import frame_commands, pack, render, response_pages
from .show import ReplyAssembler, ShowController, compile_show, schema_payload
from .test_executor import RecordingTransport


def show(body, attrs=""):
    return f'<show version="1" {attrs}>{body}</show>'


def scene(body="", duration=1000, fade=0):
    return f'<scene duration-ms="{duration}" fade-ms="{fade}">{body}</scene>'


class SchemaTests(unittest.TestCase):
    def test_complete_example_compiles(self):
        program = compile_show(Path(__file__).with_name("demo-show.xml").read_text())
        self.assertEqual(len(program.scenes), 3)
        self.assertIsNone(program.repeat)
        self.assertEqual(program.scenes[0].levels[3:], (20, 0))
        self.assertTrue(program.responses["enabled"])
        self.assertEqual(len(program.scenes[0].frame), 1024)

    def test_maximum_native_cycle_and_conflicts(self):
        program = compile_show(Path(__file__).with_name("max-speed.xml").read_text())
        self.assertEqual(program.scenes[0].cycle, "CYCLE 50 70 20")
        for body in ('<cycle step-us="51"/>', '<rgb color="#ff0000"/><cycle step-us="50"/>',
                     '<white warm="20" cool="0"/><cycle step-us="50"/>'):
            with self.assertRaises(ValueError):
                compile_show(show(scene(body)))
        with self.assertRaises(ValueError):
            compile_show(show(scene('<cycle step-us="50"/>', fade=100)))

    def test_capabilities_describe_the_real_hardware(self):
        caps = schema_payload()["hardware"]
        self.assertEqual(caps["oled"]["colors"], ["black", "white"])
        self.assertEqual(caps["white_light"]["channels"], ["warm", "cool"])

    def test_rgb_hsv_brightness_and_white_inheritance(self):
        xml = show(scene('<rgb color="#ff0000" brightness="25"/><white temperature="100" brightness="40"/>')
                   + scene('<rgb hue="120" saturation="100" brightness="50"/>'))
        program = compile_show(xml)
        self.assertEqual(program.scenes[0].levels, (25, 0, 0, 0, 40))
        self.assertEqual(program.scenes[1].levels, (0, 50, 0, 0, 40))
        self.assertEqual(compile_show(show(scene('<white warm="80" cool="20" brightness="50"/>'))).scenes[0].levels[3:], (40, 10))

    def test_rejects_ambiguous_color_requests(self):
        for body in ('<rgb/>', '<rgb color="#ff0000" hue="0"/>',
                     '<rgb color="#ff0000" saturation="20"/>', '<white/>',
                     '<white warm="20"/>', '<white temperature="20" warm="10" cool="20"/>'):
            with self.subTest(body=body), self.assertRaises(ValueError):
                compile_show(show(scene(body)))

    def test_rejects_bad_ranges_and_unknown_elements(self):
        for xml in (show(scene('<rgb color="#f00"/>')), show(scene('<rgb hue="361"/>')),
                    show(scene('<white warm="101" cool="0"/>')), show(scene('<oled><pixel x="128" y="0"/></oled>')),
                    show(scene('<oled><text color="red">test</text></oled>')),
                    show(scene('<oled><script/></oled>')), show(scene('', duration=999)),
                    show(scene('', duration=1000, fade=1001)), show(scene(), 'repeat="0"'),
                    show(scene(), 'repeat="foreverx"'), '<show version="2"/>', show('')):
            with self.subTest(xml=xml), self.assertRaises(ValueError):
                compile_show(xml)

    def test_bounds_whole_show_work(self):
        for xml in (show(scene()*65), show(scene(duration=60000)*11), "x"*65537):
            with self.assertRaises(ValueError):
                compile_show(xml)

    def test_no_document_types_entities_or_external_resources(self):
        for prefix in ('<!DOCTYPE show [<!ENTITY value "text">]>',
                       '<!DOCTYPE show SYSTEM "file:///etc/passwd">'):
            with self.assertRaises(ValueError):
                compile_show(prefix + show(scene()))
        with self.assertRaises(ValueError):
            compile_show(show(scene('<oled><include href="https://example.com"/></oled>')))

    def test_all_graphics_and_bitmap_lengths(self):
        data = base64.b64encode(bytes([0xff]*8)).decode()
        xml = show(scene('<oled><text x="0" y="0" size="8">Hi</text>'
                         '<rect x="0" y="10" width="10" height="10" fill="true"/>'
                         '<circle x="30" y="30" radius="10"/>'
                         '<line x1="0" y1="63" x2="127" y2="0"/>'
                         '<pixel x="127" y="63"/>'
                         '<bitmap x="64" y="0" width="8" height="8">ffffffffffffffff</bitmap>'
                         f'<bitmap x="80" y="0" width="8" height="8" encoding="base64">{data}</bitmap></oled>'))
        self.assertEqual(len(compile_show(xml).scenes[0].frame), 1024)
        for data in ("ff", "zz", "gggggggggggggggg"):
            with self.assertRaises(ValueError):
                compile_show(show(scene(f'<oled><bitmap x="0" y="0" width="8" height="8">{data}</bitmap></oled>')))


class GraphicsTests(unittest.TestCase):
    def test_pixel_page_bit_order(self):
        frame = render(etree.fromstring(b'<oled><pixel x="0" y="0"/><pixel x="127" y="63"/></oled>'))
        self.assertEqual(frame[0], 1)
        self.assertEqual(frame[-1], 128)
        self.assertEqual(sum(v.bit_count() for v in frame), 2)

    def test_packed_rows_do_not_confuse_oled_pages(self):
        frame = render(etree.fromstring(b'<oled><bitmap x="0" y="0" width="2" height="2">8040</bitmap></oled>'))
        self.assertEqual(frame[:2], b'\x01\x02')

    def test_frame_commands_reconstruct_exactly_and_fit_wire_limits(self):
        frame = bytes(range(256))*4
        commands = frame_commands(frame, contrast=0, invert=True)
        self.assertLessEqual(len(commands), 40)
        self.assertTrue(all(len(command) <= 127 for command in commands))
        result = bytearray(1024)
        for command in commands:
            if command.startswith("BUFFER "):
                _, offset, data = command.split()
                value = bytes.fromhex(data)
                result[int(offset):int(offset)+len(value)] = value
        self.assertEqual(result, frame)
        self.assertEqual(commands[-2:], ["PRESENT", "OLED ON"])
        self.assertIn("OLED INVERT 1", commands)

    def test_reply_pages_are_bounded_and_monochrome(self):
        pages = response_pages("**Hello** \u2603\n" + "long reply "*1000)
        self.assertGreater(len(pages), 1)
        self.assertLessEqual(len(pages), 64)
        self.assertTrue(all(len(page) == 1024 for page in pages))


class PlaybackTests(unittest.TestCase):
    def setUp(self):
        self.transport = RecordingTransport()
        self.controller = ShowController(self.transport, start_worker=False)
        self.executor = NumakerExecutor(self.transport, self.controller)

    def test_bad_show_does_not_replace_running_show_or_write_to_board(self):
        self.controller.play(compile_show(show(scene('<rgb color="#ff0000"/>'))))
        program = self.controller.program
        reply = self.executor.run("show.play", {"xml": show(scene('<white warm="500" cool="0"/>'))})
        self.assertFalse(reply["ok"])
        self.assertIs(self.controller.program, program)
        self.assertTrue(self.controller.running)
        self.assertEqual(self.transport.commands, [])

    def test_fade_timeline_repeat_and_end_off(self):
        program = compile_show(show(scene('<rgb color="#ff0000"/>', fade=500)
                                    + scene('<rgb color="#0000ff"/>'), 'repeat="2" end="off"'))
        self.controller.play(program)
        for now in (0, 1, 2, 3, 4):
            self.controller.step(now)
        self.assertEqual(self.transport.commands, [["FADE 500 100 0 0 0 0"], ["FADE 0 0 0 100 0 0"],
                                                  ["FADE 500 100 0 0 0 0"], ["FADE 0 0 0 100 0 0"], ["OFF"]])
        self.assertFalse(self.controller.running)

    def test_infinite_show_and_manual_override(self):
        self.controller.play(compile_show(show(scene(), 'repeat="forever"')))
        for now in range(5):
            self.controller.step(now)
        self.assertTrue(self.controller.running)
        self.assertEqual(self.controller.iteration, 5)
        self.assertTrue(self.executor.run("lighting.off", {})["ok"])
        self.assertFalse(self.controller.running)
        calls = len(self.transport.commands)
        self.controller.step(10)
        self.assertEqual(len(self.transport.commands), calls)

    def test_replies_override_only_oled_while_light_scenes_continue(self):
        self.controller.play(compile_show(show(scene('<rgb color="#ff0000"/><oled><text>scene</text></oled>')
                                               + scene('<rgb color="#0000ff"/>'))))
        self.controller.feed_event({"event": "message.assistant", "message_id": "reply", "text": "Muse says hello"})
        self.controller.step(0)
        self.assertEqual(self.transport.commands[0], ["FADE 0 100 0 0 0 0"])
        self.assertEqual(self.transport.commands[1][-2:], ["PRESENT", "OLED ON"])
        self.assertTrue(self.controller.reply_active)
        self.controller.step(1)
        self.assertEqual(self.transport.commands[-1], ["FADE 0 0 0 100 0 0"])

    def test_invalid_reply_policy_is_atomic(self):
        original = dict(self.controller.policy)
        for params in ({"enabled": "yes"}, {"page_ms": 0}, {"hold_ms": 60001}):
            with self.assertRaises(ValueError):
                self.controller.configure(params)
        self.assertEqual(self.controller.policy, original)

    def test_reply_finishes_and_restores_scene_graphics(self):
        self.controller.play(compile_show(show(scene('<oled><text>show</text></oled>', duration=60000))))
        self.controller.feed_event({"event": "message.assistant", "message_id": "a", "text": "reply"})
        with patch("muse_bridge.show.time.monotonic", return_value=0):
            self.controller.step(0)
        self.assertTrue(self.controller.reply_active)
        self.controller.step(11)
        self.assertFalse(self.controller.reply_active)
        self.assertEqual(self.transport.commands[-1], frame_commands(self.controller.program.scenes[0].frame,
                                                                     **self.controller.program.scenes[0].oled))

    def test_disabled_reply_display_does_not_touch_board(self):
        self.controller.configure({"enabled": False})
        self.controller.feed_event({"event": "message.assistant", "message_id": "a", "text": "reply"})
        self.controller.step(0)
        self.assertEqual(self.transport.commands, [])
        self.assertFalse(self.controller.reply_active)

    def test_brightness_and_response_settings(self):
        self.executor.run("display.brightness", {"brightness": 50})
        self.assertEqual(self.transport.commands[-1], ["OLED CONTRAST 128"])
        result = self.executor.run("display.responses", {"enabled": False, "page_ms": 2000})
        self.assertTrue(result["ok"])
        self.assertFalse(self.controller.policy["enabled"])


class ReplyTests(unittest.TestCase):
    def test_streamed_text_and_finished_event_are_deduplicated(self):
        feed = ReplyAssembler().feed
        self.assertIsNone(feed({"event": "message.user", "message_id": "u", "text": "question"}))
        self.assertIsNone(feed({"event": "delta.text_append", "message_id": "a", "text": "Hello "}))
        self.assertIsNone(feed({"event": "delta.text_append", "message_id": "a", "text": "world"}))
        self.assertEqual(feed({"event": "delta.message_done", "message_id": "a", "text": ""}), "Hello world")
        self.assertIsNone(feed({"event": "message.assistant", "message_id": "a", "text": "Hello world"}))

    def test_stream_buffer_is_bounded(self):
        assembler = ReplyAssembler()
        for i in range(30):
            assembler.feed({"event": "delta.text_append", "message_id": str(i), "text": "x"*10000})
        self.assertLessEqual(len(assembler.buffers), 16)
        self.assertTrue(all(len(text) <= 8192 for text in assembler.buffers.values()))


if __name__ == "__main__":
    unittest.main()
