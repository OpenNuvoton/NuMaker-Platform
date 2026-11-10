"""Control the DT5-7 shield's PA6 PWM and confirmed PC2 relay output."""
import argparse
from datetime import datetime, timezone
import json
import re
from pathlib import Path
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from muse_bridge.executor import BridgeError, WindowsSerial


def inputs(transport):
    response = transport._request(["INPUTS"], 5)
    if not response.get("ok"):
        raise BridgeError(response.get("error", "Input read failed"))
    match = re.fullmatch(r"OK INPUTS PA (\d+) PB (\d+) PC (\d+) PD (\d+) PF (\d+)",
                         response["responses"][0])
    if not match:
        raise BridgeError("Unexpected input response")
    return dict(zip(("PA", "PB", "PC", "PD", "PF"), map(int, match.groups())))


def analog_inputs(transport, extra=False, tail=False):
    if extra and tail:
        raise ValueError("Choose the EXTRA or TAIL bank")
    command = "ANALOG TAIL" if tail else ("ANALOG EXTRA" if extra else "ANALOG")
    response = transport._request([command], 5)
    if not response.get("ok"):
        raise BridgeError(response.get("error", "Analog input read failed"))
    reply = response["responses"][0]
    channels = range(14, 16) if tail else (range(8, 12) if extra else range(8))
    pattern = r"OK ANALOG" + "".join(rf" PB{channel} (\d+)" for channel in channels)
    match = re.fullmatch(pattern, reply)
    if not match:
        raise BridgeError("Unexpected analog input response")
    values = dict(zip((f"PB{channel}" for channel in channels), map(int, match.groups())))
    if any(value > 4095 for value in values.values()):
        raise BridgeError("Invalid ADC result")
    return values


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM5")
    sub = parser.add_subparsers(dest="action", required=True)
    sub.add_parser("status")
    lamp = sub.add_parser("lamp")
    lamp.add_argument("brightness", type=int, choices=range(101), metavar="0..100")
    relay = sub.add_parser("relay")
    relay.add_argument("setting", choices=("on", "off", "test"))
    sub.add_parser("demo")
    sub.add_parser("inputs")
    sub.add_parser("analog")
    capture = sub.add_parser("capture")
    capture.add_argument("--seconds", type=int, choices=range(5, 121), default=45)
    args = parser.parse_args()
    transport = WindowsSerial(args.port)
    try:
        state = transport.send(["STATUS"])
        if "lamp" not in state:
            raise BridgeError("This board does not report the DT5-7 PA6 lamp firmware")
        if args.action == "status":
            print(json.dumps(state, indent=2))
        elif args.action == "inputs":
            print(json.dumps(inputs(transport), indent=2))
        elif args.action == "analog":
            print(json.dumps(analog_inputs(transport), indent=2))
        elif args.action == "capture":
            transport.send(["LAMP 100", "TEXT 2 SW8 input mapping", "TEXT 4 Turn SW8 slowly"])
            # Exclude known output and serial pins from the observation mask.
            masks = {"PA": 0xffff ^ (1 << 6), "PB": 0xffff ^ (3 << 12),
                     "PC": 0xfffc, "PD": 0xffff, "PF": 0xffff}
            samples = []
            changed = {port: 0 for port in masks}
            started = time.monotonic()
            print(f"Recording SW8 inputs for {args.seconds} seconds; rotate slowly and pause at several positions.", flush=True)
            while time.monotonic() - started < args.seconds:
                pins = {port: value & masks[port] for port, value in inputs(transport).items()}
                if samples:
                    for port in masks:
                        changed[port] |= pins[port] ^ samples[-1]["pins"][port]
                samples.append({"seconds": round(time.monotonic()-started, 4), "pins": pins})
                time.sleep(0.04)
            candidates = [f"{port}{bit}" for port, mask in changed.items()
                          for bit in range(16) if mask & (1 << bit)]
            report = {"port": args.port, "captured_utc": datetime.now(timezone.utc).isoformat(),
                      "changed_pins": candidates, "samples": samples}
            (ROOT / "build/dt57-sw8-inputs.json").write_text(json.dumps(report, indent=2) + "\n")
            print(json.dumps({"samples": len(samples), "changed_pins": candidates}), flush=True)
        elif args.action == "lamp":
            state = transport.send([f"LAMP {args.brightness}"])
            if state["lamp"]["brightness"] != args.brightness:
                raise BridgeError("Lamp setting was not acknowledged")
            print(json.dumps(state, indent=2))
        elif args.action == "relay":
            if "relay" not in state:
                raise BridgeError("Relay firmware requires a physically verified PC2 active level")
            if args.setting != "test":
                expected = args.setting == "on"
                state = transport.send([f"RELAY {args.setting.upper()}"])
                if state["relay"]["on"] != expected:
                    raise BridgeError("Relay setting was not acknowledged")
                print(json.dumps(state, indent=2))
            else:
                report = {"port": args.port, "tested_utc": datetime.now(timezone.utc).isoformat(),
                          "steps": [], "physical_contact_state_measured": False}
                initial_lamp = state["lamp"]
                try:
                    for on in (False, True, False):
                        state = transport.send([f"RELAY {'ON' if on else 'OFF'}"])
                        if state["relay"]["on"] != on or state["lamp"] != initial_lamp:
                            raise BridgeError("Relay state or independent PWM check failed")
                        report["steps"].append(state)
                        print(f"Relay 1 {'ON' if on else 'OFF'}; PC2 = {state['relay']['pin_level']}", flush=True)
                        time.sleep(3)
                    for invalid in ("RELAY", "RELAY 1", "RELAY ON extra", "DRIVE C 2 1 100", "PROBE C 2 0 100"):
                        try:
                            transport.send([invalid])
                        except BridgeError as error:
                            if "Board rejected command:" not in str(error):
                                raise
                        else:
                            raise BridgeError("Invalid relay or reserved-pin command was accepted")
                    report["invalid_commands_rejected"] = 5
                finally:
                    try:
                        report["final_state"] = transport.send(["RELAY OFF"])
                    finally:
                        (ROOT / "build/dt57-relay-verification.json").write_text(json.dumps(report, indent=2) + "\n")
                print("Relay commands, electrical readback, PWM independence and invalid-command checks passed; relay OFF.")
        else:
            report = {"port": args.port, "tested_utc": datetime.now(timezone.utc).isoformat(),
                      "steps": [], "optical_change_confirmed": False}
            for value in (0, 10, 50, 100, 30):
                state = transport.send([f"LAMP {value}"])
                if state["lamp"]["brightness"] != value or not state["oled"]["detected"]:
                    raise BridgeError("Lamp or OLED check failed")
                report["steps"].append(state)
                print(f"PA6 PWM {value}%", flush=True)
                time.sleep(3)
            for invalid in ("LAMP -1", "LAMP 101", "LAMP 30 extra", "RGB 10 0 0"):
                try:
                    transport.send([invalid])
                except BridgeError as error:
                    if "Board rejected command:" not in str(error):
                        raise
                else:
                    raise BridgeError("Invalid or unsupported command was accepted")
            state = transport.send(["STATUS"])
            if state["lamp"]["brightness"] != 30:
                raise BridgeError("Rejected commands changed the lamp setting")
            report["invalid_commands_rejected"] = 4
            report["final_state"] = state
            (ROOT / "build/dt57-verification.json").write_text(json.dumps(report, indent=2) + "\n")
            print("PWM commands, bounds, and OLED checks passed; final setting 30%.")
    finally:
        transport.close()


if __name__ == "__main__":
    main()
