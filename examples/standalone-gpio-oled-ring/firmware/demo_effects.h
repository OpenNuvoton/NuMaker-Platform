/* Shared motion hold: 10 seconds at the existing 20 kHz SysTick. */
#define DEMO_ON_TICKS 200000u
#if HEADER_RING_NODE_ID == 2
static volatile uint32_t demo_relay_remaining;
static void demo_relay_tick(void) {
    if(demo_relay_remaining && !--demo_relay_remaining) relay_set(0);
}
#endif
#if HEADER_RING_NODE_ID == 3
static volatile uint32_t demo_strobe_remaining;
static uint32_t demo_strobe_starts;
static volatile uint8_t demo_white_target;
static uint32_t demo_white_current, demo_white_ticks;
static uint8_t demo_white_warm, demo_white_cool;
static void demo_police_stop(void) {
    cycle_period = 0;
    demo_strobe_remaining = 0;
    for (unsigned i=0; i<3; ++i) levels[i]=0;
}
static void demo_police_start(void) {
    LINK_IRQ_OFF();
    cycle_period = fade_duration = 0;
    for (unsigned i=0; i<8; ++i)
        for (unsigned channel=0; channel<5; ++channel)
            cycle_colors[i][channel]=(uint8_t)((i==0 || i==2) && channel==0 ? 70 :
                                              ((i==4 || i==6) && channel==2 ? 70 : 0));
    cycle_count=8; cycle_phase=0; cycle_elapsed=cycle_steps=0;
    for (unsigned i=0; i<5; ++i) levels[i]=cycle_colors[0][i];
    demo_strobe_remaining=DEMO_ON_TICKS;
    ++demo_strobe_starts;
    cycle_period=1000; /* 50 ms per step: red/off/red/off/blue/off/blue/off */
    LINK_IRQ_ON();
}
static void demo_police_tick(void) {
    if (demo_strobe_remaining && !--demo_strobe_remaining) demo_police_stop();
}
static void demo_lighting_tick(void) {
    demo_police_tick();
    if (++demo_white_ticks == 400u) { /* 20 ms; ramp 0.25 percent per update */
        demo_white_ticks=0;
        uint32_t target=(uint32_t)demo_white_target*256u;
        if (demo_white_current < target) {
            unsigned delta=target-demo_white_current;
            demo_white_current += delta>64u ? 64u : delta;
        } else if (demo_white_current > target) {
            unsigned delta=demo_white_current-target;
            demo_white_current -= delta>64u ? 64u : delta;
        }
        unsigned brightness=(demo_white_current+128u)/256u;
        /* Darker -> high dimmer duty -> warm; brighter -> lower duty -> cool.
         * Warm + cool always equals the smoothed total brightness. */
        demo_white_warm=(uint8_t)((demo_white_current*demo_white_current+3276800u)/6553600u);
        demo_white_cool=(uint8_t)(brightness-demo_white_warm);
    }
    /* Restore independent white channels after the RGB palette update. */
    levels[3]=demo_white_warm; levels[4]=demo_white_cool;
}
#endif
