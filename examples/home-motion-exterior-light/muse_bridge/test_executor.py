"""Contract tests for Muse command validation and the USB response boundary."""
import json
import subprocess
import unittest

from muse_bridge.executor import (
    BridgeError, COMMAND_SPECS, NumakerExecutor, WindowsSerial, parse_status, prepare,
)

STATUS = "OK RGB 20 0 0 WHITE 10 10 OLED ON ADDR 60 MODEL SSD1306"


class RecordingTransport:
    port = "COM3"

    def __init__(self):
        self.commands = []

    def send(self, commands, timeout_ms=None):
        self.commands.append(commands)
        return parse_status(STATUS)


class CommandTests(unittest.TestCase):
    def test_registered_commands_have_handlers_and_schemas(self):
        self.assertEqual(set(COMMAND_SPECS), {
            "device.health", "lighting.set_rgb", "lighting.set_white", "lighting.off", "lighting.cycle", "lighting.strobe",
            "display.write", "display.clear", "display.power", "display.dashboard",
            "display.brightness", "display.responses", "show.schema", "show.validate",
            "show.play", "show.stop", "show.status",
        })
        for schema in COMMAND_SPECS.values():
            self.assertIn("required", schema)
            self.assertIn("optional", schema)

    def test_led_channels_and_screen_power(self):
        self.assertEqual(prepare("lighting.set_rgb", {"red": 100, "green": 0, "blue": 50}), ["RGB 100 0 50"])
        self.assertEqual(prepare("lighting.set_white", {"warm": 75, "cool": 25}), ["WHITE 75 25"])
        self.assertEqual(prepare("display.power", {"on": False}), ["OLED OFF"])

    def test_validation_precedes_any_usb_writes(self):
        transport = RecordingTransport()
        executor = NumakerExecutor(transport)
        for invalid in (-1, 101, True, "20", 20.0, None):
            result = executor.run("lighting.set_rgb", {"red": invalid, "green": 0, "blue": 0})
            self.assertFalse(result["ok"])
        for params in ({"text": "a" * 22, "row": 7}, {"text": "a\rb"},
                       {"text": "snowman \u2603"}, {"text": "abc", "clear": "true"},
                       {"text": "abc", "row": True}, {"text": "a\n" * 9}):
            self.assertFalse(executor.run("display.write", params)["ok"])
        self.assertEqual(transport.commands, [])

    def test_display_wrapping_and_rows(self):
        self.assertEqual(prepare("display.write", {"text": "a" * 22, "row": 2}),
                         ["CLEAR", "TEXT 2 " + "a" * 21, "TEXT 3 a"])
        self.assertEqual(prepare("display.write", {"text": "one\ntwo", "clear": False}),
                         ["TEXT 0 one", "TEXT 1 two"])
        self.assertEqual(len(prepare("display.write", {"text": "a" * 168})), 9)

    def test_unknown_missing_and_extra_parameters(self):
        executor = NumakerExecutor(RecordingTransport())
        self.assertFalse(executor.run("system.run", {"command": "anything"})["ok"])
        self.assertFalse(executor.run("lighting.set_white", {"warm": 1})["ok"])
        self.assertFalse(executor.run("lighting.off", {"extra": 1})["ok"])
        self.assertFalse(executor.run("device.health", [])["ok"])

    def test_health_is_board_state(self):
        transport = RecordingTransport()
        result = NumakerExecutor(transport).run("device.health", {})
        self.assertEqual(transport.commands, [["STATUS"]])
        self.assertTrue(result["payload"]["online"])
        self.assertEqual(result["payload"]["oled"]["address"], "0x3C")
        self.assertEqual(result["payload"]["rgb"]["red"], 20)

    def test_native_cycle_bounds_and_status(self):
        self.assertEqual(prepare("lighting.cycle", {"step_us":50,"brightness":70,"white_brightness":20}), ["CYCLE 50 70 20"])
        for value in (0, 49, 51, 60000050, True, "50"):
            with self.assertRaises(ValueError):
                prepare("lighting.cycle", {"step_us":value})
        status = parse_status(STATUS + " CYCLE_US 50 STEPS 20000")
        self.assertEqual(status["cycle"], {"active":True,"step_us":50,"steps":20000})

    def test_dt57_lamp_status_is_distinct_from_lighting_channels(self):
        state = parse_status(STATUS + " LAMP 30 PWM_HZ 20000")
        self.assertEqual(state["lamp"], {"brightness": 30, "pwm_hz": 20000, "pin": "PA6"})
        self.assertEqual(state["white"], {"warm": 10, "cool": 10})
        self.assertNotIn("lamp", parse_status(STATUS))

    def test_dt57_rejects_invalid_reported_lamp_settings(self):
        for tail in (" LAMP 101 PWM_HZ 20000", " LAMP 10 PWM_HZ 0", " LAMP 10 PWM_HZ 200"):
            with self.subTest(tail=tail), self.assertRaises(BridgeError):
                parse_status(STATUS + tail)

    def test_dt57_relay_polarities_and_electrical_readback(self):
        base = STATUS + " LAMP 10 PWM_HZ 20000"
        for active_level, polarity in ((0, "LOW"), (1, "HIGH")):
            for on, label in ((False, "OFF"), (True, "ON")):
                level = active_level if on else 1 - active_level
                tail = f" RELAY {label} PIN PC2 ACTIVE {polarity} LEVEL "
                with self.subTest(active_level=active_level, on=on):
                    state = parse_status(base + tail + str(level))
                    self.assertEqual(state["relay"], {"on": on, "pin": "PC2",
                                                     "active_level": active_level, "pin_level": level})
                    self.assertEqual(state["lamp"]["brightness"], 10)
                    with self.assertRaises(BridgeError):
                        parse_status(base + tail + str(1 - level))
        self.assertNotIn("relay", parse_status(base))


class TransportTests(unittest.TestCase):
    def test_parameters_go_on_stdin(self):
        invocations = []

        def runner(args, **kwargs):
            invocations.append((args, kwargs))
            return subprocess.CompletedProcess(args, 0, json.dumps({"ok": True, "responses": [STATUS]}), "")

        state = WindowsSerial(runner=runner).send(["TEXT 0 $(literal) `text`"])
        args, kwargs = invocations[0]
        self.assertNotIn("TEXT 0 $(literal) `text`", args)
        self.assertEqual(json.loads(kwargs["input"])["commands"], ["TEXT 0 $(literal) `text`"])
        self.assertEqual(state["white"], {"warm": 10, "cool": 10})

    def test_response_count_and_invalid_json_fail(self):
        for reply in ("garbage", json.dumps({"ok": True, "responses": []}),
                      json.dumps({"ok": False, "error": "Board disconnected"})):
            def runner(args, **kwargs):
                return subprocess.CompletedProcess(args, 0, reply, "")
            with self.assertRaises(BridgeError):
                WindowsSerial(runner=runner).send(["STATUS"])

    def test_timeout_releases_usb_lock(self):
        count = 0

        def runner(args, **kwargs):
            nonlocal count
            count += 1
            if count == 1:
                raise subprocess.TimeoutExpired(args, 1)
            return subprocess.CompletedProcess(args, 0, json.dumps({"ok": True, "responses": [STATUS]}), "")

        transport = WindowsSerial(runner=runner)
        with self.assertRaises(BridgeError):
            transport.send(["STATUS"])
        self.assertTrue(transport.send(["STATUS"])["oled"]["on"])

    def test_bad_board_status_is_not_reported_as_online(self):
        for reply in ("OK", STATUS.replace("RGB 20", "RGB 101"), "ERR rejected"):
            with self.assertRaises(BridgeError):
                parse_status(reply)
        self.assertFalse(parse_status(STATUS.replace("OLED ON", "OLED NOT_FOUND"))["oled"]["detected"])


if __name__ == "__main__":
    unittest.main()
