/* Native wrapper for testing the real firmware parser/forwarder on three nodes. */
#include <stdint.h>
#include <string.h>
static uint32_t registers[0x5000 / 4], ticks;
#define REG(a) registers[((a) - 0x40000000u) / 4]
#define SYS(o) REG(0x40000000u + (o))
#define PC(o) REG(0x40004080u + (o))
#define LINK_IRQ_OFF() ((void)0)
#define LINK_IRQ_ON() ((void)0)
#include "../firmware/header_link.h"
#include "../firmware/header_ring.h"
void reset_node(void) {
    link_disable();
    memset(registers, 0, sizeof registers);
    LINK_PB(0x10) = 1u << 15;
    ring_start();
    link_tick(); /* exercise the linked physical layer once at idle */
}
int send_frame(unsigned dst, unsigned seq, unsigned ttl, const uint8_t *p, unsigned n) {
    return ring_send(dst, seq, ttl, p, n);
}
void feed(const uint8_t *p, unsigned n) {
    while (n--) {
        link_rx_queue[link_rx_head] = *p++;
        link_rx_head = (link_rx_head + 1) & 127;
    }
    ring_service();
}
unsigned take_tx(uint8_t *out) {
    unsigned n = 0;
    while (link_tx_tail != link_tx_head) {
        out[n++] = link_tx_queue[link_tx_tail];
        link_tx_tail = (link_tx_tail + 1) & 63;
    }
    ring_service();
    return n;
}
unsigned take_last(uint8_t *out) {
    unsigned n = ring_last_size;
    memcpy(out, ring_last, n); ring_last_size = 0;
    return n;
}
unsigned counter(unsigned which) {
    const uint32_t values[] = {ring_delivered, ring_forwarded, ring_expired,
                              ring_crc_errors, ring_parse_errors, ring_pending};
    return values[which];
}
void advance(unsigned n) { ticks += n; ring_service(); }
