/* Private test protocol over the DA_JP1 software UART; not DALI.
 * A5 5A 01 source destination TTL sequence length payload CRC16-BE.
 * Payload: 1..16 bytes. CRC16-CCITT-FALSE covers header and payload.
 * The destination consumes a frame; other nodes decrement TTL and forward.
 * Addressed application payloads never execute the USB command interpreter.
 */
#define RING_MAX_FRAME 26u
static uint8_t ring_active, ring_frame[RING_MAX_FRAME], ring_last[RING_MAX_FRAME];
static unsigned ring_have, ring_want, ring_pending, ring_last_size;
static uint32_t ring_last_tick, ring_delivered, ring_forwarded, ring_expired;
static uint32_t ring_crc_errors, ring_parse_errors;

static uint16_t ring_crc(const uint8_t *bytes, unsigned count) {
    uint16_t crc = 0xffff;
    while (count--) {
        crc ^= (uint16_t)*bytes++ << 8;
        for (unsigned i = 0; i < 8; ++i)
            crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
    }
    return crc;
}
static void ring_seal(uint8_t *frame, unsigned size) {
    uint16_t crc = ring_crc(frame, size - 2);
    frame[size-2] = (uint8_t)(crc >> 8); frame[size-1] = (uint8_t)crc;
}
static void ring_start(void) {
    link_disable();
    link_enable();
    ring_have = ring_want = ring_pending = ring_last_size = 0;
    ring_delivered = ring_forwarded = ring_expired = ring_crc_errors = ring_parse_errors = 0;
    ring_active = 1;
}
static int ring_send(unsigned destination, unsigned sequence, unsigned ttl,
                     const uint8_t *payload, unsigned count) {
    if (!ring_active || !destination || destination > 254 || sequence > 255 ||
        !ttl || ttl > 16 || !count || count > 16) return 0;
    uint8_t frame[RING_MAX_FRAME];
    frame[0] = 0xa5; frame[1] = 0x5a; frame[2] = 1; frame[3] = HEADER_RING_NODE_ID;
    frame[4] = (uint8_t)destination; frame[5] = (uint8_t)ttl;
    frame[6] = (uint8_t)sequence; frame[7] = (uint8_t)count;
    for (unsigned i = 0; i < count; ++i) frame[8+i] = payload[i];
    ring_seal(frame, count + 10);
    return link_enqueue(frame, count + 10);
}
static void ring_service(void) {
    if (!ring_active) return;
    if (ring_pending) {
        if (!link_enqueue(ring_frame, ring_pending)) return;
        ring_pending = 0; ++ring_forwarded;
    }
    if (ring_have && ticks - ring_last_tick > 2000u) {
        ring_have = ring_want = 0; ++ring_parse_errors;
    }
    while (link_rx_head != link_rx_tail) {
        uint8_t byte = link_rx_queue[link_rx_tail];
        link_rx_tail = (link_rx_tail + 1u) & 127u;
        ring_last_tick = ticks;
        if (!ring_have) {
            if (byte == 0xa5) ring_frame[ring_have++] = byte;
            continue;
        }
        if (ring_have == 1 && byte != 0x5a) {
            ring_have = byte == 0xa5;
            continue;
        }
        ring_frame[ring_have++] = byte;
        if (ring_have == 8) {
            if (ring_frame[2] != 1 || !ring_frame[3] || ring_frame[3] == 255 ||
                !ring_frame[4] || ring_frame[4] == 255 ||
                !ring_frame[5] || ring_frame[5] > 16 ||
                !ring_frame[7] || ring_frame[7] > 16) {
                ring_have = ring_want = 0; ++ring_parse_errors; continue;
            }
            ring_want = 10u + ring_frame[7];
        }
        if (!ring_want || ring_have < ring_want) continue;
        unsigned size = ring_want;
        ring_have = ring_want = 0;
        uint16_t expected = (uint16_t)((ring_frame[size-2] << 8) | ring_frame[size-1]);
        if (ring_crc(ring_frame, size-2) != expected) { ++ring_crc_errors; continue; }
        if (ring_frame[4] == HEADER_RING_NODE_ID) {
            for (unsigned i = 0; i < size; ++i) ring_last[i] = ring_frame[i];
            ring_last_size = size; ++ring_delivered;
#ifdef STANDALONE_DEMO
            demo_packet(ring_frame, size);
#endif
        } else if (ring_frame[5] == 1) ++ring_expired;
        else {
            --ring_frame[5]; ring_seal(ring_frame, size);
            if (link_enqueue(ring_frame, size)) ++ring_forwarded;
            else { ring_pending = size; return; }
        }
    }
}
