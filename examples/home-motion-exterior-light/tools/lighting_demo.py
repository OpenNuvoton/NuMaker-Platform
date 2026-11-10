"""Run the RGB and white board locally over the Windows serial bridge."""
import argparse
from pathlib import Path
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from muse_bridge.executor import WindowsSerial
from muse_bridge.palette import strobe_commands
from muse_bridge.show import ShowController, compile_show

ASSETS = ROOT / "assets"


def prepare_oled_assets():
    build = ROOT / "build"
    build.mkdir(exist_ok=True)
    for name in ("oled-splash.bin", "oled-com5-description.bin", "oled-sensor-description.bin"):
        frame = (ASSETS / name).read_bytes()
        if len(frame) != 1024:
            raise ValueError(f"Expected 1024 OLED page bytes in {name}")
        target = build / name
        temporary = target.with_suffix(".tmp")
        temporary.write_bytes(frame)
        temporary.replace(target)


def play_show(transport):
    program = compile_show((ASSETS / "nuvoton-winbond-com3.xml").read_text())
    controller = ShowController(transport, start_worker=False)
    controller.play(program)
    while controller.running:
        controller.step()
        time.sleep(0.1)


def play_alarm(transport, seconds):
    commands = strobe_commands(["red", "off"], step_us=250_000, brightness=70,
                               white_palette=[{"warm": 0, "cool": 20}, {"warm": 0, "cool": 0}])
    transport.send(commands)
    time.sleep(seconds)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM3")
    actions = parser.add_subparsers(dest="action", required=True)
    actions.add_parser("prepare", help="install OLED frames for the two sensor/control workers")
    actions.add_parser("show", help="rotate the logo and lighting description")
    alarm = actions.add_parser("alarm", help="flash the RGB and white LEDs together")
    alarm.add_argument("--seconds", type=int, choices=range(1, 61), default=10, metavar="1..60")
    actions.add_parser("off", help="stop the LEDs on the lighting board")
    args = parser.parse_args()
    if args.action == "prepare":
        prepare_oled_assets()
        return

    transport = WindowsSerial(args.port)
    try:
        try:
            if args.action == "show":
                play_show(transport)
            elif args.action == "alarm":
                play_alarm(transport, args.seconds)
        except KeyboardInterrupt:
            pass
        finally:
            transport.send(["OFF"])
    finally:
        transport.close()


if __name__ == "__main__":
    main()
