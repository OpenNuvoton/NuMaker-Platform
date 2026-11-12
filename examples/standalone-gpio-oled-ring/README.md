# Autonomous GPIO ring with live OLED diagnostics

Three NDA102SD2 baseboards run a sensor → DT5-7 relay/dimmer → lighting → sensor ring without USB control commands. Their OLEDs show local readings, output state and real packet counters. The sensor can display PB5 in large digits with live HIGH/LOW duration; relay and lighting show a large shared-event stopwatch and dimmer percentage.

This is a private 1000-baud software UART protocol on the MCU-side DA_JP1 test points. It does not implement the DALI bus protocol.

| Node | Role | Local hardware |
| --- | --- | --- |
| 1 | Sensor / origin | PB5 through MCU EADC channel 5; TSL25721 ambient sensor at I²C `0x39`, PC0 SDA / PC1 SCL; OLED at `0x3C` on PB0/PB1 |
| 2 | DT5-7 relay / router | PC2 active-low relay for output 208; PA6 20 kHz PWM for output 206 |
| 3 | Lighting / acknowledgment | Red/blue RGB motion effect and independent warm/cool white fade |

## Wiring

```text
SENSOR                         RELAY                         LIGHTING
D_TX / PC14 -----------------> D_RX / PB15
                               D_TX / PC14 ----------------> D_RX / PB15
D_RX / PB15 <----------------------------------------------- D_TX / PC14
GND -------------------------- GND -------------------------- GND
```

Use compatible logic supplies and one transmitter per receive wire. The lighting-to-sensor return wire completes the ring. The DALI bus-voltage terminals are separate from these logic jumpers. Each board still needs correct power; removing USB power is a separate electrical test from closing a host serial port.

## Behavior

- Every MCU applies a **15-second startup gate**. Motion additionally requires fresh ready bits from all three boards and an inactive PIR level before arming. Sampling, packet exchange and ambient lighting continue during warmup. Missing readiness appears as `WAITALL`; the normal dashboard shows S/R/L ready bits.
- The sensor samples PB5 on a **50 ms target schedule** and ambient light on a separate **200 ms target schedule**. It publishes once per second. The shared cooperative loop means these intervals are targets rather than hard realtime guarantees.
- PB5 thresholds are `>1800` HIGH and `<100` LOW, with intermediate values retaining the candidate state. The motion policy requires 100 ms stable HIGH and 200 ms stable LOW. A first pending event is kept until its matching chain acknowledgment arrives.
- A fresh accepted motion ID starts a **fixed 10-second** relay and red/blue strobe burst. Continued HIGH never extends it. Cutoff runs in SysTick. Another burst requires clear/rearm and a new rise.
- Valid lux sets the dimmer target to `round(40000 / (400 + lux))`. The relay ramps one percentage point per 80 ms service interval. Invalid lux retains the previous target and does not reset motion state.
- Lighting independently fades white brightness at up to 12.5 percentage points/s. For smoothed brightness `b`, warm is `round(b*b/100)` and cool is `b-warm`. White continues when the RGB burst ends. This is an artistic mix, not calibrated color-temperature control.
- USB accepts read-only diagnostics. Boot, sensing, motion and packet transfer require no host command. USB cannot inject motion, pause the demo, or drive outputs in this build.

The large-PB5 sensor view shows a raw 12-bit ADC count, current electrical level/duration, last completed HIGH duration, warmup/readiness and packet counts. A two-level signal measured with an ADC is not necessarily an analog motion-strength output. The PIR part, shield circuit, active polarity and digital-input voltage compatibility still need independent confirmation. See [PIR findings and verification limits](docs/verification.md).

## Build

Use Python 3, a native C compiler (`cc`), and GNU Arm Embedded GCC with `arm-none-eabi-objcopy` and `arm-none-eabi-size`. The MCU applications have no SDK runtime dependency and use no external SPI flash. The linker allocates 64 KB internal APROM and 12 KB RAM.

From this example directory:

```bash
# Latest sensor OLED: large PB5 value and electrical-level duration.
python3 tools/build.py --profile sensor --standalone-demo --demo-pb5-display
python3 tools/build.py --profile dt57 --relay-active-level 0 --standalone-demo
python3 tools/build.py --profile lighting --standalone-demo --swap-green-blue

# Normal sensor stopwatch/dimmer dashboard, if preferred.
python3 tools/build.py --profile sensor --standalone-demo
```

The tested lighting shield emits red on PC5, green on PC3 and blue on PC4, so its build uses `--swap-green-blue`. The published schematic labels green/blue oppositely; verify your board before adopting that override.

## Back up and program

Windows PowerShell 5.1 and the Nuvoton Nu-Link Command Tool are required for these flashing scripts. Connect the required board USB debugger(s), stop older host lighting/trigger controllers, and preserve your original firmware before replacing it.

1. Copy `config/boards.example.json` to **`config/boards.local.json`**, which is ignored by Git. Replace every example debugger ID, USB parent instance and COM port with the values for your boards. The configuration must describe all three known boards so debugger isolation can reject unknown devices.
2. Run `tools/backup-boards.ps1 -NuLinkIds <nonzero-ID>` for each identified nonzero debugger. A zero-ID sensor must be connected alone for `tools/backup-boards.ps1 -IsolatedZeroId`. Local backups stay under `backup/`; no factory images or device configuration dumps are distributed here.
3. Build the images and flash the chosen variants from Windows PowerShell. The flasher elevates to isolate debugger interfaces and restores them in `finally`. It checks the exact USB parent/interface, identifies NDA102SD2, snapshots current APROM, and performs erase/write/verify/reset.

```powershell
tools/flash-oled-boards.ps1 -StandaloneDemo -Board lighting
tools/flash-oled-boards.ps1 -StandaloneDemo -Board dt57
tools/flash-oled-boards.ps1 -StandaloneDemo -Board sensor -PB5Display
# Omit -PB5Display to select the normal sensor dashboard image.
```

Use `-TargetConfig <path>` to select another local board configuration. The scripts refuse unedited placeholders. COM numbers in examples are defaults, not device identities. APROM programming leaves LDROM and configuration words outside the update operation.

## Observe the running boards

Production diagnostics include `STATUS`, `HELP`, `INPUTS`, `DEMO STATUS`, `DEMO FRAME`, `LINK STATUS`, `RING STATUS` and `RING READ`. The USB serial protocol is 115200 baud, 8N1, newline-delimited ASCII; send one query and wait for its `OK` or `ERR` reply.

```powershell
# Read-only live PB5 window, 60-second graph and timestamped sample log.
tools/open-pir-live.ps1 -Port COM11
tools/open-pir-live.ps1 -Stop

# Close the live logger first: only one reader may own a COM port.
tools/observe-standalone.ps1 -SensorPort COM11 -RelayPort COM5 -LightingPort COM3
tools/trace-demo-inputs.ps1 -Seconds 60 -SensorPort COM11 -RelayPort COM5 -LightingPort COM3
```

The observer waits for warmup/readiness, closes all host serial ports for 12 seconds, then verifies packet and sampling progress. It does not inject events or prove that boards have adequate power with USB cables physically removed. Reports and framebuffer captures are generated under ignored `build/`. The live window uses Windows Forms; closing it leaves the read-only worker running until `-Stop`, while firmware itself remains autonomous.

## Optional fixed-output PIR experiment

The diagnostic relay build holds its PWM at 60% while lux acquisition, motion and forwarding continue. The lighting white target follows that fixed duty. It removes ambient-driven output changes from a cover/uncover comparison while preserving the motion path.

```bash
python3 tools/build.py --profile dt57 --relay-active-level 0 --standalone-demo --demo-fixed-dimmer 60
```

```powershell
tools/flash-oled-boards.ps1 -StandaloneDemo -Board dt57 -FixedDimmer 60
tools/observe-fixed-dimmer.ps1 -Port COM5 -Duty 60
# Restore the normal image when the experiment is complete.
tools/flash-oled-boards.ps1 -StandaloneDemo -Board dt57
```

Wait for startup and the white fade to settle before comparing several repeat trials. The bench record currently marks this diagnostic **built and native-tested, deployment pending**. It is not evidence that the long PIR pulse has been fixed.

## Verify without hardware

```bash
python3 tools/verify.py          # UART, ring and application-policy simulations
python3 tools/verify.py --build  # Also compile all three normal applications and both diagnostics
```

Coverage includes UART sample phase/transmit timing, CRC/TTL/routing/backpressure, warmup/readiness, first-trigger cutoff, held-high suppression, reboot sequence reset, sensor-failure independence, fixed PWM, and the 19.6-second raw-level tracker. Hardware or optical behavior requires bench checks.

## Documentation

- [PowerPoint engineering write-up](docs/NuMaker_GPIO_Daisy_Chain_and_Ambient_Light.pptx)
- [PDF engineering write-up](docs/NuMaker_GPIO_Daisy_Chain_and_Ambient_Light.pdf)
- [Packet format and timing](docs/protocol.md)
- [Ambient light acquisition and lux calculation](docs/ambient-light.md)
- [Bench verification and open PIR findings](docs/verification.md)

The OLED font derives from DejaVu Sans Mono; its license is in [reference/font-license.txt](reference/font-license.txt). Official Nuvoton and Winbond logos are embedded in the write-ups. The code does not depend on external Winbond flash.
