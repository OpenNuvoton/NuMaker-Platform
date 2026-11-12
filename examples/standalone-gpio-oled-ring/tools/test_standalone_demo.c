/* Test the actual relay-node demo policy with a simulated clock and GPIO. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../firmware/font.h"
static uint32_t registers[0x5000/4], ticks;
#define REG(a) registers[((a)-0x40000000u)/4]
#define SYS(o) REG(0x40000000u+(o))
#define PC(o) REG(0x40004080u+(o))
#define LINK_IRQ_OFF() ((void)0)
#define LINK_IRQ_ON() ((void)0)
#define HEADER_RING_NODE_ID 2
#define DT57_PWM
#define STANDALONE_DEMO
static void demo_packet(const uint8_t *, unsigned);
#include "../firmware/header_link.h"
#include "../firmware/header_ring.h"
static uint8_t framebuffer[1024], oled_on=1, dashboard=1;
static uint32_t rx_head,rx_tail;
static unsigned observed_pwm, observed_relay, pages;
static void lamp_set(unsigned n) {observed_pwm=n;}
static void relay_set(unsigned n) {observed_relay=n;}
static int flush_page(unsigned n) {assert(n<8);++pages;return 1;}
#include "../firmware/demo_effects.h"
#include "../firmware/standalone_demo.h"
static unsigned test_sequence,test_high,test_ready=3,test_warm;
static void sensor(unsigned pir, unsigned lux, unsigned flags) {
    unsigned high=pir>1800 || (flags&4u);
    if(high&&!test_high)++test_sequence;
    test_high=high;
    uint8_t frame[26]={0xa5,0x5a,1,1,2,4,42,16,0x10,(uint8_t)(flags|8u),
                       (uint8_t)pir,(uint8_t)(pir>>8),(uint8_t)lux,(uint8_t)(lux>>8),10,0,0,0,0,2,(uint8_t)test_warm,
                       (uint8_t)test_ready,(uint8_t)test_sequence,1};
    ring_seal(frame,26);
    for(unsigned i=0;i<26;++i) {
        link_rx_queue[link_rx_head]=frame[i];link_rx_head=(link_rx_head+1)&127;
    }
    ring_service();demo_service();
    /* Drain queued application telemetry without spending wall time. */
    link_tx_tail=link_tx_head;
}
static void advance(unsigned until) {
    while(ticks<until){++ticks;demo_relay_tick();}
    demo_service();
}
#ifndef DEMO_FIXED_DIMMER
int main(int argc,char **argv) {
    LINK_PB(0x10)=1u<<15;
    demo_init();link_tick();
    assert(ring_active&&link_enabled&&demo_active&&!dashboard&&!demo_acks);
    sensor(2000,400,3);
    assert(!observed_relay&&demo_target==50&&demo_tx_packets==1);
    advance(30*20000);sensor(2000,400,3);assert(!observed_relay);
    test_ready=7;sensor(2000,400,3);assert(observed_relay&&demo_event_id==1);
    advance(35*20000);sensor(2000,400,7);
    assert(demo_event_id==1&&demo_timer_ms(ticks)==5000);
    uint32_t remaining=demo_relay_remaining;
    sensor(2000,0,1);assert(demo_target==100); /* lux works with invalid PIR */
    sensor(2000,7000,1);assert(demo_target==5);
    sensor(2000,0,2);assert(demo_target==5); /* invalid lux holds target */
    assert(demo_event_id==1&&demo_timer_ms(ticks)==5000&&demo_relay_remaining==remaining);
    advance(39*20000); sensor(2000,400,7);
    assert(observed_relay==1); /* below threshold does not defeat the hold */
    advance(40*20000);sensor(2000,400,7);
    assert(observed_relay==0);
    assert(demo_timer_ms(ticks)==10000&&demo_event_id==1);
    advance(41*20000);sensor(2000,400,7);assert(observed_relay==0);
    sensor(1,400,3);sensor(1900,0,3);
    assert(observed_relay==1&&demo_target==100);
    assert(demo_event_id==2);
    advance(46*20000);sensor(1900,7000,3);assert(demo_target==5&&demo_event_id==2);
    advance(50*20000);assert(observed_relay==1);
    advance(51*20000);assert(observed_relay==0); /* fixed deadline even with sustained motion */
    unsigned target=demo_target;
    sensor(1,0,2);assert(demo_target==target); /* invalid lux freezes target */
    unsigned old_pwm=demo_pwm;
    advance(ticks+1600);
    assert(observed_pwm==old_pwm-1&&observed_pwm<=100);
    sensor(1,400,3);sensor(1,400,7);assert(observed_relay==1); /* latched brief motion */
    advance(ticks+10*20000);assert(!observed_relay);
    /* A partition must not replay a consumed request. A sensor reboot must
     * allow a new event even when its restarted sequence has the same ID. */
    unsigned event=demo_event_id;
    test_ready=3;sensor(1900,400,3);test_ready=7;sensor(1900,400,3);
    assert(!observed_relay&&demo_event_id==event);
    test_warm=30;test_ready=6;sensor(1,400,3);assert(!demo_have_motion);
    test_sequence=demo_motion_seen-1u; /* restart onto the previously used ID */
    test_warm=0;test_ready=7;sensor(1900,400,3);
    assert(observed_relay&&demo_event_id==event+1);
    demo_draw();
    unsigned lit=0;for(unsigned i=0;i<1024;++i)lit+=!!framebuffer[i];
    assert(lit>200&&demo_oled_frames>0);
    for(unsigned i=0;i<8;++i) demo_service();
    assert(pages>=8);
    unsigned saved=ticks;ticks=demo_boot_tick+1000u;assert(!demo_warmup_ms(ticks));ticks=saved;
    puts("PASS: first-trigger 10-second cutoff despite sustained high; clear/rearm; dimmer; big stopwatch OLED");
    if(argc>1) {
        demo_relay=1;demo_event_elapsed=7400;demo_event_anchor=ticks;demo_pwm=67;
        if(argc>2){demo_relay=0;demo_warm_done=0;demo_boot_tick=ticks-94000u;demo_peer_mask=1;}
        demo_draw();
        FILE *out=fopen(argv[1],"wb");assert(out);assert(fwrite(framebuffer,1,1024,out)==1024);fclose(out);
    }
}
#else
int main(void) {
    LINK_PB(0x10)=1u<<15;demo_init();link_tick();
    assert(observed_pwm==DEMO_FIXED_DIMMER&&!demo_acks);
    sensor(1,400,3);advance(15*20000);test_ready=7;
    sensor(1900,0,3);assert(observed_relay&&demo_target==100);
    uint8_t event=demo_event_id;
    for(unsigned second=16;second<=25;++second) {
        advance(second*20000);
        sensor(1900,second&1u?0:7000,3);
        assert(demo_target==(second&1u?100:5));
        assert(demo_pwm==DEMO_FIXED_DIMMER&&observed_pwm==DEMO_FIXED_DIMMER);
        assert(demo_event_id==event);
        assert(observed_relay==(second<25));
    }
    assert(demo_timer_ms(ticks)==10000);
    sensor(1,0,2);assert(observed_pwm==DEMO_FIXED_DIMMER);
    sensor(1900,7000,2);assert(observed_relay&&demo_event_id==event+1);
    assert(demo_target==100); /* invalid lux leaves its target alone */
    demo_draw();assert(demo_oled_frames&&demo_pwm==DEMO_FIXED_DIMMER);
    puts("PASS: fixed PWM through alternating/invalid lux; motion, fixed 10-second cutoff, rearm and OLED continue");
}
#endif
