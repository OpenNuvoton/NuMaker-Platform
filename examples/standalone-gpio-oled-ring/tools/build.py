"""Build the standalone controller with the installed Arm GCC toolchain."""
from pathlib import Path
import argparse
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
BUILD.mkdir(exist_ok=True)
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--profile", choices=("lighting", "display", "dt57", "sensor"), default="lighting")
parser.add_argument("--relay-active-level", type=int, choices=(0, 1),
                    help="Enable DT5-7 PC2 relay control with the physically verified ON level")
parser.add_argument("--oled-splash", action="store_true",
                    help="Store the boot logo and board description in application flash")
parser.add_argument("--header-link", action="store_true",
                    help="Add opt-in software serial diagnostics on DA_JP1 PC14/PB15")
parser.add_argument("--header-ring", action="store_true",
                    help="Add addressed packet forwarding on the three-board header ring")
parser.add_argument("--standalone-demo", action="store_true",
                    help="Boot into the autonomous sensor/relay/lighting packet demo")
parser.add_argument("--swap-green-blue", action="store_true",
                    help="Use the physically observed PC3=green, PC4=blue lighting wiring")
parser.add_argument("--demo-fixed-dimmer", type=int, choices=range(101), metavar="0..100",
                    help="Diagnostic DT5-7 build: hold PWM fixed while sensing and motion continue")
parser.add_argument("--demo-pb5-display", action="store_true",
                    help="Diagnostic sensor build: large raw PB5 value and level duration on OLED")
args = parser.parse_args()
if args.demo_pb5_display and not (args.standalone_demo and args.profile == "sensor"):
    parser.error("--demo-pb5-display requires --profile sensor --standalone-demo")
if args.demo_fixed_dimmer is not None and not (args.standalone_demo and args.profile == "dt57"):
    parser.error("--demo-fixed-dimmer requires --profile dt57 --standalone-demo")
if args.standalone_demo:
    args.header_ring = True
    if args.profile == "dt57" and args.relay_active_level != 0:
        parser.error("--standalone-demo on dt57 requires --relay-active-level 0")
if args.header_ring:
    args.header_link = True
if args.relay_active_level is not None and args.profile != "dt57":
    parser.error("--relay-active-level requires --profile dt57")
if args.swap_green_blue and args.profile != "lighting":
    parser.error("--swap-green-blue requires --profile lighting")
if args.header_link and args.profile == "display":
    parser.error("--header-link requires lighting, sensor or dt57")
name = {"lighting": "numaker-control", "display": "numaker-display", "dt57": "numaker-dt57", "sensor": "numaker-sensor"}[args.profile]
definitions = ["-DDISPLAY_ONLY"] if args.profile != "lighting" else []
if args.swap_green_blue:
    definitions.append("-DLIGHTING_SWAP_GB")
if args.oled_splash:
    if args.profile == "display":
        parser.error("--oled-splash requires lighting, dt57, or sensor")
    name += "-oled"
    header = BUILD / f"{name}-frames.h"
    arrays = []
    for symbol, asset in (("oled_boot_logo", "logo"), ("oled_boot_description", args.profile)):
        frame = (ROOT / "firmware" / "oled" / f"{asset}.bin").read_bytes()
        if len(frame) != 1024:
            parser.error(f"OLED frame {asset}.bin must contain exactly 1024 bytes")
        rows = [", ".join(f"0x{byte:02x}" for byte in frame[start:start + 16])
                for start in range(0, len(frame), 16)]
        arrays.append(f"static const unsigned char {symbol}[1024] = {{\n    "
                      + ",\n    ".join(rows) + "\n};\n")
    header.write_text("\n".join(arrays))
    definitions.extend(["-DOLED_BOOT_SHOW", "-include", str(header)])
if args.profile == "dt57":
    definitions.append("-DDT57_PWM")
    if args.relay_active_level is not None:
        definitions.append(f"-DDT57_RELAY_ACTIVE_LEVEL={args.relay_active_level}")
if args.profile in ("dt57", "sensor"):
    definitions.append("-DINPUT_DIAG")
if args.profile == "sensor":
    definitions.append("-DSENSOR_PROFILE")
if args.header_link:
    name += "-link"
    definitions.append("-DDALI_HEADER_LINK")
if args.header_ring:
    name += "-ring"
    node = {"sensor": 1, "dt57": 2, "lighting": 3}[args.profile]
    definitions.append(f"-DHEADER_RING_NODE_ID={node}")
if args.standalone_demo:
    name += "-demo"
    definitions.append("-DSTANDALONE_DEMO")
if args.demo_fixed_dimmer is not None:
    name += f"-fixed{args.demo_fixed_dimmer}"
    definitions.append(f"-DDEMO_FIXED_DIMMER={args.demo_fixed_dimmer}")
if args.demo_pb5_display:
    name += "-pb5"
    definitions.append("-DDEMO_PB5_DISPLAY")
elf = BUILD / (name+".elf")
subprocess.run([
    "arm-none-eabi-gcc", "-mcpu=cortex-m23", "-mthumb", "-Os", "-g3",
    "-Wall", "-Wextra", "-Werror", "-ffreestanding", "-fno-builtin",
    "-fdata-sections", "-ffunction-sections", "-nostdlib",
    "-Wl,--gc-sections", f"-Wl,-Map={BUILD / (name+'.map')}", *definitions,
    f"-T{ROOT / 'firmware/link.ld'}", str(ROOT / "firmware/main.c"),
    "-lgcc", "-o", str(elf),
], check=True)
subprocess.run(["arm-none-eabi-objcopy", "-O", "binary", str(elf),
                str(BUILD / (name+".bin"))], check=True)
subprocess.run(["arm-none-eabi-size", str(elf)], check=True)
