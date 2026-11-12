# Ambient light: TSL25721 digital readout

The tested sensor identifies as TSL25721 (`ID=0x34`) at I²C address `0x39`, on PC0 SDA / PC1 SCL. Its two photodiodes and integrating ADCs are inside the sensor: channel 0 measures visible plus infrared, while channel 1 is primarily infrared. **Ambient light does not use the MCU's ADC.** PB5/EADC channel 5 is a separate voltage measurement used by the prototype motion path.

The firmware bit-bangs I²C with bounded waits and temporarily selects the ambient bus. It restores the OLED bus selection afterward; the sensor OLED is on PB0/PB1 at `0x3C`. These I²C links are distinct from the PC14/PB15 software-UART ring.

## Initialization

The values below are command bytes, including the command-select bit. Register offsets alone are shown in parentheses.

| Operation | Command | Data/result |
| --- | --- | --- |
| Read ID (0x12) | `0x92` | Expect `0x34` |
| Power oscillator, ENABLE (0x00) | `0x80` | Write `0x01` / PON |
| Wait | — | At least 3 ms, scheduled without a blocking delay |
| Integration, ATIME (0x01) | `0x81` | Write `0xED` = 237 |
| Gain, CONTROL (0x0F) | `0x8F` | Write `0x00` = 1× |
| CONFIG (0x0D) | `0x8D` | Write `0x00` = standard gain |
| Enable continuous ALS | `0x80` | Write `0x03` = PON + AEN |

Nominal integration is `(256−237) × 2.73 ms = 51.87 ms`. The sensor integrates continuously while enabled; the MCU polls on a separate 200 ms target schedule. A conversion interval and the firmware polling interval are different quantities.

## Read transaction

First read STATUS with command `0x93` and require `AVALID` bit 0. Then use command `0xB4 = 0xA0 | 0x14` to auto-increment through both result pairs:

```text
START → 0x72 (write) → ACK → 0xB4 → ACK
REPEATED START → 0x73 (read) → ACK
C0 low → ACK → C0 high → ACK → C1 low → ACK → C1 high → NACK → STOP
```

`0x39` is the 7-bit address; `0x72`/`0x73` include the write/read bit. Assemble `C0=byte[0] | byte[1]<<8` and `C1=byte[2] | byte[3]<<8`. These are two 16-bit sensor ADC results, not MCU ADC samples.

Read failures, missing valid data or either channel `>=19456` clear the lux-valid flag. The relay preserves its last target if lux is invalid. Ambient retries do not reset the PIR edge detector or output deadline.

## Lux calculation

For the configured integration time, 1× gain and glass attenuation factor 1:

```text
CPL = 51.87 / 60 = 0.8645
L1  = (C0 − 1.87*C1) / CPL
L2  = (0.63*C0 − C1) / CPL
lux = max(L1, L2, 0)
```

An earlier bench read returned bytes `EF 02 7A 00`: C0=751, C1=122 and approximately 604.81 lux. The MCU uses integer arithmetic:

```c
L1 = (100*C0 - 187*C1)*60/5187;
L2 = (63*C0 - 100*C1)*60/5187;
```

It clamps the maximum result to 0–65535 and transports an integer; this example becomes 604 lux. No enclosure, spectral or optical calibration has been applied. Gain/integration changes require updating the scale and saturation policy.

Valid lux maps to `round(40000/(400+lux))` percent dimmer duty. The relay moves one point per 80 ms service interval. Lighting independently smooths the received duty and computes warm=`round(b²/100)`, cool=`b-warm`; it continues after RGB motion timeout.

Primary reference: [ams OSRAM TSL2572 DS000178 v2-00](https://look.ams-osram.com/m/7501e2380e896801/original/TSL2572-DS000178.pdf), especially photodiodes/ALS operation, lux equation, command, enable, control, status and channel-data registers. [Manufacturer product page](https://ams-osram.com/products/sensor-solutions/ambient-light-color-spectral-proximity-sensors/ams-tsl25721-ambient-light-sensor).
