from pathlib import Path
import unittest

from .executor import NumakerExecutor, parse_status, prepare
from .palette import strobe_commands
from .show import ShowController, compile_show
from .test_executor import RecordingTransport, STATUS


class PaletteTests(unittest.TestCase):
    def test_named_colors_and_black_steps(self):
        commands = prepare("lighting.strobe", {"colors":["red","yellow","blue","off"],"brightness":70})
        self.assertEqual(commands, ["PALETTE CLEAR","PALETTE 0 70 0 0 20 0",
            "PALETTE 1 70 70 0 20 0","PALETTE 2 0 0 70 20 0",
            "PALETTE 3 0 0 0 20 0","STROBE 100000 4"])

    def test_hex_brightness_and_white_palette(self):
        commands = strobe_commands(["#ff0000","#00ffff"],50,50,
                                  white_palette=[{"warm":20,"cool":0},{"warm":0,"cool":30}])
        self.assertEqual(commands[1:3], ["PALETTE 0 50 0 0 20 0","PALETTE 1 0 50 50 0 30"])
        self.assertEqual(commands[-1], "STROBE 50 2")

    def test_invalid_palette_never_touches_running_board(self):
        transport = RecordingTransport()
        executor = NumakerExecutor(transport)
        for params in ({"colors":[]}, {"colors":["red"]*33}, {"colors":"red,yellow"},
                       {"colors":["made-up-color"]}, {"colors":["#ff000080"]},
                       {"colors":[None]}, {"colors":["red"],"step_us":51},
                       {"colors":["red"],"brightness":True},
                       {"colors":["red","blue"],"white_palette":[{"warm":0,"cool":0}]}):
            self.assertFalse(executor.run("lighting.strobe",params)["ok"],params)
        self.assertEqual(transport.commands,[])

    def test_custom_palette_status_and_max_batch(self):
        state = parse_status(STATUS+" CYCLE_US 50 STEPS 1234 PALETTE 32")
        self.assertEqual(state["cycle"]["palette_size"],32)
        self.assertEqual(len(strobe_commands(["red"]*32)),34)

    def test_custom_xml_and_conflicts(self):
        program = compile_show(Path(__file__).with_name("red-yellow-fast.xml").read_text())
        self.assertEqual(program.scenes[0].strobe[-1],"STROBE 100000 2")
        for body in ('<rgb color="#ff0000"/><strobe><color value="red"/></strobe>',
                     '<cycle step-us="50"/><strobe><color value="red"/></strobe>',
                     '<strobe/>', '<strobe step-us="51"><color value="red"/></strobe>'):
            with self.assertRaises(ValueError):
                compile_show('<show version="1"><scene duration-ms="1000">'+body+'</scene></show>')

    def test_playback_uploads_custom_palette_before_graphics(self):
        transport = RecordingTransport()
        controller = ShowController(transport,start_worker=False)
        controller.play(compile_show(Path(__file__).with_name("red-yellow-fast.xml").read_text()))
        controller.step(0)
        commands = transport.commands[0]
        self.assertEqual(commands[:4],["PALETTE CLEAR","PALETTE 0 70 0 0 20 0",
                                      "PALETTE 1 70 70 0 20 0","STROBE 100000 2"])
        self.assertLessEqual(len(commands),64)


if __name__ == "__main__":
    unittest.main()
