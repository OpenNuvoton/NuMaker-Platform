/* Host simulation of the firmware ISR against independent UART waveforms.
 * cc -std=c11 -Wall -Wextra -Werror tools/test_header_link.c -o /tmp/test-header-link
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static uint32_t registers[0x5000 / 4];
#define REG(a) registers[((a) - 0x40000000u) / 4]
#define SYS(o) REG(0x40000000u + (o))
#define PC(o) REG(0x40004080u + (o))
#define LINK_IRQ_OFF() ((void)0)
#define LINK_IRQ_ON() ((void)0)
#include "../firmware/header_link.h"

static void input(unsigned level, unsigned duration) {
    LINK_PB(0x10) = (LINK_PB(0x10) & ~(1u << 15)) | (level << 15);
    while (duration--) link_tick();
}
static void frame(unsigned byte, unsigned stop) {
    input(0, 20);
    for (unsigned bit = 0; bit < 8; ++bit) input((byte >> bit) & 1, 20);
    input(stop, 20);
}
int main(void) {
    LINK_PB(0x10) = 1u << 15;
    link_enable();
    /* Every byte, back-to-back frames, and every initial sample phase. */
    for (unsigned phase = 0; phase < 20; ++phase) {
        input(1, phase + 1);
        for (unsigned byte = 0; byte < 256; ++byte) {
            frame(byte, 1);
            assert(link_rx_head != link_rx_tail);
            assert(link_rx_queue[link_rx_tail] == byte);
            link_rx_tail = (link_rx_tail + 1) & 127;
        }
    }
    assert(link_received == 5120 && !link_framing && !link_overflow);
    frame(0xff, 0);
    assert(link_framing == 1 && link_rx_head == link_rx_tail);
    input(1, 40);
    /* An unread burst must report overflow, never overwrite unread data. */
    for (unsigned byte = 0; byte < 128; ++byte) frame(byte, 1);
    assert(link_overflow == 1);
    assert(((link_rx_head - link_rx_tail) & 127) == 127);
    for (unsigned byte = 0; byte < 127; ++byte) {
        assert(link_rx_queue[link_rx_tail] == byte);
        link_rx_tail = (link_rx_tail + 1) & 127;
    }
    /* Queued TX must generate exact 8-N-1 levels at 1000 baud. */
    const unsigned bytes[] = {0x00, 0x55, 0xff, 0xa3};
    const uint8_t queued[] = {0x00, 0x55, 0xff, 0xa3};
    assert(link_enqueue(queued, sizeof queued));
    for (unsigned i = 0; i < 4; ++i) {
        unsigned bits = (bytes[i] << 1) | (1u << 9);
        for (unsigned bit = 0; bit < 10; ++bit) {
            for (unsigned t = 0; t < 20; ++t) {
                link_tick();
                assert(LINK_TX == ((bits >> bit) & 1));
            }
        }
    }
    link_tick();
    assert(link_sent == 4 && LINK_TX == 1);
    link_disable();
    /* OFF restores owned pin fields while preserving unrelated pin changes. */
    memset(registers, 0xa5, sizeof registers);
    uint32_t pc_mode = PC(0), pb_mode = LINK_PB(0), pc_mux = SYS(0x44), pb_mux = SYS(0x3c);
    uint32_t pc_pull = PC(0x30), pb_pull = LINK_PB(0x30), digital = LINK_PB(0x20), dout = PC(8);
    link_enable();
    PC(0) ^= 1;
    SYS(0x44) ^= 1;
    link_enable(); /* idempotent ON must not replace the saved configuration */
    link_disable();
    assert(PC(0) == (pc_mode ^ 1) && LINK_PB(0) == pb_mode);
    assert(SYS(0x44) == (pc_mux ^ 1) && SYS(0x3c) == pb_mux);
    assert(PC(0x30) == pc_pull && LINK_PB(0x30) == pb_pull);
    assert(LINK_PB(0x20) == digital && PC(8) == dout);
    unsigned sent = link_sent;
    for (unsigned t = 0; t < 500; ++t) link_tick();
    assert(!link_enabled && link_sent == sent);
    puts("PASS: 5120 received frames; framing/overflow; transmit timing; pin restoration");
}
