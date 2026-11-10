#!/usr/bin/env python3
"""Control the attached NuMaker lighting board through Windows COM3."""
import argparse
import json
from pathlib import Path
import shutil
import socket
import subprocess
import sys


def level(value):
    number = int(value)
    if not 0 <= number <= 100:
        raise argparse.ArgumentTypeError("brightness must be from 0 to 100")
    return number


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM3")
    commands = parser.add_subparsers(dest="action", required=True)
    rgb = commands.add_parser("rgb", help="set red, green and blue percentages")
    for name in ("red", "green", "blue"):
        rgb.add_argument(name, type=level)
    white = commands.add_parser("white", help="set warm and cool white percentages")
    for name in ("warm", "cool"):
        white.add_argument(name, type=level)
    text = commands.add_parser("text", help="write up to 21 ASCII characters on one OLED row")
    text.add_argument("text")
    text.add_argument("--row", type=int, choices=range(8), default=0)
    oled = commands.add_parser("oled", help="switch or reinitialize the OLED")
    oled.add_argument("state", choices=("on", "off", "init"))
    model = commands.add_parser("model", help="select the OLED controller if needed")
    model.add_argument("name", choices=("ssd1306", "sh1106"))
    for name in ("status", "off", "clear", "dashboard", "help"):
        commands.add_parser(name)
    args = parser.parse_args()
    if args.action == "rgb":
        command = f"RGB {args.red} {args.green} {args.blue}"
    elif args.action == "white":
        command = f"WHITE {args.warm} {args.cool}"
    elif args.action == "text":
        if len(args.text) > 21 or any(not 32 <= ord(c) <= 126 for c in args.text):
            parser.error("OLED text must contain at most 21 printable ASCII characters")
        command = f"TEXT {args.row} {args.text}"
    elif args.action == "oled":
        command = "OLED " + args.state.upper()
    elif args.action == "model":
        command = "MODEL " + args.name.upper()
    else:
        command = args.action.upper()
    local_socket = Path(__file__).resolve().parent / "muse_bridge/state/bridge.sock"
    if sys.platform != "win32" and local_socket.exists():
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as conn:
                conn.settimeout(25)
                conn.connect(str(local_socket))
                conn.sendall(json.dumps({"command": "bridge.raw", "port": args.port,
                                         "commands": [command]}).encode() + b"\n")
                result = json.loads(conn.makefile("rb").readline())
            print(json.dumps(result, indent=2))
            return 0 if result.get("ok") else 1
        except (FileNotFoundError, ConnectionRefusedError):
            pass
    script = Path(__file__).resolve().parent / "tools/control.ps1"
    if sys.platform == "win32":
        script_path = str(script)
    elif shutil.which("wslpath"):
        script_path = subprocess.check_output(["wslpath", "-w", str(script)], text=True).strip()
    else:
        parser.error("Run this tool from Windows or WSL with Windows interop enabled")
    result = subprocess.run([
        "powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass",
        "-File", script_path, "-Port", args.port, "-CommandText", command,
    ])
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
