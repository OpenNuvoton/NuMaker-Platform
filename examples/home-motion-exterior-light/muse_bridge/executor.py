"""Board-specific Muse commands and serialized Windows USB transport."""
from __future__ import annotations

import json
import os
import queue
from pathlib import Path
import re
import socket
import subprocess
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
DISPLAY_NAME = "NuMaker DALI Lighting (USB via laptop)"
POWERSHELL = "/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe"


def field(kind, description):
    return {"type": kind, "description": description}


def spec(description, required=None, optional=None):
    return {"description": description, "required": required or {},
            "optional": optional or {}, "timeout_ms": 20000}


COMMAND_SPECS = {
    "device.health": spec("Read the real USB-connected NuMaker lighting board status: RGB and warm/cool white percentages, OLED power and connectivity. The laptop bridges COM3 to the NDA102SD2 board."),
    "lighting.set_rgb": spec("Set the board's RGB LED color using red, green and blue brightness percentages, each 0..100. White LED settings are independent.", {
        c: field("integer", f"{c.capitalize()} brightness, 0..100 percent")
        for c in ("red", "green", "blue")}, {"brightness": field("integer", "Overall RGB brightness 0..100; default 100")}),
    "lighting.set_white": spec("Set the board's tunable white LED with independent warm and cool brightness percentages, each 0..100. Use warm=100,cool=0 for warm white or warm=0,cool=100 for cool white.", {
        c: field("integer", f"{c.capitalize()} white brightness, 0..100 percent")
        for c in ("warm", "cool")}, {"brightness": field("integer", "Overall white brightness 0..100; default 100")}),
    "lighting.off": spec("Turn off both lighting LEDs, leaving the OLED screen available."),
    "lighting.cycle": spec("Run a six-color RGB cycle and alternating warm/cool white on the MCU, independently of USB/OLED updates. 50 microseconds per color is the maximum configured speed (20000 color steps/second). Fast settings visually blend colors. Manual RGB/white, a new show, or lighting.off cancels cycling.", {
        "step_us": field("integer", "Microseconds per color: 50..60000000, in multiples of 50")}, {
        "brightness": field("integer", "RGB brightness 0..100, default 50"),
        "white_brightness": field("integer", "Warm/cool white brightness 0..100, default 20")}),
    "lighting.strobe": spec('Strobe ONLY the user-chosen colors, in order, at native MCU speed. Use this for fast red/yellow, blue/off, or any custom palette; use lighting.cycle only for the fixed six-color rainbow. Example: colors=["red","yellow"], step_us=100000, brightness=70. Default 100 ms/color is fast and visibly distinct. Minimum step_us=50 is about 20000 updates/sec and appears blended. Use lighting.off to stop. Muse OLED reply paging continues independently.', {
        "colors": {"type":"array", "items":{"type":"string"}, "description":'Ordered palette of 1..32 RGB names or hex colors, e.g. ["red","yellow"] or ["#0000FF","off"]. off is black.'}}, {
        "step_us": field("integer", "Microseconds per color, multiples of 50; default 100000 (100 ms). Range 50..60000000."),
        "brightness": field("integer", "RGB brightness percentage 0..100; default 70"),
        "white_warm": field("integer", "Constant warm-white brightness 0..100; default 20"),
        "white_cool": field("integer", "Constant cool-white brightness 0..100; default 0"),
        "white_palette": {"type":"array", "items":{"type":"object"}, "description":'Optional per-step warm/cool objects, one per RGB color, e.g. [{"warm":20,"cool":0},{"warm":0,"cool":20}]. Replaces constant white settings.'}}),
    "display.write": spec("Write text on the board's monochrome OLED: 8 rows of 21 printable ASCII characters. Longer lines wrap. At most 168 characters; row plus wrapped rows must fit. This replaces the status dashboard with custom text.", {
        "text": field("string", "Printable ASCII text; newlines and wrapping are supported")}, {
        "row": field("integer", "First row, 0..7; default 0"),
        "clear": field("boolean", "Clear the screen first; default true")}),
    "display.clear": spec("Clear all text from the board's OLED screen."),
    "display.power": spec("Turn the board's OLED on or off, independently of both LEDs.", {
        "on": field("boolean", "True turns the screen on; false turns it off")}),
    "display.dashboard": spec("Restore the board's built-in RGB and white LED status dashboard on the OLED."),
    "display.brightness": spec("Set monochrome OLED brightness using its contrast register. This changes brightness, not pixel colors.", {
        "brightness": field("integer", "OLED brightness 0..100 percent")}),
    "display.responses": spec("Enable or disable automatic display of completed Muse replies on the OLED. Replies wrap and page while a light show keeps running.", {
        "enabled": field("boolean", "Whether to print Muse replies on the OLED")}, {
        "page_ms": field("integer", "Time per reply page: 1000..60000 ms, default 4000"),
        "hold_ms": field("integer", "Hold final page before resuming a show's graphics: 1000..60000 ms, default 10000")}),
    "show.schema": spec("Read the complete NuMaker light-show XSD, a working XML example, limits and real hardware capabilities. OLED is 128x64 monochrome; one bulb is RGB and the other is tunable white."),
    "show.validate": spec("Validate and pre-render an entire show XML without changing the board.", {
        "xml": field("string", "Version-1 <show> XML; get show.schema for the full grammar")}),
    "show.play": spec('Replace the running show with fully validated version-1 XML. '
        'Root <show version="1" name="..." repeat="1|forever" end="hold|off">. '
        'Optional <responses enabled="true" page-ms="4000" hold-ms="10000"/>. '
        '1..64 <scene duration-ms="1000..60000" fade-ms="0..duration"> in sequence. '
        'Children in order: <rgb color="#RRGGBB" brightness="0..100"/> or RGB hue="0..360" saturation="0..100"; '
        '<white temperature="0..100" brightness="0..100"/> or warm/cool percentages; '
        'or instead of rgb/white use <cycle step-us="50" brightness="50" white-brightness="20"/> for native maximum-speed cycling (step-us multiples of 50). '
        'For custom fast colors use <strobe step-us="100000" brightness="70"><color value="red" warm="20" cool="0"/><color value="yellow" warm="20" cool="0"/></strobe> instead of rgb/white/cycle/fade. '
        '<oled on="true" contrast="0..255" invert="false" background="black|white"> with text(x,y,size,color), '
        'rect(x,y,width,height,fill,color), circle(x,y,radius,fill,color), line(x1,y1,x2,y2,color), pixel(x,y,color), '
        'bitmap(x,y,width,height,encoding=hex|base64; row-major MSB-first data). '
        'OLED colors are only black/white. Light channels inherit when omitted. Native smooth fades; completed Muse replies override only OLED temporarily. '
        'Get show.schema for the complete XSD and example.', {
        "xml": field("string", "Complete version-1 <show> XML, at most 65536 UTF-8 bytes")}),
    "show.stop": spec("Stop the current light show, optionally turning both bulbs off. Reply display remains enabled independently.", optional={
        "off": field("boolean", "Turn both bulbs off; default true")}),
    "show.status": spec("Read current show, scene, repeat iteration, USB rendering duration, errors and Muse reply display state."),
}


class BridgeError(Exception):
    pass


def parse_status(reply):
    match = re.fullmatch(
        r"OK RGB (\d+) (\d+) (\d+) WHITE (\d+) (\d+) "
        r"OLED (ON|OFF|NOT_FOUND) ADDR (\d+) MODEL (SSD1306|SH1106)"
        r"(?: CYCLE_US (\d+) STEPS (\d+)(?: PALETTE (\d+))?)?"
        r"(?: LAMP (\d+) PWM_HZ (\d+)"
        r"(?: RELAY (ON|OFF) PIN PC2 ACTIVE (HIGH|LOW) LEVEL ([01]))?)?", reply)
    if not match:
        raise BridgeError("The board returned an unexpected status response")
    red, green, blue, warm, cool = map(int, match.groups()[:5])
    if any(v > 100 for v in (red, green, blue, warm, cool)):
        raise BridgeError("The board reported an invalid brightness value")
    result = {
        "rgb": {"red": red, "green": green, "blue": blue},
        "white": {"warm": warm, "cool": cool},
        "oled": {"on": match[6] == "ON", "detected": match[6] != "NOT_FOUND",
                 "address": f"0x{int(match[7]):02X}", "model": match[8]},
    }
    if match[9] is not None:
        result["cycle"] = {"step_us": int(match[9]), "steps": int(match[10]), "active": True}
        if match[11] is not None:
            result["cycle"]["palette_size"] = int(match[11])
    if match[12] is not None:
        brightness, pwm_hz = int(match[12]), int(match[13])
        if brightness > 100 or pwm_hz != 20000:
            raise BridgeError("The board reported invalid lamp PWM settings")
        result["lamp"] = {"brightness": brightness, "pwm_hz": pwm_hz, "pin": "PA6"}
    if match[14] is not None:
        on, active_level, pin_level = match[14] == "ON", int(match[15] == "HIGH"), int(match[16])
        if pin_level != (active_level if on else 1 - active_level):
            raise BridgeError("Relay PC2 readback does not match the commanded state")
        result["relay"] = {"on": on, "pin": "PC2", "active_level": active_level,
                           "pin_level": pin_level}
    return result


class WindowsSerial:
    def __init__(self, port="COM3", runner=None):
        if not re.fullmatch(r"COM[1-9][0-9]*", port, re.IGNORECASE):
            raise ValueError("Expected a Windows serial port such as COM3")
        self.port = port.upper()
        self._runner = runner
        self._lock = threading.Lock()
        self._process = None
        self._windows_pid = None
        self._response_queue = None
        self._request_id = 0

    @staticmethod
    def interop_env():
        env = os.environ.copy()
        current = env.get("WSL_INTEROP")
        if current and Path(current).exists():
            return env
        candidates = sorted(Path("/run/WSL").glob("*_interop"),
                            key=lambda p: p.stat().st_mtime, reverse=True)
        if candidates:
            env["WSL_INTEROP"] = str(candidates[0])
        return env

    def send(self, commands, timeout_ms=None):
        commands = list(commands)
        if not 1 <= len(commands) <= 64:
            raise BridgeError("Expected between one and sixty-four board commands")
        for command in commands:
            if (not isinstance(command, str) or not command or len(command) > 127
                    or any(not 32 <= ord(c) <= 126 for c in command)):
                raise BridgeError("Invalid serial command")
        budget = min(20.0, max(0.1, (timeout_ms or 20000) / 1000))
        deadline = time.monotonic() + budget
        if not self._lock.acquire(timeout=budget):
            raise BridgeError("The board is busy; retry the command")
        try:
            if self._runner is None:
                response = self._request(commands, max(0.1, deadline-time.monotonic()))
                return self._decode(response, len(commands))
            script = ROOT / "tools/bridge-serial.ps1"
            # No shell evaluates device parameters. All board commands go on stdin.
            if os.name == "nt":
                script_path = str(script)
                executable = "powershell.exe"
            else:
                distro = os.environ.get("WSL_DISTRO_NAME", "Ubuntu")
                script_path = "\\\\wsl.localhost\\" + distro + str(script).replace("/", "\\")
                executable = POWERSHELL
            result = self._runner([
                executable, "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                "-File", script_path, "-Port", self.port,
            ], input=json.dumps({"commands": commands}) + "\n", text=True,
                capture_output=True, env=self.interop_env(),
                timeout=max(0.1, deadline - time.monotonic()))
            try:
                response = json.loads(result.stdout.strip().lstrip("\ufeff"))
            except (ValueError, AttributeError):
                raise BridgeError("The Windows USB bridge did not return a valid response") from None
            if result.returncode or not response.get("ok"):
                raise BridgeError(response.get("error") or "USB bridge failed")
            return self._decode(response, len(commands))
        except subprocess.TimeoutExpired:
            raise BridgeError("USB communication timed out; check the board cable and VCOM switches") from None
        except OSError:
            raise BridgeError("The Windows USB bridge is unavailable") from None
        finally:
            self._lock.release()

    @staticmethod
    def _decode(response, count):
        if not isinstance(response, dict) or not response.get("ok"):
            raise BridgeError(response.get("error") if isinstance(response, dict) else "Invalid USB reply")
        replies = response.get("responses")
        if not isinstance(replies, list) or len(replies) != count:
            raise BridgeError("The board did not acknowledge every command")
        if any(not isinstance(reply, str) or not reply.startswith("OK ") for reply in replies):
            raise BridgeError("The board rejected a command")
        return parse_status(replies[-1])

    @staticmethod
    def _read_worker(process, output):
        try:
            while line := process.stdout.readline(65536):
                try:
                    output.put(json.loads(line.strip().lstrip("\ufeff")))
                except ValueError:
                    output.put({"_error": "Invalid response from Windows serial worker"})
        finally:
            output.put({"_error": "Windows serial worker disconnected"})

    def _start_worker(self):
        if self._process is not None and self._process.poll() is None:
            return
        distro = os.environ.get("WSL_DISTRO_NAME", "Ubuntu")
        script = "\\\\wsl.localhost\\" + distro + str(ROOT / "tools/serial-worker.ps1").replace("/", "\\")
        self._response_queue = queue.Queue()
        self._windows_pid = None
        self._process = subprocess.Popen([
            POWERSHELL, "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
            "-File", script, "-Port", self.port,
        ], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            text=True, bufsize=1, env=self.interop_env())
        threading.Thread(target=self._read_worker, args=(self._process, self._response_queue), daemon=True).start()

    def _request(self, commands, timeout, *, read_timeout_ms=3000):
        if type(read_timeout_ms) is not int or not 100 <= read_timeout_ms <= 20000:
            raise ValueError("Serial read timeout must be 100..20000 ms")
        deadline = time.monotonic() + timeout
        self._start_worker()
        self._request_id += 1
        request_id = self._request_id
        self._process.stdin.write(json.dumps({"id": request_id, "commands": commands,
                                             "read_timeout_ms": read_timeout_ms}) + "\n")
        self._process.stdin.flush()
        try:
            while True:
                reply = self._response_queue.get(timeout=max(0.01, deadline-time.monotonic()))
                if isinstance(reply, dict) and reply.get("ready"):
                    self._windows_pid = reply.get("pid")
                    continue
                if not isinstance(reply, dict) or reply.get("_error"):
                    message = reply.get("_error", "Invalid serial worker response") if isinstance(reply, dict) else "Invalid serial worker response"
                    self._close_worker(force=True)
                    raise BridgeError(message)
                if reply.get("id") != request_id:
                    self._close_worker(force=True)
                    raise BridgeError("USB response did not match its request")
                return reply
        except queue.Empty:
            self._close_worker(force=True)
            raise BridgeError("USB communication timed out; check the board cable and VCOM switches") from None

    def _close_worker(self, force=False):
        process = self._process
        if process is None:
            return
        try:
            if process.poll() is None and not force:
                process.stdin.write('{"quit":true}\n')
                process.stdin.flush()
                process.stdin.close()
                process.wait(timeout=3)
        except (OSError, subprocess.TimeoutExpired):
            force = True
        if force and process.poll() is None:
            if type(self._windows_pid) is int:
                try:
                    subprocess.run(["/mnt/c/Windows/System32/taskkill.exe", "/PID", str(self._windows_pid), "/T", "/F"],
                                   capture_output=True, timeout=5, env=self.interop_env())
                except (OSError, subprocess.TimeoutExpired):
                    pass
            process.kill()
            process.wait(timeout=3)
        self._process = None
        self._windows_pid = None

    def close(self):
        with self._lock:
            self._close_worker()


def percentage(params, key):
    value = params.get(key)
    if type(value) is not int or not 0 <= value <= 100:
        raise ValueError(f"{key} must be an integer from 0 to 100")
    return value


def prepare(command, params):
    """Validate the entire request before touching the board."""
    if command not in COMMAND_SPECS:
        raise ValueError(f"Unsupported board command: {command}")
    if not isinstance(params, dict):
        raise ValueError("Command parameters must be an object")
    schema = COMMAND_SPECS[command]
    allowed = schema["required"].keys() | schema["optional"].keys()
    if params.keys() - allowed:
        raise ValueError("Unexpected command parameters")
    if schema["required"].keys() - params.keys():
        raise ValueError("Missing required command parameters")
    if command == "device.health":
        return ["STATUS"]
    if command == "lighting.set_rgb":
        gain = percentage(params, "brightness") if "brightness" in params else 100
        return ["RGB " + " ".join(str(round(percentage(params, c)*gain/100)) for c in ("red", "green", "blue"))]
    if command == "lighting.set_white":
        gain = percentage(params, "brightness") if "brightness" in params else 100
        return ["WHITE " + " ".join(str(round(percentage(params, c)*gain/100)) for c in ("warm", "cool"))]
    if command == "lighting.off":
        return ["OFF"]
    if command == "lighting.strobe":
        from .palette import strobe_commands
        return strobe_commands(**params)
    if command == "lighting.cycle":
        step = params["step_us"]
        if type(step) is not int or not 50 <= step <= 60000000 or step % 50:
            raise ValueError("step_us must be a multiple of 50 from 50 to 60000000")
        brightness = percentage(params, "brightness") if "brightness" in params else 50
        white = percentage(params, "white_brightness") if "white_brightness" in params else 20
        return [f"CYCLE {step} {brightness} {white}"]
    if command == "display.clear":
        return ["CLEAR"]
    if command == "display.dashboard":
        return ["DASHBOARD"]
    if command == "display.brightness":
        return ["OLED CONTRAST " + str(round(percentage(params, "brightness")*255/100))]
    if command == "display.power":
        if type(params["on"]) is not bool:
            raise ValueError("on must be a boolean")
        return ["OLED ON" if params["on"] else "OLED OFF"]
    text, row, clear = params["text"], params.get("row", 0), params.get("clear", True)
    if not isinstance(text, str) or len(text) > 168:
        raise ValueError("text must be a string of at most 168 characters")
    if any(c != "\n" and not 32 <= ord(c) <= 126 for c in text):
        raise ValueError("Use printable ASCII text and newlines")
    if type(row) is not int or not 0 <= row <= 7:
        raise ValueError("row must be an integer from 0 to 7")
    if type(clear) is not bool:
        raise ValueError("clear must be a boolean")
    lines = []
    for line in text.split("\n"):
        lines.extend([line[i:i+21] for i in range(0, len(line), 21)] or [""])
    if row + len(lines) > 8:
        raise ValueError("The text does not fit on the OLED from the requested row")
    return (["CLEAR"] if clear else []) + [f"TEXT {row+i} {line}" for i, line in enumerate(lines)]


class NumakerExecutor:
    def __init__(self, transport=None, controller=None):
        self.transport = transport or WindowsSerial(os.environ.get("NUMAKER_PORT", "COM3"))
        self.controller = controller

    def _special(self, command, params):
        from .show import compile_show, schema_payload
        if command == "show.schema":
            return schema_payload()
        if command == "show.validate":
            return compile_show(params["xml"]).summary()
        if self.controller is None:
            raise ValueError("Show controller is unavailable")
        if command == "show.play":
            return self.controller.play(compile_show(params["xml"]))
        with self.controller.lock:
            if command == "show.stop":
                off = params.get("off", True)
                if type(off) is not bool:
                    raise ValueError("off must be a boolean")
                return self.controller.stop(off)
            if command == "show.status":
                return self.controller.status()
            if command == "display.responses":
                return self.controller.configure(params)
        raise ValueError("Unknown show command")

    def run(self, command, params, timeout_ms=None):
        try:
            if not isinstance(command, str):
                raise ValueError("Command must be a string")
            if command.startswith("show.") or command == "display.responses":
                if not isinstance(params, dict):
                    raise ValueError("Command parameters must be an object")
                schema = COMMAND_SPECS.get(command)
                if not schema or params.keys() - (schema["required"].keys() | schema["optional"].keys()) or schema["required"].keys() - params.keys():
                    raise ValueError("Invalid show command parameters")
                return {"ok": True, "payload": self._special(command, params)}
            lines = prepare(command, params)
            if self.controller:
                with self.controller.lock:
                    if command != "device.health":
                        self.controller.stop(off=False)
                        if command.startswith("display."):
                            self.controller.reply_active = False
                            self.controller.pages = []
                    state = self.transport.send(lines, timeout_ms)
            else:
                state = self.transport.send(lines, timeout_ms)
            payload = {
                "device": DISPLAY_NAME, "online": True, "bridge_host": socket.gethostname(),
                "transport": "USB serial via Windows", "port": self.transport.port,
                **state,
            }
            if command == "display.write":
                payload["text"] = params["text"]
                payload["first_row"] = params.get("row", 0)
            if command == "lighting.strobe":
                payload["colors"] = params["colors"]
            return {"ok": True, "payload": payload}
        except (ValueError, BridgeError) as exc:
            return {"ok": False, "error": str(exc)}
