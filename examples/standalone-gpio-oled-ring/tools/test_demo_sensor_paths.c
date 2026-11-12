/* Exercise the actual sensor services with a missing/saturated ambient sensor. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../firmware/font.h"
static uint32_t registers[0x5000/4],ticks;
#define REG(a) registers[((a)-0x40000000u)/4]
#define SYS(o) REG(0x40000000u+(o))
#define PC(o) REG(0x40004080u+(o))
#define LINK_IRQ_OFF() ((void)0)
#define LINK_IRQ_ON() ((void)0)
#define HEADER_RING_NODE_ID 1
#define SENSOR_PROFILE
#define STANDALONE_DEMO
static void demo_packet(const uint8_t *,unsigned);
#include "../firmware/header_link.h"
#include "../firmware/header_ring.h"
static uint8_t framebuffer[1024],oled_on,oled_bus=1,dashboard=1;
static uint32_t rx_head,rx_tail;
static int pir_input=1,ambient_present;
static unsigned adc_reads,bus_reads,i2c_phase,i2c_reg,i2c_index,ambient_c0;
static int analog_read(unsigned channel){assert(channel==5);++adc_reads;return pir_input;}
static int i2c_start(void){i2c_phase=0;return ambient_present;}
static int i2c_byte(uint8_t value){if(i2c_phase++==1){i2c_reg=value;i2c_index=0;}return 1;}
static int i2c_read_byte(uint8_t *out,int last){
    (void)last;++bus_reads;
    if(i2c_reg==0x92)*out=0x34;
    else if(i2c_reg==0x93)*out=1;
    else {*out=i2c_index<2?(uint8_t)(ambient_c0>>(8*i2c_index)):0;++i2c_index;}
    return 1;
}
static void i2c_stop(void){}
static int flush_page(unsigned page){(void)page;return 1;}
#include "../firmware/standalone_demo.h"
static void step(unsigned count){
    while(count--){ticks+=1000;link_tick();ring_service();demo_service();link_tx_tail=link_tx_head;}
}
int main(int argc,char **argv){
    LINK_PB(0x10)=1u<<15;demo_init();
    step(299);assert(!demo_motion.armed&&demo_warmup_ms(ticks)==50);
    step(1);assert(demo_motion.armed&&!demo_warmup_ms(ticks));
    assert(adc_reads==300&&demo_lux_samples==75&&!demo_ambient_ready);
    pir_input=2000;step(3);assert(demo_motion.pending&&demo_motion.sequence==1);
    DemoMotion motion=demo_motion;
    unsigned flags=demo_flags&10u,reads=adc_reads;
    for(unsigned i=0;i<10;++i)demo_lux_sample();
    assert(!memcmp(&motion,&demo_motion,sizeof motion)&&adc_reads==reads&&(demo_flags&10u)==flags);
    /* Ambient device recovers while the first motion request is outstanding. */
    ambient_present=1;ticks+=100001;step(1);assert(demo_ambient_powered);
    step(4);assert(demo_ambient_ready&&(demo_flags&1u)&&demo_lux==0&&bus_reads);
    assert(demo_motion.pending&&demo_motion.sequence==1&&oled_bus==1);
    motion=demo_motion;ambient_c0=20000;demo_lux_sample();
    assert(!(demo_flags&1u)&&!memcmp(&motion,&demo_motion,sizeof motion));
    ambient_c0=100;demo_lux_sample();assert(demo_flags&1u);
    unsigned lux=demo_lux;pir_input=-1;demo_pir_sample();
    assert(!(demo_flags&2u)&&(demo_flags&1u)&&demo_lux==lux&&demo_motion.pending);
    demo_motion_ack(&demo_motion,1,1);pir_input=1;step(5);
    pir_input=2000;step(3);assert(demo_motion.pending&&demo_motion.sequence==2);
#ifdef DEMO_PB5_DISPLAY
    demo_pb5_track(1,ticks);demo_pb5_track(1939,ticks);demo_pir=1939;
    ticks+=392000u;demo_draw();
    if(argc>1){FILE *out=fopen(argv[1],"wb");assert(out);assert(fwrite(framebuffer,1,1024,out)==1024);fclose(out);}
    demo_pb5_track(1,ticks);assert(demo_pb5_last_high_ms==19600u&&demo_pb5_level==0);
#else
    (void)argc;(void)argv;
#endif
    puts("PASS: independent PIR/lux schedules, missing ambient sensor, nonblocking recovery, saturation, invalid PIR, motion rearm");
}
