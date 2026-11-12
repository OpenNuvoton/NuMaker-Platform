/* NuMaker-DALI-NDA102SD2 + NuMaker-DALI-Lighting v1.1.
 * Board pin mapping: Nuvoton user manual, sections 4 and 8.
 * Shared register layout: installed Nuvoton M251 DFP; checked against the
 * original NDA102SD2 firmware. Pin mux: Nuvoton NUC_NDA102_Content.js.
 * This standalone firmware controls local outputs; it does not implement DALI.
 */
#include <stdint.h>
#include "font.h"

#define REG(a) (*(volatile uint32_t *)(a))
#define SYS(o) REG(0x40000000u + (o))
#define CLK(o) REG(0x40000200u + (o))
#define PA(o) REG(0x40004000u + (o))
#define PC(o) REG(0x40004080u + (o))
#define UART(o) REG(0x40070000u + (o))
#define APIN(n) REG(0x40004800u + 4u*(n))
#define CPIN(n) REG(0x40004880u + 4u*(n))
#ifdef SENSOR_PROFILE
static uint8_t oled_bus;
#define OPIN(n) REG((oled_bus ? 0x40004840u : 0x40004880u) + 4u*(n))
#define OLED_INPUT REG(oled_bus ? 0x40004050u : 0x40004090u)
#else
#define OPIN(n) CPIN(n)
#define OLED_INPUT PC(0x10)
#endif
#ifdef DT57_PWM
#define BPWM1(o) REG(0x4005b000u + (o))
static uint8_t lamp_level;
/* Saved DT5-7 factory APROM selects PA6 function 12 (BPWM1_CH3),
 * configures channel 3 for 20 kHz and loops at 100% duty. Only this
 * evidenced PWM output is enabled here. Relay polarity is separately verified. */
static void lamp_set(unsigned level) {
    if (level == 0 || level == 100) {
        BPWM1(0xbc) = level ? 8u : 0u;
        BPWM1(0xb8) = 8u;
    } else {
        BPWM1(0x5c) = level * 6u;
        BPWM1(0xb8) = 0;
    }
    lamp_level = (uint8_t)level;
}
#ifdef DT57_RELAY_ACTIVE_LEVEL
/* User observed Relay 1 LED and clicks during isolated PC2 tests.
 * Enable only with the physically confirmed ON polarity supplied at build. */
static uint8_t relay_on;
static void relay_set(unsigned on) {
    CPIN(2) = on ? DT57_RELAY_ACTIVE_LEVEL : !DT57_RELAY_ACTIVE_LEVEL;
    relay_on = (uint8_t)on;
}
#endif
#endif
#ifdef INPUT_DIAG
#define EADC(o) REG(0x40043000u + (o))
/* Read an Arduino-header analog input without leaving its pin mux changed.
 * EADC is PCLK1 / 4 = 3 MHz; configuration follows Nuvoton's M251
 * EADC_SWTRG_Trigger example and the NDA102 pin mux reference. */
static int analog_read(unsigned channel) {
    unsigned mux_offset = channel < 8 ? 0x38 : 0x3c;
    unsigned mux_shift = (channel & 7u) * 4u;
    uint32_t mode = REG(0x40004040u), mux = SYS(mux_offset);
    uint32_t digital = REG(0x40004060u);
    REG(0x40004040u) = mode & ~(3u << (2u*channel));
    SYS(0x100) = 0x59; SYS(0x100) = 0x16; SYS(0x100) = 0x88;
    SYS(mux_offset) = (mux & ~(15u << mux_shift)) | (1u << mux_shift);
    SYS(0x100) = 0;
    REG(0x40004060u) = digital | (1u << (16u+channel));
    unsigned timeout = 100000;
    while (!(EADC(0x110) & 1u) && --timeout) {}
    int result = -1;
    if (timeout) {
        (void)EADC(0);
        EADC(0x80) = channel | (32u << 24);
        EADC(0x54) = 1;
        timeout = 100000;
        while (--timeout) {
            uint32_t sample = EADC(0);
            if (sample & (1u << 17)) { result = (int)(sample & 4095u); break; }
        }
    }
    REG(0x40004060u) = digital;
    SYS(0x100) = 0x59; SYS(0x100) = 0x16; SYS(0x100) = 0x88;
    SYS(mux_offset) = mux;
    SYS(0x100) = 0;
    REG(0x40004040u) = mode;
    return result;
}
#endif

static volatile uint8_t levels[5]; /* R, G, B, warm, cool: 0..100 */
static volatile uint8_t rx[256];
static volatile uint32_t rx_head, rx_tail, rx_overrun, ticks;
static uint8_t oled_addr, oled_on, sh1106, dashboard = 1;
static char rows[8][22];
static uint8_t framebuffer[1024], dirty_pages;
static volatile uint32_t fade_duration, fade_elapsed;
static uint8_t fade_start[5], fade_target[5];
static volatile uint32_t cycle_period, cycle_steps;
static uint32_t cycle_elapsed;
static uint8_t cycle_phase, cycle_count;
static uint8_t cycle_colors[32][5], palette_staging[32][5];
static uint32_t palette_ready;

#ifdef DALI_HEADER_LINK
#include "header_link.h"
#ifdef HEADER_RING_NODE_ID
#ifdef STANDALONE_DEMO
static void demo_packet(const uint8_t *frame, unsigned size);
#endif
#include "header_ring.h"
#endif
#ifdef STANDALONE_DEMO
#include "demo_effects.h"
#endif
#endif

void SysTick_Handler(void) {
#ifdef DALI_HEADER_LINK
    link_tick();
#endif
    static unsigned phase;
    static unsigned millisecond;
    if (cycle_period && ++cycle_elapsed >= cycle_period) {
        cycle_elapsed = 0;
        if (++cycle_phase == cycle_count) cycle_phase = 0;
        for (unsigned i = 0; i < 5; ++i) levels[i] = cycle_colors[cycle_phase][i];
        ++cycle_steps;
    }
    if (++millisecond == 20) {
        millisecond = 0;
        if (fade_duration) {
            uint32_t elapsed = ++fade_elapsed;
            for (unsigned i = 0; i < 5; ++i) {
                int32_t difference = (int32_t)fade_target[i] - fade_start[i];
                levels[i] = (uint8_t)((int32_t)fade_start[i] + difference * (int32_t)elapsed / (int32_t)fade_duration);
            }
            if (elapsed >= fade_duration) fade_duration = 0;
        }
    }
#if defined(STANDALONE_DEMO) && HEADER_RING_NODE_ID == 3
    demo_lighting_tick();
#elif defined(STANDALONE_DEMO) && HEADER_RING_NODE_ID == 2
    demo_relay_tick();
#endif
    if (++phase == 100) phase = 0;
#ifndef DISPLAY_ONLY
    CPIN(5) = phase >= levels[0];
#ifdef LIGHTING_SWAP_GB
    /* This user's lighting board: PC3 observed green, PC4 inferred blue.
     * The base schematic labels those two colors in the opposite order. */
    CPIN(3) = phase >= levels[1];
    CPIN(4) = phase >= levels[2];
#else
    CPIN(4) = phase >= levels[1];
    CPIN(3) = phase >= levels[2];
#endif
    CPIN(2) = phase >= levels[3];
    APIN(7) = phase >= levels[4];
#endif
    while (!(UART(0x18) & (1u << 14))) {
        uint8_t c = (uint8_t)UART(0);
        uint32_t next = (rx_head + 1) & 255;
        if (next == rx_tail) rx_overrun = 1;
        else { rx[rx_head] = c; rx_head = next; }
    }
    ++ticks;
}

static void delay(void) {
    for (volatile unsigned i = 0; i < 3; ++i) __asm volatile("nop");
}
static void wait_ms(unsigned ms) {
    uint32_t start = ticks;
    while (ticks - start < ms * 20u) {}
}
static void putc_uart(char c) {
    unsigned timeout = 1000000;
    while ((UART(0x18) & (1u << 23)) && --timeout) {}
    if (timeout) UART(0) = (uint8_t)c;
}
static void puts_uart(const char *s) { while (*s) putc_uart(*s++); }
static void number(unsigned n) {
    char s[11]; unsigned i = 0;
    do { s[i++] = (char)('0' + n % 10); n /= 10; } while (n);
    while (i) putc_uart(s[--i]);
}
static int eq(const char *a, const char *b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}
static char *token(char **cursor) {
    char *p = *cursor;
    while (*p == ' ') ++p;
    if (!*p) { *cursor = p; return 0; }
    char *start = p;
    while (*p && *p != ' ') ++p;
    if (*p) *p++ = 0;
    *cursor = p;
    return start;
}
static int parse_uint(char *s, unsigned *out, unsigned limit) {
    unsigned n = 0;
    if (!s || !*s) return 0;
    while (*s) {
        if (*s < '0' || *s > '9') return 0;
        unsigned digit = (unsigned)(*s++ - '0');
        if (n > limit / 10u || (n == limit / 10u && digit > limit % 10u)) return 0;
        n = n * 10u + digit;
    }
    *out = n; return 1;
}
static int parse_level(char *s, unsigned *out) { return parse_uint(s, out, 100); }
static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Open-drain I2C on PC0/PC1. Each wait has a timeout. */
static int scl_high(void) {
    OPIN(1) = 1;
    for (unsigned n = 0; n < 20000; ++n)
        if (OLED_INPUT & 2) { delay(); return 1; }
    return 0;
}
static void i2c_stop(void) {
    OPIN(1) = 0; OPIN(0) = 0; delay();
    (void)scl_high(); OPIN(0) = 1; delay();
}
static int i2c_start(void) {
    OPIN(0) = 1;
    if (!scl_high() || !(OLED_INPUT & 1)) return 0;
    OPIN(0) = 0; delay(); OPIN(1) = 0; return 1;
}
static int i2c_byte(uint8_t b) {
    for (unsigned bit = 0; bit < 8; ++bit) {
        OPIN(0) = !!(b & 0x80); b <<= 1; delay();
        if (!scl_high()) return 0;
        OPIN(1) = 0; delay();
    }
    OPIN(0) = 1; delay();
    if (!scl_high()) return 0;
    int ack = !(OLED_INPUT & 1);
    OPIN(1) = 0; delay(); return ack;
}
#ifdef SENSOR_PROFILE
static int i2c_read_byte(uint8_t *out, int last) {
    uint8_t value = 0;
    OPIN(0) = 1;
    for (unsigned bit = 0; bit < 8; ++bit) {
        delay();
        if (!scl_high()) return 0;
        value = (uint8_t)((value << 1) | !!(OLED_INPUT & 1u));
        OPIN(1) = 0; delay();
    }
    OPIN(0) = !!last; delay(); /* ACK every byte except the last. */
    if (!scl_high()) return 0;
    OPIN(1) = 0; OPIN(0) = 1;
    *out = value;
    return 1;
}
#endif
static int oled_send(uint8_t control, const uint8_t *data, unsigned n) {
    if (!oled_addr) return 0;
    int ok = i2c_start() && i2c_byte((uint8_t)(oled_addr << 1)) && i2c_byte(control);
    for (unsigned i = 0; ok && i < n; ++i) ok = i2c_byte(data[i]);
    i2c_stop(); return ok;
}
static int flush_page(unsigned row) {
    uint8_t commands[] = {(uint8_t)(0xb0 + row), (uint8_t)(sh1106 ? 2 : 0), 0x10};
    if (!oled_send(0, commands, sizeof(commands)) ||
        !oled_send(0x40, framebuffer + row*128, 128)) { oled_on = 0; return 0; }
    dirty_pages &= (uint8_t)~(1u << row);
    return 1;
}
#ifdef OLED_BOOT_SHOW
static uint32_t oled_last_command, oled_frame_since;
static uint8_t oled_host_active, oled_frame_index, oled_next_page;
static uint8_t oled_frame_pending = 1;

static void oled_host_takeover(void) {
    oled_host_active = 1;
}

static void oled_boot_update(void) {
    if (!oled_on) return;
    uint32_t now = ticks;
    if (oled_host_active) {
        if (now - oled_last_command < 20u * 15000u) return;
        oled_host_active = 0;
        oled_frame_index = 0;
        oled_next_page = 0;
        oled_frame_pending = 1;
    }
    if (!oled_frame_pending) {
        uint32_t duration = oled_frame_index ? 6000u : 4000u;
        if (now - oled_frame_since < 20u * duration) return;
        oled_frame_index ^= 1u;
        oled_frame_pending = 1;
    }

    unsigned page = oled_next_page;
    if (page == 0) {
        static const uint8_t defaults[] = {0x81, 160, 0xa6};
        if (!oled_send(0, defaults, sizeof(defaults))) { oled_on = 0; return; }
        dashboard = 0;
    }
    const uint8_t *frame = oled_frame_index ? oled_boot_description : oled_boot_logo;
    unsigned start = 128u * page;
    for (unsigned i = start; i < start + 128u; ++i) framebuffer[i] = frame[i];
    if (flush_page(page) && ++oled_next_page == 8) {
        oled_next_page = 0;
        oled_frame_pending = 0;
        oled_frame_since = ticks;
    }
}
#else
#define oled_host_takeover() ((void)0)
#endif
static void draw_row(unsigned row) {
    uint8_t *pixels = framebuffer + row*128;
    for (unsigned x = 0; x < 128; ++x) pixels[x] = 0;
    for (unsigned x = 0; x < 21 && rows[row][x]; ++x) {
        unsigned c = (unsigned char)rows[row][x];
        if (c < 32 || c > 126) c = '?';
        for (unsigned col = 0; col < 5; ++col) pixels[x*6 + col] = font[c-32][col];
    }
    (void)flush_page(row);
}
static void set_row(unsigned row, const char *s) {
    unsigned i = 0;
    for (; i < 21 && s[i]; ++i) rows[row][i] = s[i];
    rows[row][i] = 0;
    draw_row(row);
}
static void oled_init(void) {
    oled_addr = 0; oled_on = 0;
#ifdef SENSOR_PROFILE
    oled_bus = 0;
#endif
    OPIN(0) = 1;
    for (unsigned i = 0; i < 9; ++i) { OPIN(1) = 0; delay(); (void)scl_high(); }
    i2c_stop();
    for (uint8_t addr = 0x3c; addr <= 0x3d; ++addr) {
        int ack = i2c_start() && i2c_byte((uint8_t)(addr << 1));
        i2c_stop();
        if (ack) { oled_addr = addr; break; }
    }
#ifdef SENSOR_PROFILE
    if (!oled_addr) {
        /* PB0/PB1 are the documented Arduino A4/SDA and A5/SCL pair.
         * Probe only OLED addresses and restore the pins if none responds. */
        uint32_t mode = REG(0x40004040u), mux = SYS(0x38);
        uint32_t pull = REG(0x40004070u), dout = REG(0x40004048u);
        uint32_t digital = REG(0x40004060u);
        SYS(0x100) = 0x59; SYS(0x100) = 0x16; SYS(0x100) = 0x88;
        SYS(0x38) = mux & ~255u;
        SYS(0x100) = 0;
        REG(0x40004048u) = dout | 3u;
        REG(0x40004040u) = (mode & ~15u) | 10u;
        REG(0x40004070u) = (pull & ~15u) | 5u;
        REG(0x40004060u) = digital & ~(3u << 16);
        oled_bus = 1;
        OPIN(0) = 1;
        for (unsigned i = 0; i < 9; ++i) { OPIN(1) = 0; delay(); (void)scl_high(); }
        i2c_stop();
        for (uint8_t addr = 0x3c; addr <= 0x3d; ++addr) {
            int ack = i2c_start() && i2c_byte((uint8_t)(addr << 1));
            i2c_stop();
            if (ack) { oled_addr = addr; break; }
        }
        if (!oled_addr) {
            REG(0x40004040u) = mode;
            REG(0x40004048u) = dout;
            REG(0x40004070u) = pull;
            REG(0x40004060u) = digital;
            SYS(0x100) = 0x59; SYS(0x100) = 0x16; SYS(0x100) = 0x88;
            SYS(0x38) = mux;
            SYS(0x100) = 0;
            oled_bus = 0;
        }
    }
#endif
    static const uint8_t init[] = {
        0xae, 0xd5, 0x80, 0xa8, 0x3f, 0xd3, 0x00, 0x40,
        0x8d, 0x14, 0x20, 0x02, 0xa1, 0xc8, 0xda, 0x12,
        0x81, 0x7f, 0xd9, 0xf1, 0xdb, 0x40, 0xa4, 0xa6
    };
    if (!oled_send(0, init, sizeof(init))) return;
    if (sh1106) {
        const uint8_t power[] = {0xad, 0x8b};
        if (!oled_send(0, power, sizeof(power))) return;
    }
    for (unsigned row = 0; row < 8; ++row) draw_row(row);
    const uint8_t on[] = {0xaf};
    oled_on = (uint8_t)oled_send(0, on, 1);
}
static char *append_num(char *p, unsigned n) {
    if (n == 100) { *p++ = '1'; *p++ = '0'; *p++ = '0'; }
    else { if (n >= 10) *p++ = (char)('0' + n/10); *p++ = (char)('0' + n%10); }
    *p = 0; return p;
}
static void draw_dashboard(void) {
    char line[22], *p = line;
    *p++ = 'R'; *p++ = ' '; p = append_num(p, levels[0]); *p++ = ' ';
    *p++ = 'G'; *p++ = ' '; p = append_num(p, levels[1]); *p++ = ' ';
    *p++ = 'B'; *p++ = ' '; p = append_num(p, levels[2]);
    set_row(2, line);
    p = line; *p++ = 'W'; *p++ = ' '; p = append_num(p, levels[3]);
    *p++ = ' '; *p++ = 'C'; *p++ = ' '; p = append_num(p, levels[4]);
    set_row(4, line);
}
#ifdef STANDALONE_DEMO
#include "standalone_demo.h"
#endif
static void status(void) {
    uint8_t snapshot[5];
    uint32_t period, steps, count;
    __asm volatile("cpsid i" ::: "memory");
    for (unsigned i = 0; i < 5; ++i) snapshot[i] = levels[i];
    period = cycle_period; steps = cycle_steps; count = cycle_count;
    __asm volatile("cpsie i" ::: "memory");
    puts_uart("OK RGB");
    for (unsigned i = 0; i < 5; ++i) {
        if (i == 3) puts_uart(" WHITE");
        putc_uart(' '); number(snapshot[i]);
    }
    puts_uart(" OLED ");
    puts_uart(oled_addr ? (oled_on ? "ON" : "OFF") : "NOT_FOUND");
    puts_uart(" ADDR "); number(oled_addr);
    puts_uart(sh1106 ? " MODEL SH1106" : " MODEL SSD1306");
    if (period) {
        puts_uart(" CYCLE_US "); number(period*50u);
        puts_uart(" STEPS "); number(steps);
        puts_uart(" PALETTE "); number(count);
    }
#ifdef DT57_PWM
    puts_uart(" LAMP "); number(lamp_level);
    puts_uart(" PWM_HZ 20000");
#ifdef DT57_RELAY_ACTIVE_LEVEL
    puts_uart(relay_on ? " RELAY ON PIN PC2 ACTIVE " : " RELAY OFF PIN PC2 ACTIVE ");
    puts_uart(DT57_RELAY_ACTIVE_LEVEL ? "HIGH" : "LOW");
    puts_uart(" LEVEL "); number(CPIN(2) & 1u);
#endif
#endif
    puts_uart("\r\n");
}
static void command(char *line) {
    char *cursor = line, *cmd = token(&cursor);
    if (!cmd) return;
#ifdef STANDALONE_DEMO
    /* Production outputs and startup are controlled only by ring traffic. */
    const char *argument=cursor;while(*argument==' ')++argument;
    int diagnostic=(eq(cmd,"STATUS") || eq(cmd,"HELP") || eq(cmd,"INPUTS")) && !*argument;
    diagnostic |= eq(cmd,"DEMO") && (eq(argument,"STATUS") || eq(argument,"FRAME"));
    diagnostic |= eq(cmd,"RING") && (eq(argument,"STATUS") || eq(argument,"READ"));
    diagnostic |= eq(cmd,"LINK") && eq(argument,"STATUS");
    if(!diagnostic){puts_uart("ERR autonomous mode: USB diagnostics are read-only\r\n");return;}
    if(eq(cmd,"HELP")){puts_uart("OK HELP STATUS | DEMO STATUS/FRAME | RING STATUS/READ | LINK STATUS | INPUTS; autonomous ring control\r\n");return;}
#endif
#ifdef STANDALONE_DEMO
    if (eq(cmd, "DEMO")) {
        char *operation = token(&cursor);
        if (!operation || token(&cursor)) goto invalid;
        if (eq(operation, "PAUSE")) demo_active = 0;
        else if (eq(operation, "RUN")) { demo_active = 1; demo_publish_tick = ticks; }
#if HEADER_RING_NODE_ID == 3
        else if (eq(operation, "POLICE")) demo_police_start();
#endif
        else if (eq(operation, "FRAME")) {
            static const char hex[] = "0123456789ABCDEF";
            puts_uart("OK DEMO FRAME ");
            for (unsigned i = 0; i < sizeof(framebuffer); ++i) {
                putc_uart(hex[framebuffer[i] >> 4]); putc_uart(hex[framebuffer[i] & 15]);
            }
            puts_uart("\r\n"); return;
        } else if (!eq(operation, "STATUS")) goto invalid;
        puts_uart("OK DEMO NODE "); number(HEADER_RING_NODE_ID);
        puts_uart(" RUNNING "); number(demo_active);
        puts_uart(" TX_PACKETS "); number(demo_tx_packets);
        puts_uart(" RX_PACKETS "); number(demo_rx_packets);
        puts_uart(" ACKS "); number(demo_acks);
        puts_uart(" LUX "); number(demo_lux);
        puts_uart(" PIR "); number(demo_pir);
        puts_uart(" VALID "); number(demo_flags & 3u);
        puts_uart(" RELAY "); number(demo_relay);
        puts_uart(" PWM "); number(demo_pwm);
#ifdef DT57_PWM
        puts_uart(" PWM_TARGET "); number(demo_target);
#ifdef DEMO_FIXED_DIMMER
        puts_uart(" FIXED_DIMMER "); number(1);
#else
        puts_uart(" FIXED_DIMMER "); number(0);
#endif
#endif
        puts_uart(" OLED_FRAMES "); number(demo_oled_frames);
        puts_uart(" AGE_MS "); number((ticks-demo_received_tick)/20u);
        puts_uart(" TIMER_MS "); number(demo_timer_ms(ticks));
        puts_uart(" TIMER_EVENT "); number(demo_event_id);
        puts_uart(" TIMER_ACTIVE "); number(demo_relay && demo_timer_ms(ticks)<10000u);
        puts_uart(" UPTIME_MS "); number(ticks/20u);
        puts_uart(" WARMUP_MS "); number(demo_warmup_ms(ticks));
        puts_uart(" READY_MASK "); number(demo_ready_mask(ticks));
        puts_uart(" PIR_ARMED "); number(!!(demo_flags&8u));
#ifdef SENSOR_PROFILE
        puts_uart(" MOTION_SEQ "); number(demo_motion.sequence);
        puts_uart(" PENDING "); number(demo_motion.pending);
        puts_uart(" PIR_HIGH "); number(demo_motion.high);
        puts_uart(" PIR_SAMPLES "); number(demo_pir_samples);
        puts_uart(" LUX_SAMPLES "); number(demo_lux_samples);
#ifdef DEMO_PB5_DISPLAY
        puts_uart(" PB5_DISPLAY "); number(1);
        puts_uart(" PB5_LEVEL "); number(demo_pb5_level);
        puts_uart(" PB5_LEVEL_MS "); number((ticks-demo_pb5_since)/20u);
        puts_uart(" PB5_LAST_HIGH_MS "); number(demo_pb5_last_high_ms);
#endif
#else
        puts_uart(" MOTION_SEQ "); number(demo_motion_sequence);
#endif
#if HEADER_RING_NODE_ID == 3
        puts_uart(" STROBE_MS "); number((demo_strobe_remaining+19u)/20u);
        puts_uart(" STROBE_STARTS "); number(demo_strobe_starts);
        puts_uart(" WHITE_WARM "); number(demo_white_warm);
        puts_uart(" WHITE_COOL "); number(demo_white_cool);
        puts_uart(" WHITE_TARGET "); number(demo_white_target);
#endif
        puts_uart("\r\n"); return;
    }
#endif
#ifdef HEADER_RING_NODE_ID
    if (eq(cmd, "RING")) {
        char *operation = token(&cursor);
        if (!operation) goto invalid;
        if (eq(operation, "SEND")) {
            unsigned destination, sequence, ttl, count = 0;
            uint8_t payload[16];
            if (!ring_active || !parse_uint(token(&cursor), &destination, 254) || !destination ||
                !parse_uint(token(&cursor), &sequence, 255) ||
                !parse_uint(token(&cursor), &ttl, 16) || !ttl) goto invalid;
            char *hex = token(&cursor);
            if (!hex || !*hex || token(&cursor)) goto invalid;
            while (*hex) {
                if (count == sizeof(payload) || !hex[1]) goto invalid;
                int high = hex_digit(*hex++), low = hex_digit(*hex++);
                if (high < 0 || low < 0) goto invalid;
                payload[count++] = (uint8_t)((high << 4) | low);
            }
            if (!ring_send(destination, sequence, ttl, payload, count)) {
                puts_uart("ERR ring transmit queue full\r\n"); return;
            }
            puts_uart("OK RING QUEUED "); number(count); puts_uart("\r\n"); return;
        }
        if (token(&cursor)) goto invalid;
        if (eq(operation, "ON")) ring_start();
        else if (eq(operation, "OFF")) { ring_active = 0; link_disable(); }
        else if (eq(operation, "READ")) {
            if (!ring_last_size) { puts_uart("OK RING EMPTY\r\n"); return; }
            static const char hex[] = "0123456789ABCDEF";
            puts_uart("OK RING FRAME ");
            for (unsigned i = 0; i < ring_last_size; ++i) {
                putc_uart(hex[ring_last[i] >> 4]); putc_uart(hex[ring_last[i] & 15]);
            }
            ring_last_size = 0;
            puts_uart("\r\n"); return;
        } else if (!eq(operation, "STATUS")) goto invalid;
        puts_uart("OK RING NODE "); number(HEADER_RING_NODE_ID);
        puts_uart(" ACTIVE "); number(ring_active);
        puts_uart(" DELIVERED "); number(ring_delivered);
        puts_uart(" FORWARDED "); number(ring_forwarded);
        puts_uart(" EXPIRED "); number(ring_expired);
        puts_uart(" CRC_ERRORS "); number(ring_crc_errors);
        puts_uart(" PARSE_ERRORS "); number(ring_parse_errors);
        puts_uart("\r\n"); return;
    }
#endif
#ifdef DALI_HEADER_LINK
    if (eq(cmd, "LINK")) {
        char *operation = token(&cursor);
        if (!operation) goto invalid;
#ifdef HEADER_RING_NODE_ID
        if (eq(operation, "OFF")) ring_active = 0;
        if (ring_active && (eq(operation, "SEND") || eq(operation, "READ") || eq(operation, "CLEAR"))) {
            puts_uart("ERR ring owns link; use RING commands\r\n"); return;
        }
#endif
        if (eq(operation, "SEND")) {
            char *payload = token(&cursor);
            unsigned count = 0;
            uint8_t bytes[48];
            if (!link_enabled || !payload || !*payload || token(&cursor)) goto invalid;
            while (*payload) {
                if (count == sizeof(bytes) || !payload[1]) goto invalid;
                int high = hex_digit(*payload++), low = hex_digit(*payload++);
                if (high < 0 || low < 0) goto invalid;
                bytes[count++] = (uint8_t)((high << 4) | low);
            }
            if (!link_enqueue(bytes, count)) {
                puts_uart("ERR link transmit queue full\r\n"); return;
            }
            puts_uart("OK LINK QUEUED "); number(count); puts_uart("\r\n"); return;
        }
        if (token(&cursor)) goto invalid;
        if (eq(operation, "ON")) link_enable();
        else if (eq(operation, "OFF")) link_disable();
        else if (eq(operation, "CLEAR")) {
            link_rx_tail = link_rx_head;
        } else if (eq(operation, "READ")) {
            static const char hex[] = "0123456789ABCDEF";
            uint8_t bytes[48]; unsigned count = 0;
            while (link_rx_tail != link_rx_head && count < sizeof(bytes)) {
                bytes[count++] = link_rx_queue[link_rx_tail];
                link_rx_tail = (link_rx_tail + 1u) & 127u;
            }
            puts_uart("OK LINK DATA "); number(count); putc_uart(' ');
            for (unsigned i = 0; i < count; ++i) {
                putc_uart(hex[bytes[i] >> 4]); putc_uart(hex[bytes[i] & 15]);
            }
            puts_uart("\r\n"); return;
        } else if (!eq(operation, "STATUS")) goto invalid;
        puts_uart(link_enabled ? "OK LINK ON" : "OK LINK OFF");
        puts_uart(" TX PC14 RX PB15 BAUD 1000 TX_LEVEL "); number((PC(0x10) >> 14) & 1u);
        puts_uart(" RX_LEVEL "); number((REG(0x40004050u) >> 15) & 1u);
        puts_uart(" TX_STARTED "); number(link_sent);
        puts_uart(" RX_BYTES "); number(link_received);
        puts_uart(" FRAMING "); number(link_framing);
        puts_uart(" OVERFLOW "); number(link_overflow);
        puts_uart(" RX_EDGES "); number(link_edges);
        puts_uart("\r\n"); return;
    }
#endif
#ifdef DISPLAY_ONLY
    if (eq(cmd, "PALETTE") || eq(cmd, "STROBE") || eq(cmd, "CYCLE") ||
        eq(cmd, "FADE") || eq(cmd, "RGB") || eq(cmd, "WHITE") || eq(cmd, "OFF")) {
        puts_uart("ERR lighting unavailable in display-only profile\r\n"); return;
    }
#endif
#ifdef INPUT_DIAG
#ifdef SENSOR_PROFILE
    if (eq(cmd, "I2C")) {
        char *operation = token(&cursor);
        if (!operation) goto invalid;
        int alternate = eq(operation, "ALT");
        if (alternate) operation = token(&cursor);
        if (!operation) goto invalid;
        uint8_t original_bus = oled_bus;
        if (eq(operation, "SCAN")) {
            if (token(&cursor)) goto invalid;
            if (alternate) oled_bus ^= 1u; /* PC0/PC1 is already open-drain. */
            puts_uart("OK I2C ADDRESSES");
            for (unsigned address = 8; address < 120; ++address) {
                int ack = i2c_start() && i2c_byte((uint8_t)(address << 1));
                i2c_stop();
                if (ack) { putc_uart(' '); number(address); }
            }
            oled_bus = original_bus;
            puts_uart("\r\n"); return;
        }
        unsigned address, reg = 0, count;
        if (!parse_uint(token(&cursor), &address, 119) || address < 8 ||
            address == 0x3c || address == 0x3d) goto invalid; /* protect OLED */
        uint8_t bytes[16];
        int ok;
        if (eq(operation, "WRITE")) {
            char *hex = token(&cursor);
            if (!hex || token(&cursor)) goto invalid;
            unsigned length = 0; while (hex[length]) ++length;
            if (!length || length > 32 || (length & 1u)) goto invalid;
            count = length / 2;
            for (unsigned i = 0; i < count; ++i) {
                int high = hex_digit(hex[2*i]), low = hex_digit(hex[2*i+1]);
                if (high < 0 || low < 0) goto invalid;
                bytes[i] = (uint8_t)((high << 4) | low);
            }
            if (alternate) oled_bus ^= 1u;
            ok = i2c_start() && i2c_byte((uint8_t)(address << 1));
            for (unsigned i = 0; ok && i < count; ++i) ok = i2c_byte(bytes[i]);
            i2c_stop();
            oled_bus = original_bus;
            if (!ok) { puts_uart("ERR I2C NACK or bus timeout\r\n"); return; }
            puts_uart("OK I2C WRITE "); number(address); puts_uart("\r\n"); return;
        }
        int register_read = eq(operation, "READ");
        if (!register_read && !eq(operation, "RAW")) goto invalid;
        if (register_read && !parse_uint(token(&cursor), &reg, 255)) goto invalid;
        if (!parse_uint(token(&cursor), &count, 16) || !count || token(&cursor)) goto invalid;
        if (alternate) oled_bus ^= 1u;
        ok = 1;
        if (register_read)
            ok = i2c_start() && i2c_byte((uint8_t)(address << 1)) && i2c_byte((uint8_t)reg);
        if (ok) ok = i2c_start() && i2c_byte((uint8_t)((address << 1) | 1u));
        for (unsigned i = 0; ok && i < count; ++i) ok = i2c_read_byte(&bytes[i], i+1 == count);
        i2c_stop();
        oled_bus = original_bus;
        if (!ok) { puts_uart("ERR I2C NACK or bus timeout\r\n"); return; }
        static const char hex[] = "0123456789ABCDEF";
        puts_uart("OK I2C DATA "); number(address); putc_uart(' ');
        for (unsigned i = 0; i < count; ++i) { putc_uart(hex[bytes[i] >> 4]); putc_uart(hex[bytes[i] & 15]); }
        puts_uart("\r\n"); return;
    }
#endif
    if (eq(cmd, "ANALOG")) {
        char *mode = token(&cursor);
        unsigned first = mode ? 8 : 0, end = mode ? 12 : 8;
        if (mode && !eq(mode, "EXTRA")) {
#ifdef SENSOR_PROFILE
            /* PB12/PB13 remain UART. Scan only the unused ADC tail pins. */
            if (!eq(mode, "TAIL")) goto invalid;
            first = 14; end = 16;
#else
            goto invalid;
#endif
        }
        if (token(&cursor)) goto invalid;
#ifdef DALI_HEADER_LINK
        if (link_enabled && end > 15) {
            puts_uart("ERR PB15 reserved while header link enabled\r\n"); return;
        }
#endif
        unsigned values[8];
        for (unsigned channel = first; channel < end; ++channel) {
            int value = analog_read(channel);
            if (value < 0) { puts_uart("ERR ADC conversion timed out\r\n"); return; }
            values[channel-first] = (unsigned)value;
        }
        puts_uart("OK ANALOG");
        for (unsigned channel = first; channel < end; ++channel) {
            puts_uart(" PB"); number(channel); putc_uart(' '); number(values[channel-first]);
        }
        puts_uart("\r\n"); return;
    }
    if (eq(cmd, "INPUTS")) {
        if (token(&cursor)) goto invalid;
        uint32_t pins[5] = {PA(0x10), REG(0x40004050u), PC(0x10),
                            REG(0x400040d0u), REG(0x40004150u)};
        static const char ports[] = "ABCDF";
        puts_uart("OK INPUTS");
        for (unsigned i = 0; i < 5; ++i) {
            puts_uart(" P"); putc_uart(ports[i]); putc_uart(' ');
            number(pins[i] & 65535u);
        }
        puts_uart("\r\n"); return;
    }
#endif
#ifdef DT57_PWM
#ifdef DT57_RELAY_ACTIVE_LEVEL
    if (eq(cmd, "RELAY")) {
        char *value = token(&cursor);
        if (!value || (!eq(value, "ON") && !eq(value, "OFF")) || token(&cursor)) goto invalid;
        relay_set(eq(value, "ON"));
        set_row(4, relay_on ? "Relay 1: ON" : "Relay 1: OFF");
        status(); return;
    }
#endif
    if (eq(cmd, "PROBE") || eq(cmd, "DRIVE")) {
        int drive = eq(cmd, "DRIVE");
        char *port = token(&cursor);
        unsigned pin, pull, duration;
        if (!port || (!eq(port, "A") && !eq(port, "C")) ||
            !parse_uint(token(&cursor), &pin, 7) ||
            !parse_uint(token(&cursor), &pull, 1) ||
            !parse_uint(token(&cursor), &duration, 8000) || duration < 50 || token(&cursor)) goto invalid;
        if ((!drive && duration > 2000) ||
            (drive && duration > 1000 && !(eq(port, "C") && pin == 2))) goto invalid;
        /* Exclude OLED, UART, analog inputs, DALI bus and PA6 PWM.
         * DRIVE also requires fresh weak-pull tests to follow both levels. */
        int port_a = eq(port, "A");
#ifdef DT57_RELAY_ACTIVE_LEVEL
        if (!port_a && pin == 2) goto invalid; /* use RELAY; preserve live state */
#endif
        if ((port_a && pin == 6) || (!port_a && (pin < 2 || pin > 5))) goto invalid;
        uint32_t base = port_a ? 0x40004000u : 0x40004080u;
        unsigned mfp = port_a ? 0x30 : 0x40;
        uint32_t mode = REG(base), bias = REG(base + 0x30), mux = SYS(mfp), dout = REG(base + 8);
        if (drive && ((mode >> (2u*pin)) & 3u)) goto invalid;
        static char label[] = "PROBE PA0 0";
        const char *title = drive ? "DRIVE" : "PROBE";
        for (unsigned i = 0; i < 5; ++i) label[i] = title[i];
        label[7] = port_a ? 'A' : 'C'; label[8] = (char)('0' + pin); label[10] = (char)('0' + pull);
        set_row(2, label); set_row(4, drive ? "Auto restore on end" : "1=weak up 0=down");
        REG(base) = mode & ~(3u << (2u*pin));
        SYS(0x100) = 0x59; SYS(0x100) = 0x16; SYS(0x100) = 0x88;
        SYS(mfp) = mux & ~(15u << (4u*pin));
        SYS(0x100) = 0;
        int allowed = 1;
        if (drive) {
            REG(base + 0x30) = (bias & ~(3u << (2u*pin))) | (1u << (2u*pin));
            wait_ms(10);
            allowed = !!(REG(base + 0x10) & (1u << pin));
            REG(base + 0x30) = (bias & ~(3u << (2u*pin))) | (2u << (2u*pin));
            wait_ms(10);
            allowed = allowed && !(REG(base + 0x10) & (1u << pin));
            if (allowed) {
                REG(base + 0x30) = bias & ~(3u << (2u*pin));
                REG(base + 8) = (dout & ~(1u << pin)) | (pull << pin);
                REG(base) = (mode & ~(3u << (2u*pin))) | (1u << (2u*pin));
                wait_ms(1);
                allowed = ((REG(base + 0x10) >> pin) & 1u) == pull;
                if (allowed) wait_ms(duration-1);
            }
        } else {
            REG(base + 0x30) = (bias & ~(3u << (2u*pin))) | ((pull ? 1u : 2u) << (2u*pin));
            wait_ms(duration);
        }
        unsigned observed = (REG(base + 0x10) >> pin) & 1u;
        REG(base) = mode;
        if (drive) REG(base + 8) = dout;
        REG(base + 0x30) = bias;
        SYS(0x100) = 0x59; SYS(0x100) = 0x16; SYS(0x100) = 0x88;
        SYS(mfp) = mux;
        SYS(0x100) = 0;
        if (REG(base) != mode || REG(base + 0x30) != bias || SYS(mfp) != mux ||
            (drive && REG(base + 8) != dout)) {
            REG(base) &= ~(3u << (2u*pin));
            puts_uart("ERR pin restore verification failed\r\n"); return;
        }
        if (!allowed) { puts_uart("ERR pin did not follow test levels; restored\r\n"); return; }
        puts_uart("OK "); puts_uart(title); puts_uart(" P"); putc_uart(port_a ? 'A' : 'C'); number(pin);
        puts_uart(" PULL "); number(pull); puts_uart(" LEVEL "); number(observed);
        puts_uart(" RESTORED 1\r\n"); return;
    }
    if (eq(cmd, "LAMP")) {
        unsigned level;
        if (!parse_level(token(&cursor), &level) || token(&cursor)) goto invalid;
        lamp_set(level);
        static char text[] = "PWM duty: 000%";
        text[10] = (char)('0' + level / 100);
        text[11] = (char)('0' + (level / 10) % 10);
        text[12] = (char)('0' + level % 10);
        set_row(6, text);
        status(); return;
    }
#endif
    if (eq(cmd, "PALETTE")) {
        char *first = token(&cursor);
        if (!first) goto invalid;
        if (eq(first, "CLEAR")) {
            if (token(&cursor)) goto invalid;
            palette_ready = 0;
        } else {
            unsigned index, values[5];
            if (!parse_uint(first, &index, 31)) goto invalid;
            for (unsigned i = 0; i < 5; ++i)
                if (!parse_level(token(&cursor), &values[i])) goto invalid;
            if (token(&cursor)) goto invalid;
            for (unsigned i = 0; i < 5; ++i) palette_staging[index][i] = (uint8_t)values[i];
            palette_ready |= 1u << index;
        }
        puts_uart("OK PALETTE\r\n"); return;
    } else if (eq(cmd, "STROBE")) {
        unsigned period, count;
        if (!parse_uint(token(&cursor), &period, 60000000) || period < 50 || period % 50 ||
            !parse_uint(token(&cursor), &count, 32) || !count || token(&cursor)) goto invalid;
        uint32_t required = count == 32 ? 0xffffffffu : (1u << count)-1u;
        if ((palette_ready & required) != required) goto invalid;
        __asm volatile("cpsid i" ::: "memory");
        fade_duration = 0; cycle_period = 0;
        for (unsigned n = 0; n < count; ++n)
            for (unsigned i = 0; i < 5; ++i) cycle_colors[n][i] = palette_staging[n][i];
        for (unsigned i = 0; i < 5; ++i) levels[i] = cycle_colors[0][i];
        cycle_count = (uint8_t)count; cycle_phase = 0; cycle_elapsed = 0; cycle_steps = 0;
        palette_ready = 0; cycle_period = period/50;
        __asm volatile("cpsie i" ::: "memory");
        dashboard = 0;
    } else if (eq(cmd, "CYCLE")) {
        unsigned period, brightness, white;
        if (!parse_uint(token(&cursor), &period, 60000000) || period < 50 || period % 50 ||
            !parse_level(token(&cursor), &brightness) || !parse_level(token(&cursor), &white) ||
            token(&cursor)) goto invalid;
        __asm volatile("cpsid i" ::: "memory");
        fade_duration = 0; cycle_period = 0;
        static const uint8_t colors[6] = {4, 6, 2, 3, 1, 5};
        for (unsigned n = 0; n < 6; ++n) {
            cycle_colors[n][0] = (colors[n] & 4) ? (uint8_t)brightness : 0;
            cycle_colors[n][1] = (colors[n] & 2) ? (uint8_t)brightness : 0;
            cycle_colors[n][2] = (colors[n] & 1) ? (uint8_t)brightness : 0;
            cycle_colors[n][3] = (n & 1) ? 0 : (uint8_t)white;
            cycle_colors[n][4] = (n & 1) ? (uint8_t)white : 0;
        }
        cycle_count = 6;
        cycle_phase = 0; cycle_elapsed = 0; cycle_steps = 0;
        levels[0] = (uint8_t)brightness; levels[1] = 0; levels[2] = 0;
        levels[3] = (uint8_t)white; levels[4] = 0;
        cycle_period = period/50;
        __asm volatile("cpsie i" ::: "memory");
        dashboard = 0;
    } else if (eq(cmd, "BUFFER")) {
        unsigned offset, count = 0;
        if (!parse_uint(token(&cursor), &offset, 1023)) goto invalid;
        char *hex = token(&cursor);
        if (!hex || token(&cursor)) goto invalid;
        while (hex[count]) { if (hex_digit(hex[count]) < 0 || count >= 96) goto invalid; ++count; }
        if (!count || count % 2 || offset + count/2 > 1024) goto invalid;
        oled_host_takeover();
        dashboard = 0;
        for (unsigned i = 0; i < count/2; ++i) {
            framebuffer[offset+i] = (uint8_t)((hex_digit(hex[2*i]) << 4) | hex_digit(hex[2*i+1]));
            dirty_pages |= (uint8_t)(1u << ((offset+i)/128));
        }
        puts_uart("OK BUFFER\r\n"); return;
    } else if (eq(cmd, "PRESENT")) {
        if (token(&cursor)) goto invalid;
        oled_host_takeover();
        for (unsigned page = 0; page < 8; ++page)
            if ((dirty_pages & (1u << page)) && !flush_page(page)) {
                puts_uart("ERR OLED write failed\r\n"); return;
            }
    } else if (eq(cmd, "FADE")) {
        unsigned duration, values[5];
        if (!parse_uint(token(&cursor), &duration, 600000)) goto invalid;
        for (unsigned i = 0; i < 5; ++i)
            if (!parse_level(token(&cursor), &values[i])) goto invalid;
        if (token(&cursor)) goto invalid;
        __asm volatile("cpsid i" ::: "memory");
        fade_duration = 0; fade_elapsed = 0; cycle_period = 0;
        for (unsigned i = 0; i < 5; ++i) {
            fade_start[i] = levels[i]; fade_target[i] = (uint8_t)values[i];
            if (!duration) levels[i] = (uint8_t)values[i];
        }
        fade_duration = duration;
        __asm volatile("cpsie i" ::: "memory");
        dashboard = 0;
    } else if (eq(cmd, "RGB") || eq(cmd, "WHITE")) {
        unsigned n = eq(cmd, "RGB") ? 3 : 2, values[3];
        for (unsigned i = 0; i < n; ++i) {
            if (!parse_level(token(&cursor), &values[i])) goto invalid;
        }
        if (token(&cursor)) goto invalid;
        unsigned start = n == 3 ? 0 : 3;
        __asm volatile("cpsid i" ::: "memory");
        fade_duration = 0;
        cycle_period = 0;
        for (unsigned i = 0; i < n; ++i) levels[start+i] = (uint8_t)values[i];
        __asm volatile("cpsie i" ::: "memory");
        if (dashboard) draw_dashboard();
    } else if (eq(cmd, "OFF")) {
        if (token(&cursor)) goto invalid;
        fade_duration = 0;
        cycle_period = 0;
        for (unsigned i = 0; i < 5; ++i) levels[i] = 0;
        if (dashboard) draw_dashboard();
    } else if (eq(cmd, "TEXT")) {
        unsigned row;
        if (!parse_level(token(&cursor), &row) || row > 7) goto invalid;
        while (*cursor == ' ') ++cursor;
        unsigned length = 0; while (cursor[length]) ++length;
        if (length > 21) goto invalid;
        oled_host_takeover();
        dashboard = 0; set_row(row, cursor);
    } else if (eq(cmd, "CLEAR")) {
        if (token(&cursor)) goto invalid;
        oled_host_takeover();
        dashboard = 0;
        for (unsigned row = 0; row < 8; ++row) set_row(row, "");
    } else if (eq(cmd, "DASHBOARD")) {
        if (token(&cursor)) goto invalid;
        oled_host_takeover();
        dashboard = 1;
        for (unsigned row = 0; row < 8; ++row) rows[row][0] = 0;
        set_row(0, "NuMaker USB Control"); set_row(6, "115200 baud - ready");
        for (unsigned row = 1; row < 8; row += 2) draw_row(row);
        draw_dashboard();
    } else if (eq(cmd, "OLED")) {
        char *value = token(&cursor);
        if (!value) goto invalid;
#ifdef OLED_BOOT_SHOW
        if (eq(value, "BOOT")) {
            if (token(&cursor)) goto invalid;
            puts_uart("OK OLED BOOT ");
            if (!oled_on) puts_uart("OFF");
            else if (oled_host_active) puts_uart("HOST");
            else if (oled_frame_pending) puts_uart("DRAWING");
            else puts_uart(oled_frame_index ? "DESCRIPTION" : "LOGO");
            puts_uart("\r\n"); return;
        }
#endif
#ifdef DISPLAY_ONLY
        if (eq(value, "PROBE")) {
            if (token(&cursor)) goto invalid;
            OPIN(0) = 1; (void)scl_high();
            puts_uart("OK BUS");
#ifdef SENSOR_PROFILE
            puts_uart(oled_bus ? " PB0/PB1" : " PC0/PC1");
#endif
            puts_uart(" SDA "); number(OLED_INPUT & 1u);
            puts_uart(" SCL "); number((OLED_INPUT >> 1) & 1u);
            puts_uart(" ADDRESSES");
            for (unsigned addr = 8; addr < 120; ++addr) {
                int ack = i2c_start() && i2c_byte((uint8_t)(addr << 1));
                i2c_stop();
                if (ack) { putc_uart(' '); number(addr); }
            }
            puts_uart("\r\n"); return;
        }
#endif
        if (eq(value, "CONTRAST") || eq(value, "INVERT")) {
            unsigned number_value;
            int contrast = eq(value, "CONTRAST");
            if (!parse_uint(token(&cursor), &number_value, contrast ? 255 : 1) || token(&cursor)) goto invalid;
            oled_host_takeover();
            uint8_t bytes[] = {contrast ? 0x81 : (uint8_t)(number_value ? 0xa7 : 0xa6), (uint8_t)number_value};
            if (!oled_send(0, bytes, contrast ? 2 : 1)) { puts_uart("ERR OLED write failed\r\n"); return; }
            status(); return;
        }
        if (token(&cursor)) goto invalid;
        if (eq(value, "INIT")) { oled_host_takeover(); oled_init(); }
        else if (eq(value, "ON") || eq(value, "OFF")) {
            oled_host_takeover();
            uint8_t value_byte = eq(value, "ON") ? 0xaf : 0xae;
            int ok = oled_send(0, &value_byte, 1);
            oled_on = (uint8_t)(ok && value_byte == 0xaf);
        } else goto invalid;
    } else if (eq(cmd, "MODEL")) {
        char *value = token(&cursor);
        if (!value || token(&cursor)) goto invalid;
        if (eq(value, "SSD1306")) sh1106 = 0;
        else if (eq(value, "SH1106")) sh1106 = 1;
        else goto invalid;
        oled_host_takeover();
        oled_init();
    } else if (eq(cmd, "STATUS")) {
        if (token(&cursor)) goto invalid;
    } else if (eq(cmd, "HELP")) {
#ifdef HEADER_RING_NODE_ID
        puts_uart("RING ON/OFF/STATUS/READ/SEND destination sequence ttl hex | ");
#endif
#ifdef DALI_HEADER_LINK
        puts_uart("LINK ON/OFF/STATUS/CLEAR/READ/SEND hex | ");
#endif
#ifdef DT57_PWM
        puts_uart("LAMP 0..100 | INPUTS | ANALOG | ");
#ifdef DT57_RELAY_ACTIVE_LEVEL
        puts_uart("RELAY ON/OFF | ");
#endif
#endif
        puts_uart("RGB r g b | WHITE warm cool | FADE ms r g b w c | CYCLE us rgb_percent white_percent | PALETTE CLEAR/index r g b w c | STROBE us count | OFF | TEXT row text | CLEAR | DASHBOARD | BUFFER offset hex | PRESENT | OLED ON/OFF/INIT/CONTRAST n/INVERT 0|1 | MODEL SSD1306/SH1106 | STATUS\r\n");
    } else goto invalid;
    status(); return;
invalid:
    puts_uart("ERR invalid command; use HELP\r\n");
}
static void hardware_init(void) {
    SYS(0x100) = 0x59; SYS(0x100) = 0x16; SYS(0x100) = 0x88;
    CLK(0) |= 4; while (!(CLK(0x50) & 16)) {}
    /* Divide HIRC by four: 12 MHz requires no increased flash wait states. */
    CLK(0x20) = (CLK(0x20) & ~15u) | 3;
    CLK(0x10) = (CLK(0x10) & ~7u) | 7;
    CLK(4) |= 7u << 24; /* GPIOA/B/C clocks */
    CLK(8) |= 1u << 16; /* UART0 clock */
    CLK(0x14) = (CLK(0x14) & ~(7u << 24)) | (3u << 24);
    CLK(0x20) &= ~(15u << 8); /* UART0 divider = 1 */
#ifndef DISPLAY_ONLY
    SYS(0x30) &= ~0xf0000000u; /* PA7 GPIO */
    SYS(0x40) &= ~0x00ffffffu; /* PC0..5 GPIO */
#else
    SYS(0x40) &= ~0x000000ffu; /* PC0/PC1 OLED only */
#endif
    SYS(0x3c) = (SYS(0x3c) & ~0x00ff0000u) | 0x00660000u;
#ifndef DISPLAY_ONLY
    APIN(7) = 1;
    for (unsigned n = 2; n <= 5; ++n) CPIN(n) = 1;
#endif
    CPIN(0) = 1; CPIN(1) = 1;
#ifndef DISPLAY_ONLY
    PA(0) = (PA(0) & ~(3u << 14)) | (1u << 14);
    PC(0) = (PC(0) & ~0xfffu) | 0x55a;
#else
    PC(0) = (PC(0) & ~15u) | 10;
#endif
    PC(0x30) = (PC(0x30) & ~15u) | 5; /* I2C pullups */
#ifdef INPUT_DIAG
    CLK(4) |= (1u << 27) | (1u << 29); /* GPIO D/F clocks for input reads */
    CLK(0x34) &= ~(7u << 4); /* PCLK1 = HCLK = 12 MHz */
    CLK(8) |= 1u << 28; /* EADC clock */
    CLK(0x20) = (CLK(0x20) & ~(255u << 16)) | (3u << 16);
    EADC(0x110) = 128u << 8; /* ADC startup >=10 us at 3 MHz */
    EADC(0x50) = 1;
#endif
#ifdef DT57_PWM
#ifdef DT57_RELAY_ACTIVE_LEVEL
    relay_set(0); /* preload OFF before enabling the PC2 output driver */
    SYS(0x40) &= ~(15u << 8);
    PC(0x30) &= ~(3u << 4);
    PC(0) = (PC(0) & ~(3u << 4)) | (1u << 4);
#endif
    CLK(0x0c) |= 1u << 19; /* BPWM1 clock */
    CLK(0x18) |= 1u << 9; /* BPWM1 from PCLK1 */
    BPWM1(0x20) = 0;
    BPWM1(0) = 0; BPWM1(4) = 0; /* up counter, buffered compare */
    BPWM1(0x10) = 0; BPWM1(0x14) = 0; /* peripheral clock / 1 */
    BPWM1(0x30) = 599; /* 12 MHz / 600 = 20 kHz */
    BPWM1(0x5c) = 60; /* channel 3 compare: 10% */
    BPWM1(0xb0) = 2u << 6; /* channel 3 high at counter zero */
    BPWM1(0xb4) = 1u << 6; /* channel 3 low at compare up */
    BPWM1(0xd4) = 0; /* non-inverted output */
    lamp_set(0); /* mask low until initialization completes */
    BPWM1(0xd8) = 8; /* output channel 3 only */
    BPWM1(0x20) = 1;
    SYS(0x30) = (SYS(0x30) & ~(15u << 24)) | (12u << 24); /* PA6 */
#endif
    UART(8) = 6; UART(0x0c) = 3; UART(0x30) = 0;
    UART(0x24) = 0x30000000u | 415; /* HIRC / (415+2) = 115108 baud */
    SYS(0x100) = 0;
    REG(0xe000e014u) = 599; /* 12 MHz / 600 = 20 kHz */
    REG(0xe000e018u) = 0;
    REG(0xe000e010u) = 7;
    __asm volatile("cpsie i" ::: "memory");
}
static void main_loop(void) {
    hardware_init();
    wait_ms(100);
    oled_init();
#ifndef DISPLAY_ONLY
    set_row(0, "NuMaker USB Control"); set_row(6, "115200 baud - ready");
    levels[1] = 10; levels[3] = 10; levels[4] = 10;
    draw_dashboard();
#else
    dashboard = 0;
#ifdef DT57_PWM
    set_row(0, "DT5-7 OUTPUT TEST");
    set_row(2, "PA6 BPWM1 channel 3");
    set_row(4, "LAMP 0..100 percent");
#ifdef DT57_RELAY_ACTIVE_LEVEL
    set_row(4, "Relay 1: OFF");
#endif
    lamp_set(10);
    set_row(6, "PWM duty: 010%");
#else
#ifdef SENSOR_PROFILE
    set_row(0, "DALI SENSOR INPUTS");
    set_row(2, "Light / PIR mapping");
    set_row(4, "USB diagnostics ready");
#else
    set_row(0, "NuMaker OLED test");
    set_row(2, "Display-only mode");
    set_row(4, "USB serial ready");
#endif
#endif
#endif
    puts_uart("NuMaker USB control ready\r\n"); status();
#ifdef STANDALONE_DEMO
    demo_init();
#endif
    char line[128]; unsigned length = 0, discard = 0;
    for (;;) {
#ifdef HEADER_RING_NODE_ID
        ring_service();
#endif
#ifdef STANDALONE_DEMO
        demo_service();
#endif
        if (rx_overrun) { discard = 1; rx_overrun = 0; }
        if (rx_tail == rx_head) {
#if defined(OLED_BOOT_SHOW) && !defined(STANDALONE_DEMO)
            if (!length && !discard) oled_boot_update();
#endif
            continue;
        }
        char c = (char)rx[rx_tail]; rx_tail = (rx_tail + 1) & 255;
        if (c == '\n' || c == '\r') {
            if (discard) puts_uart("ERR line too long or receive overflow\r\n");
            else if (length) {
#ifdef OLED_BOOT_SHOW
                oled_last_command = ticks;
#endif
                line[length] = 0;
                command(line);
            }
            length = discard = 0;
        } else if (!discard && c >= 32 && c <= 126) {
            if (length + 1 < sizeof(line)) line[length++] = c;
            else discard = 1;
        }
    }
}
extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _stack_top;
void Reset_Handler(void) {
    uint32_t *src = &_sidata;
    for (uint32_t *dst = &_sdata; dst < &_edata;) *dst++ = *src++;
    for (uint32_t *dst = &_sbss; dst < &_ebss;) *dst++ = 0;
    main_loop();
    for (;;) {}
}
void Default_Handler(void) { for (;;) {} }
__attribute__((section(".vectors"), used))
const uintptr_t vectors[48] = {
    [0] = (uintptr_t)&_stack_top, [1] = (uintptr_t)Reset_Handler,
    [2 ... 14] = (uintptr_t)Default_Handler, [15] = (uintptr_t)SysTick_Handler,
    [16 ... 47] = (uintptr_t)Default_Handler,
};
