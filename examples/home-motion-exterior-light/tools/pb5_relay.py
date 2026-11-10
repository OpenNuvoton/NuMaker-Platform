"""Control bulb 208 from PIR motion and bulb 206 from ambient lux."""
from __future__ import annotations

import argparse
from collections import deque
from datetime import datetime, timezone
import fcntl
import json
import math
import os
from pathlib import Path
import signal
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from muse_bridge.executor import WindowsSerial
from muse_bridge.graphics import frame_commands

BUILD = ROOT / "build"
STATE = BUILD / "pb5-relay-state.json"
PIDFILE = BUILD / "pb5-relay.pid"
SOURCE = BUILD / "sensor-live-latest.json"
OLED_SPLASH = BUILD / "oled-splash.bin"
OLED_DESCRIPTION = BUILD / "oled-com5-description.bin"
OLED_LABELS = ["TEXT 0 208 PIR / 206 DIMMER", "TEXT 1 MIN ON: 30 seconds",
               "TEXT 2 ON>1800 OFF<100"]
MINIMUM_ON_SECONDS = 30
SAMPLE_TIMEOUT_SECONDS = 1
DIMMER_HALF_LEVEL_LUX = 400
DIMMER_UPDATE_SECONDS = 0.5


def desired_state(value, current):
    if type(value) is not int or not 0 <= value <= 4095:
        raise ValueError("PB5 must be a raw 12-bit ADC value")
    if value > 1800:
        return True
    if value < 100:
        return False
    return current


class SecurityLight:
    """Retriggerable minimum ON time layered over the PIR ADC hysteresis."""
    def __init__(self):
        self.hold_until = 0.0

    def confirmed_on(self, now):
        self.hold_until = max(self.hold_until, now + MINIMUM_ON_SECONDS)

    def update(self, value, current, now):
        target = False if value is None else desired_state(value, current)
        if value is not None and value > 1800:
            self.confirmed_on(now)
        return target or (current and now < self.hold_until)

    def remaining(self, now):
        return max(0.0, self.hold_until - now)


def dimmer_target(lux):
    return round(100 * DIMMER_HALF_LEVEL_LUX / (DIMMER_HALF_LEVEL_LUX + lux))


def dimmer_step(lux, current):
    target = dimmer_target(lux)
    step = round((target - current) / 4)
    return current + step if step else target


def ambient_lux(sample):
    ambient = sample.get("ambient")
    if (sample.get("ambient_error") or not isinstance(ambient, dict)
            or ambient.get("valid") is not True or ambient.get("saturated") is not False):
        raise ValueError("Waiting for a valid, unsaturated ambient reading")
    lux = ambient.get("lux")
    if type(lux) not in (int, float) or lux < 0 or not math.isfinite(lux):
        raise ValueError("Invalid ambient lux")
    # Both timestamps come from the Windows worker's clock.
    saved = datetime.fromisoformat(sample["updated_utc"])
    measured = datetime.fromisoformat(ambient["updated_utc"])
    if not 0 <= (saved - measured).total_seconds() <= SAMPLE_TIMEOUT_SECONDS:
        raise ValueError("Ambient reading is stale")
    return lux


class SensorFeed:
    """Use sample progress as a heartbeat, avoiding Windows/WSL clock skew."""
    def __init__(self, path=SOURCE):
        self.path = path
        self.stamp = None
        self.last_change = None
        self.updates = 0

    def read(self, now):
        data = json.loads(self.path.read_text())
        if data.get("status") != "online":
            raise ValueError("Sensor monitor is not online")
        stamp = data.get("updated_utc")
        if not isinstance(stamp, str) or not stamp:
            raise ValueError("Sensor sample has no timestamp")
        if stamp != self.stamp:
            self.stamp, self.last_change = stamp, now
            self.updates += 1
        if self.updates < 2:
            raise ValueError("Waiting for a new sensor sample")
        if now - self.last_change > SAMPLE_TIMEOUT_SECONDS:
            raise ValueError("Sensor samples stopped advancing")
        return data


def splash_page(frame, page):
    start = 128 * page
    end = start + 128
    return [f"BUFFER {offset} {frame[offset:min(offset + 48, end)].hex()}"
            for offset in range(start, end, 48)] + ["PRESENT"]


def oled_frame():
    try:
        logo = OLED_SPLASH.read_bytes()
    except FileNotFoundError:
        return None, None
    if time.time() % 10 >= 4:
        try:
            description = OLED_DESCRIPTION.read_bytes()
        except FileNotFoundError:
            pass
        else:
            if len(description) == 1024:
                return description, "description"
    return logo, "logo"


def apply_relay(transport, on, on_level, splash=None):
    before = transport.send(["STATUS"], timeout_ms=5000)
    if "lamp" not in before or before.get("relay", {}).get("pin") != "PC2":
        raise RuntimeError("Expected DT5-7 relay firmware on COM5")
    target = on_level if on else 1 - on_level
    command = "RELAY ON" if target == before["relay"]["active_level"] else "RELAY OFF"
    physical = "ON" if on else "OFF"
    display = splash_page(splash, 4) if splash else [f"TEXT 4 Relay physical: {physical}"]
    after = transport.send([command, *display, "STATUS"], timeout_ms=5000)
    if after["relay"]["pin_level"] != target or after["lamp"] != before["lamp"]:
        raise RuntimeError("Relay readback or independent 206 check failed")
    return after


def apply_dimmer(transport, level, relay_pin_level, splash=None):
    commands = [f"LAMP {level}"]
    if splash:
        commands.extend(splash_page(splash, 6))
    after = transport.send(commands, timeout_ms=5000)
    if after["lamp"]["brightness"] != level or after["relay"]["pin_level"] != relay_pin_level:
        raise RuntimeError("Dimmer readback or independent 208 check failed")
    return after


def save_state(data):
    data = {**data, "pid": os.getpid(), "updated_utc": datetime.now(timezone.utc).isoformat()}
    temporary = STATE.with_suffix(".tmp")
    temporary.write_text(json.dumps(data, indent=2) + "\n")
    temporary.replace(STATE)


def live_pid():
    try:
        pid = int(PIDFILE.read_text())
        args = Path(f"/proc/{pid}/cmdline").read_bytes().split(b"\0")
        if str(Path(__file__).resolve()).encode() in args and b"--run" in args:
            return pid
    except (OSError, ValueError):
        pass
    return None


def run():
    lock = (BUILD / "pb5-relay.lock").open("w")
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    PIDFILE.write_text(str(os.getpid()))
    stop = threading.Event()
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda *_: stop.set())
    calibration_path = BUILD / "dt57-relay-calibration.json"
    if not calibration_path.is_file():
        calibration_path = ROOT / "config" / "dt57-relay.json"
    calibration = json.loads(calibration_path.read_text())
    on_level = calibration["physical_on_level"]
    if type(on_level) is not int or on_level not in (0, 1):
        raise ValueError("Relay physical polarity is not calibrated")
    feed = SensorFeed()
    security_light = SecurityLight()
    transport = WindowsSerial("COM5")
    current = None
    board = None
    transitions = deque(maxlen=50)
    started = time.monotonic()
    last_save = last_check = last_oled = last_dimmer = 0.0
    value = None
    lux = None
    mode = "starting"
    error = None
    ambient_error = None
    splash = None
    oled_page = None
    try:
        while not stop.is_set():
            now = time.monotonic()
            try:
                if current is None:
                    board = transport.send(["STATUS"], timeout_ms=5000)
                    if "lamp" not in board or board.get("relay", {}).get("pin") != "PC2":
                        raise RuntimeError("Expected DT5-7 board on COM5")
                    current = board["relay"]["pin_level"] == on_level
                    if current:
                        security_light.confirmed_on(time.monotonic())
                    transport.send(OLED_LABELS, timeout_ms=5000)
                    splash = None
                    oled_page = None
                    last_oled = 0.0
                try:
                    sample = feed.read(now)
                except (OSError, ValueError, KeyError, TypeError) as exc:
                    value, lux, mode, error = None, None, "waiting_sensor", str(exc)
                    # Ignore a transient partial file write. Require a full second
                    # without sample progress before releasing the relay.
                    last_sample = feed.last_change if feed.last_change is not None else started
                    target = (current if now - last_sample <= SAMPLE_TIMEOUT_SECONDS
                              else security_light.update(None, current, now))
                    ambient_error = str(exc)
                else:
                    try:
                        value = sample["analog"]["PB5"]
                        target = security_light.update(value, current, now)
                    except (ValueError, KeyError, TypeError) as exc:
                        value, mode, error = None, "waiting_sensor", str(exc)
                        target = security_light.update(None, current, now)
                    else:
                        mode, error = "running", None
                    try:
                        lux = ambient_lux(sample)
                    except (ValueError, KeyError, TypeError, OverflowError) as exc:
                        lux, ambient_error = None, str(exc)
                    else:
                        ambient_error = None
                if target != current:
                    board = apply_relay(transport, target, on_level, splash)
                    current = target
                    if current:
                        security_light.confirmed_on(time.monotonic())
                    transitions.append({"utc": datetime.now(timezone.utc).isoformat(),
                                        "ambient_lux": lux, "pb5": value,
                                        "relay": "ON" if current else "OFF"})
                    print(json.dumps(transitions[-1]), flush=True)
                if now - last_check >= 1:
                    board = transport.send(["STATUS"], timeout_ms=5000)
                    actual = board["relay"]["pin_level"] == on_level
                    if actual != current:
                        board = apply_relay(transport, current, on_level, splash)
                    last_check = now
                if lux is not None and now - last_dimmer >= DIMMER_UPDATE_SECONDS:
                    brightness = board["lamp"]["brightness"]
                    next_level = dimmer_step(lux, brightness)
                    if next_level != brightness:
                        board = apply_dimmer(transport, next_level, on_level if current else 1 - on_level, splash)
                    last_dimmer = now
                if now - last_oled >= 0.5:
                    image, page = oled_frame()
                    if image is not None and len(image) != 1024:
                        image, page = splash, oled_page
                    if image is None:
                        commands = []
                        if splash is not None:
                            physical = "ON" if current else "OFF"
                            brightness = board["lamp"]["brightness"]
                            commands = ["CLEAR", *OLED_LABELS, f"TEXT 4 Relay physical: {physical}",
                                        f"TEXT 6 PWM duty: {brightness:03d}%"]
                        text = f"Lux: {lux:.1f}" if lux is not None else "Waiting for lux"
                        motion = f"PB5 ADC: {value}" if value is not None else "PB5 unavailable"
                        hold = security_light.remaining(time.monotonic())
                        commands.extend([f"TEXT 3 Hold: {hold:4.1f}s", f"TEXT 5 {text}", f"TEXT 7 {motion}"])
                        board = transport.send(commands, timeout_ms=5000)
                    elif image != splash:
                        board = transport.send(frame_commands(image), timeout_ms=5000)
                        if not board["oled"]["on"]:
                            raise RuntimeError("DT5-7 splash OLED is unavailable")
                    splash = image
                    oled_page = page
                    last_oled = now
            except Exception as exc:
                error, mode, current = str(exc), "waiting_relay", None
                transport.close()
                stop.wait(0.5)
            if now - last_save >= 0.1:
                save_state({"active": True, "status": mode, "pb5": value,
                            "relay": None if current is None else ("ON" if current else "OFF"),
                            "turn_on_above": 1800, "turn_off_below": 100,
                            "minimum_on_seconds": MINIMUM_ON_SECONDS,
                            "hold_remaining_seconds": round(security_light.remaining(time.monotonic()), 2),
                            "retrigger": "each sample above 1800 renews the 30-second hold",
                            "between_thresholds": "hold", "error": error,
                            "ambient_lux": lux, "ambient_error": ambient_error,
                            "oled_splash": splash is not None,
                            "oled_page": oled_page,
                            "dimmer_target_percent": None if lux is None else dimmer_target(lux),
                            "source": str(SOURCE), "relay_port": "COM5",
                            "relay_physical_on_level": on_level,
                            "lamp": None if board is None else board.get("lamp"),
                            "transitions": list(transitions)})
                last_save = now
            stop.wait(0.02)
    finally:
        transport.close()
        save_state({"active": False, "status": "stopped", "pb5": value,
                    "relay": None if current is None else ("ON" if current else "OFF"),
                    "turn_on_above": 1800, "turn_off_below": 100,
                    "minimum_on_seconds": MINIMUM_ON_SECONDS,
                    "hold_remaining_seconds": round(security_light.remaining(time.monotonic()), 2),
                    "ambient_lux": lux,
                    "transitions": list(transitions)})
        if PIDFILE.exists() and PIDFILE.read_text().strip() == str(os.getpid()):
            PIDFILE.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--start", action="store_true")
    mode.add_argument("--restart", action="store_true")
    mode.add_argument("--run", action="store_true")
    mode.add_argument("--stop", action="store_true")
    mode.add_argument("--status", action="store_true")
    args = parser.parse_args()
    BUILD.mkdir(exist_ok=True)
    if args.run:
        run()
    elif args.start or args.restart:
        pid = live_pid()
        if args.restart and pid is not None:
            os.kill(pid, signal.SIGTERM)
            deadline = time.monotonic() + 10
            while live_pid() is not None and time.monotonic() < deadline:
                time.sleep(0.1)
            if live_pid() is not None:
                raise RuntimeError("Previous relay controller has not stopped; restart cancelled")
            pid = None
        if pid is None:
            with (BUILD / "pb5-relay.log").open("a") as log:
                process = subprocess.Popen([sys.executable, "-u", str(Path(__file__).resolve()), "--run"],
                                           cwd=ROOT, stdout=log, stderr=log, start_new_session=True,
                                           env=WindowsSerial.interop_env())
                PIDFILE.write_text(str(process.pid))
                pid = process.pid
        print(json.dumps({"pid": pid, "state_file": str(STATE)}))
    elif args.stop:
        pid = live_pid()
        if pid is not None:
            os.kill(pid, signal.SIGTERM)
        print(json.dumps({"stopping_pid": pid, "relay": "last state retained"}))
    else:
        print(STATE.read_text() if STATE.exists() else '{"active": false}')


if __name__ == "__main__":
    main()
