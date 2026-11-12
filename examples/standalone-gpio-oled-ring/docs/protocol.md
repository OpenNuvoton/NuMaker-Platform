# GPIO link and application protocol

The physical links are sensor PC14 → relay PB15, relay PC14 → lighting PB15, and lighting PC14 → sensor PB15, with common ground. PC14 is open-drain, PB15 is an input. This uses MCU-side DA_JP1 test points and a private UART-style protocol, rather than DALI bus messages.

The 20 kHz SysTick provides twenty ticks per 1000-baud bit. Frames use 8N1. TX/RX queues hold 64/128 bytes respectively, with one slot reserved to distinguish full from empty. CRC and application processing run in the main loop.

## Transport frame

```text
A5 5A 01 SRC DST TTL SEQ LEN PAYLOAD CRC_H CRC_L
```

Payloads have 1–16 bytes; the demo uses 16 for a total 26-byte frame. CRC16-CCITT-FALSE uses polynomial `0x1021`, initial value `0xFFFF`, no final XOR, and covers the header and payload. The destination consumes a valid frame. Other nodes decrement TTL, recompute CRC and forward; TTL=1 expires. Forwarding waits for TX space rather than overwriting queued bytes. A partially parsed frame times out after 100 ms without parser-received progress. CRC is an error-detection mechanism, not authentication.

## Demo schema 2

| Payload byte(s) | Meaning |
| --- | --- |
| 0 | `0x10` sensor→relay, `0x11` relay→lighting, `0x12` lighting→sensor |
| 1 | Flags: lux-valid bit 0, PIR-valid bit 1, PIR-armed bit 3 |
| 2–3 | Raw PB5 ADC count, little-endian |
| 4–5 | Integer lux, little-endian |
| 6 | Dimmer duty, 0–100 |
| 7 | Relay state, 0/1 |
| 8 | Relay event ID, zero before first event |
| 9–10 | Event elapsed milliseconds, little-endian, 0–10000 |
| 11 | Schema marker `2` |
| 12 | Sender's remaining local warmup seconds |
| 13 | Ready mask: sensor=1, relay=2, lighting=4 |
| 14 | Sensor motion sequence |
| 15 | Pending request bit 0 / accepted acknowledgment bit 1 |

The source publishes at approximately 1 Hz. Relay and lighting generate successor state packets when they receive data. Local motion is edge-captured and held pending until its matching accepted ACK returns. The relay deduplicates motion IDs and starts a fixed 10-second event only when all ready bits are fresh. A sensor warmup packet allows the relay to recognize a reset motion sequence after sensor reboot without treating ordinary link loss as a new sequence epoch.

Each board contributes its ready bit only after 15 seconds. Peer readiness expires when incoming state is at least 3 seconds old; a broken chain can recover as traffic returns. Existing relay and strobe deadlines still expire locally if packets stop. Ambient PWM retains its last valid target.

A full frame takes `26 × 10 / 1000 = 260 ms` on each link, or 780 ms for three serial transmissions. Sampling phase, the 1 Hz publication schedule, queueing and cooperative service add latency. Receiver stopwatches compensate one frame's airtime and avoid moving backward; this is approximate event alignment, not a synchronized distributed clock. The fixed output deadline uses local SysTick counters.
