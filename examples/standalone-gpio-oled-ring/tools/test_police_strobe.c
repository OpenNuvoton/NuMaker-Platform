#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define HEADER_RING_NODE_ID 3
#define LINK_IRQ_OFF() ((void)0)
#define LINK_IRQ_ON() ((void)0)
static volatile uint8_t levels[5];
static volatile uint32_t cycle_period, fade_duration, cycle_steps;
static uint32_t cycle_elapsed;
static uint8_t cycle_phase, cycle_count, cycle_colors[32][5];
#include "../firmware/demo_effects.h"
int main(void) {
    demo_police_start();
    assert(demo_strobe_remaining==200000 && cycle_period==1000 && cycle_count==8);
    for(unsigned i=0;i<8;++i) {
        assert(cycle_colors[i][0]==((i==0||i==2)?70:0));
        assert(cycle_colors[i][2]==((i==4||i==6)?70:0));
        assert(!cycle_colors[i][1]&&!cycle_colors[i][3]&&!cycle_colors[i][4]);
    }
    for(unsigned i=0;i<99999;++i)demo_police_tick();
    cycle_phase=5;
    assert(demo_strobe_remaining==100001&&cycle_phase==5&&demo_strobe_starts==1);
    for(unsigned i=0;i<100000;++i)demo_police_tick();
    assert(demo_strobe_remaining==1&&cycle_period);
    demo_police_tick();
    assert(!demo_strobe_remaining&&!cycle_period);
    for(unsigned i=0;i<5;++i)assert(!levels[i]);
    demo_police_start();assert(demo_strobe_starts==2&&demo_strobe_remaining==200000);
    demo_white_target=100;
    for(unsigned i=0;i<160000;++i)demo_lighting_tick();
    assert(demo_white_current==25600&&levels[3]==100&&levels[4]==0);
    for(unsigned i=0;i<40000;++i)demo_lighting_tick();
    assert(!demo_strobe_remaining&&!cycle_period&&levels[3]==100&&levels[4]==0);
    demo_white_target=50;
    uint32_t previous=demo_white_current;
    for(unsigned i=0;i<80000;++i) {
        demo_lighting_tick();
        assert(demo_white_current<=previous);previous=demo_white_current;
        assert(levels[3]+levels[4]<=100);
    }
    assert(levels[3]==25&&levels[4]==25);
    demo_white_target=20;
    for(unsigned i=0;i<48000;++i)demo_lighting_tick();
    assert(levels[3]==4&&levels[4]==16);
    demo_white_target=0;
    for(unsigned i=0;i<32000;++i)demo_lighting_tick();
    assert(!levels[3]&&!levels[4]);
    puts("PASS: 50 ms police flashes; 10-second hold; independent smooth warm/cool dimming");
}
