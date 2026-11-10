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
args = parser.parse_args()
if args.relay_active_level is not None and args.profile != "dt57":
    parser.error("--relay-active-level requires --profile dt57")
name = {"lighting": "numaker-control", "display": "numaker-display", "dt57": "numaker-dt57", "sensor": "numaker-sensor"}[args.profile]
definitions = ["-DDISPLAY_ONLY"] if args.profile != "lighting" else []
if args.profile == "dt57":
    definitions.append("-DDT57_PWM")
    if args.relay_active_level is not None:
        definitions.append(f"-DDT57_RELAY_ACTIVE_LEVEL={args.relay_active_level}")
if args.profile in ("dt57", "sensor"):
    definitions.append("-DINPUT_DIAG")
if args.profile == "sensor":
    definitions.append("-DSENSOR_PROFILE")
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
