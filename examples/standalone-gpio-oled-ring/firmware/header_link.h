/* DA_JP1 on the NDA102SD2 baseboard: PC14 TX, PB15 RX (manual p.50).
 * These are DALI transceiver test points, not hardware UART data pins.
 * Opt-in 1000-baud 8-N-1 software serial, clocked by the 20 kHz SysTick.
 * Open-drain TX avoids driving high against the attached receive transistor.
 * The byte transport never executes USB commands; the ring layer validates
 * addressed application frames before the standalone policy handles them.
 */
#define LINK_PB(o) REG(0x40004040u + (o))
#define LINK_TX REG(0x400048b8u)
#define LINK_BIT_TICKS 20u
#ifndef LINK_IRQ_OFF
#define LINK_IRQ_OFF() __asm volatile("cpsid i" ::: "memory")
#define LINK_IRQ_ON() __asm volatile("cpsie i" ::: "memory")
#endif
static volatile uint8_t link_enabled, link_tx_queue[64], link_rx_queue[128];
static volatile uint32_t link_tx_head, link_tx_tail, link_rx_head, link_rx_tail;
static volatile uint32_t link_sent, link_received, link_framing, link_overflow, link_edges;
static uint32_t link_saved_pc_mode, link_saved_pb_mode, link_saved_pc_mux, link_saved_pb_mux;
static uint32_t link_saved_pc_pull, link_saved_pb_pull, link_saved_pc_dout, link_saved_pb_digital;
static unsigned link_tx_wait, link_tx_bits, link_tx_frame;
static unsigned link_rx_wait, link_rx_bits, link_rx_frame, link_previous;

static int link_enqueue(const uint8_t *bytes, unsigned count) {
    unsigned head = link_tx_head;
    if (count > ((link_tx_tail - head - 1u) & 63u)) return 0;
    for (unsigned i = 0; i < count; ++i) {
        link_tx_queue[head] = bytes[i]; head = (head + 1u) & 63u;
    }
    link_tx_head = head;
    return 1;
}

static void link_tick(void) {
    if (!link_enabled) return;
    unsigned input = (LINK_PB(0x10) >> 15) & 1u;
    if (input != link_previous) ++link_edges;
    if (link_rx_wait) {
        if (!--link_rx_wait) {
            if (link_rx_bits < 8) {
                link_rx_frame |= input << link_rx_bits++;
                link_rx_wait = LINK_BIT_TICKS;
            } else if (input) {
                ++link_received;
                uint32_t next = (link_rx_head + 1u) & 127u;
                if (next == link_rx_tail) ++link_overflow;
                else {
                    link_rx_queue[link_rx_head] = (uint8_t)link_rx_frame;
                    link_rx_head = next;
                }
            } else ++link_framing;
        }
    } else if (!input && link_previous) {
        link_rx_frame = link_rx_bits = 0;
        link_rx_wait = LINK_BIT_TICKS + LINK_BIT_TICKS / 2u;
    }
    link_previous = input;
    if (link_tx_wait && --link_tx_wait) return;
    if (link_tx_bits) {
        LINK_TX = link_tx_frame & 1u;
        link_tx_frame >>= 1;
        --link_tx_bits;
        link_tx_wait = LINK_BIT_TICKS;
    } else if (link_tx_head != link_tx_tail) {
        unsigned byte = link_tx_queue[link_tx_tail];
        link_tx_tail = (link_tx_tail + 1u) & 63u;
        link_tx_frame = byte | (1u << 8); /* eight data bits then stop */
        link_tx_bits = 9;
        LINK_TX = 0; /* start */
        link_tx_wait = LINK_BIT_TICKS;
        ++link_sent;
    }
}

static void link_disable(void) {
    if (!link_enabled) return;
    LINK_IRQ_OFF();
    link_enabled = 0;
    /* Stop driving before restoring the saved latch and mux. */
    PC(0) &= ~(3u << 28);
    PC(8) = (PC(8) & ~(1u << 14)) | link_saved_pc_dout;
    PC(0x30) = (PC(0x30) & ~(3u << 28)) | link_saved_pc_pull;
    LINK_PB(0x30) = (LINK_PB(0x30) & ~(3u << 30)) | link_saved_pb_pull;
    LINK_PB(0x20) = (LINK_PB(0x20) & ~(1u << 31)) | link_saved_pb_digital;
    SYS(0x100) = 0x59; SYS(0x100) = 0x16; SYS(0x100) = 0x88;
    SYS(0x44) = (SYS(0x44) & ~(15u << 24)) | link_saved_pc_mux;
    SYS(0x3c) = (SYS(0x3c) & ~(15u << 28)) | link_saved_pb_mux;
    SYS(0x100) = 0;
    LINK_PB(0) = (LINK_PB(0) & ~(3u << 30)) | link_saved_pb_mode;
    PC(0) = (PC(0) & ~(3u << 28)) | link_saved_pc_mode;
    LINK_IRQ_ON();
}

static void link_enable(void) {
    if (link_enabled) return;
    LINK_IRQ_OFF();
    link_saved_pc_mode = PC(0) & (3u << 28);
    link_saved_pb_mode = LINK_PB(0) & (3u << 30);
    link_saved_pc_mux = SYS(0x44) & (15u << 24);
    link_saved_pb_mux = SYS(0x3c) & (15u << 28);
    link_saved_pc_pull = PC(0x30) & (3u << 28);
    link_saved_pb_pull = LINK_PB(0x30) & (3u << 30);
    link_saved_pc_dout = PC(8) & (1u << 14);
    link_saved_pb_digital = LINK_PB(0x20) & (1u << 31);
    PC(0) &= ~(3u << 28);
    LINK_PB(0) &= ~(3u << 30);
    LINK_TX = 1;
    SYS(0x100) = 0x59; SYS(0x100) = 0x16; SYS(0x100) = 0x88;
    SYS(0x44) &= ~(15u << 24);
    SYS(0x3c) &= ~(15u << 28);
    SYS(0x100) = 0;
    PC(0x30) = (PC(0x30) & ~(3u << 28)) | (1u << 28);
    LINK_PB(0x30) = (LINK_PB(0x30) & ~(3u << 30)) | (1u << 30);
    LINK_PB(0x20) &= ~(1u << 31);
    PC(0) |= 2u << 28; /* open-drain PC14 */
    link_tx_head = link_tx_tail = link_rx_head = link_rx_tail = 0;
    link_sent = link_received = link_framing = link_overflow = link_edges = 0;
    link_tx_wait = link_tx_bits = link_rx_wait = 0;
    link_previous = (LINK_PB(0x10) >> 15) & 1u;
    link_enabled = 1;
    LINK_IRQ_ON();
}
