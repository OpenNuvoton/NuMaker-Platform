/* Autonomous packet-powered porch: sensor -> relay -> lighting -> sensor.
 * USB provides optional diagnostics only. Startup enables the ring itself.
 * Schema-2 payloads carry validity/armed flags, PB5/lux LE16, PWM/relay,
 * shared event time, warmup/readiness and motion sequence/acknowledgment.
 */
#include "demo_startup.h"
static uint8_t demo_active = 1, demo_flags, demo_relay;
#ifdef DEMO_FIXED_DIMMER
#if !defined(DT57_PWM) || DEMO_FIXED_DIMMER < 0 || DEMO_FIXED_DIMMER > 100
#error "Fixed dimmer diagnostic requires DT5-7 and a duty from 0 to 100"
#endif
static uint8_t demo_pwm = DEMO_FIXED_DIMMER;
#else
static uint8_t demo_pwm = 10;
#endif
#ifdef DT57_PWM
static uint8_t demo_target = 10;
static uint8_t demo_motion_seen, demo_have_motion;
#endif
static uint8_t demo_sequence, demo_incoming[16], demo_incoming_ready, demo_incoming_sequence;
static uint8_t demo_peer_mask;
#ifndef SENSOR_PROFILE
static uint8_t demo_motion_sequence,demo_motion_reply;
#endif
static uint32_t demo_boot_tick;
static uint8_t demo_warm_done;
static uint8_t demo_event_id;
static uint16_t demo_event_elapsed;
static uint32_t demo_event_anchor;
static uint16_t demo_lux, demo_pir;
static uint32_t demo_tx_packets, demo_rx_packets, demo_acks, demo_oled_frames;
static uint32_t demo_publish_tick, demo_received_tick, demo_render_tick;
static uint32_t demo_ramp_tick, demo_tx_tick, demo_rx_tick;
static uint8_t demo_render_dirty;
static unsigned demo_warmup_ms(uint32_t now) {
    if(demo_warm_done)return 0;
    unsigned age=now-demo_boot_tick;
    if(age>=DEMO_WARMUP_TICKS){demo_warm_done=1;return 0;}
    return (DEMO_WARMUP_TICKS-age+19u)/20u;
}
static uint8_t demo_ready_mask(uint32_t now) {
    unsigned age=demo_rx_packets ? now-demo_received_tick : DEMO_PEER_TIMEOUT;
    return demo_ready_bits(HEADER_RING_NODE_ID,age,demo_peer_mask,!demo_warmup_ms(now));
}
static unsigned demo_timer_ms(uint32_t now) {
    if(!demo_event_id) return 0;
    unsigned elapsed=demo_event_elapsed;
    if(demo_relay) elapsed+=(now-demo_event_anchor)/20u;
    return elapsed>10000u ? 10000u : elapsed;
}
#if HEADER_RING_NODE_ID != 2
static int demo_apply_timer(const uint8_t *p, uint32_t now) {
    int changed=p[8]!=demo_event_id;
    unsigned old=demo_timer_ms(now);
    unsigned elapsed=p[9]|((unsigned)p[10]<<8);
    if(p[7] && p[8]) elapsed+=260u; /* 26 bytes x 10 serial bits at 1000 baud */
    if(elapsed>10000u) elapsed=10000u;
    if(!changed && p[7] && elapsed<old) elapsed=old; /* stopwatch never runs backward */
    demo_event_id=p[8];demo_event_elapsed=(uint16_t)elapsed;demo_event_anchor=now;
    demo_relay=p[7];
    return changed;
}
#endif
#ifdef SENSOR_PROFILE
static uint8_t demo_ambient_ready, demo_ambient_powered;
static DemoMotion demo_motion;
#ifdef DEMO_PB5_DISPLAY
static uint8_t demo_pb5_seen, demo_pb5_level=3;
static uint32_t demo_pb5_since, demo_pb5_last_high_ms;
static void demo_pb5_track(int value,uint32_t now) {
    unsigned level=value<0?3u:(value>1800?1u:(value<100?0u:2u));
    if(!demo_pb5_seen || level!=demo_pb5_level) {
        if(demo_pb5_seen && demo_pb5_level==1u)demo_pb5_last_high_ms=(now-demo_pb5_since)/20u;
        demo_pb5_seen=1;demo_pb5_level=(uint8_t)level;demo_pb5_since=now;
    }
}
#endif
static uint32_t demo_pir_sample_tick, demo_lux_sample_tick, demo_ambient_retry, demo_ambient_power_tick;
static uint32_t demo_pir_samples, demo_lux_samples;
static int demo_sensor_io(uint8_t reg, uint8_t *bytes, unsigned n, int read) {
    uint8_t saved = oled_bus; oled_bus = 0;
    int ok = i2c_start() && i2c_byte(0x72) && i2c_byte(reg);
    if (read) {
        ok = ok && i2c_start() && i2c_byte(0x73);
        for (unsigned i = 0; ok && i < n; ++i) ok = i2c_read_byte(bytes+i, i+1 == n);
    } else {
        for (unsigned i = 0; ok && i < n; ++i) ok = i2c_byte(bytes[i]);
    }
    i2c_stop(); oled_bus = saved;
    return ok;
}
static int demo_sensor_write(uint8_t reg, uint8_t value) {
    return demo_sensor_io(reg, &value, 1, 0);
}
static void demo_sensor_init(void) {
    uint8_t id;
    demo_ambient_ready = demo_ambient_powered = 0;
    if (!demo_sensor_io(0x92, &id, 1, 1) || id != 0x34) return;
    if (!demo_sensor_write(0x80, 1)) return;
    demo_ambient_powered=1;demo_ambient_power_tick=ticks;
}
/* PIR owns only motion state and its validity/armed bits. No lux I/O here. */
static void demo_pir_sample(void) {
    ++demo_pir_samples;
    int pir = analog_read(5);
#ifdef DEMO_PB5_DISPLAY
    demo_pb5_track(pir,ticks);
#endif
    if (pir >= 0) {
        demo_pir = (uint16_t)pir; demo_flags |= 2u;
    } else demo_flags &= (uint8_t)~2u;
    demo_motion_sample(&demo_motion,pir,ticks,!demo_warmup_ms(ticks),demo_relay&&demo_timer_ms(ticks)<10000u);
    demo_flags=(uint8_t)((demo_flags&~8u)|(demo_motion.armed?8u:0u));
}
/* Ambient retries, saturation and brightness changes own only lux state. */
static void demo_lux_sample(void) {
    ++demo_lux_samples;
    if (!demo_ambient_ready && !demo_ambient_powered && ticks-demo_ambient_retry > 100000u) {
        demo_ambient_retry = ticks; demo_sensor_init();
    }
    if(demo_ambient_powered && ticks-demo_ambient_power_tick>=60u) {
        demo_ambient_powered=0;
        demo_ambient_ready=demo_sensor_write(0x81,237) && demo_sensor_write(0x8f,0) &&
                           demo_sensor_write(0x8d,0) && demo_sensor_write(0x80,3);
    }
    uint8_t status, data[4];
    if (!demo_ambient_ready || !demo_sensor_io(0x93, &status, 1, 1) || !(status & 1u) ||
        !demo_sensor_io(0xb4, data, 4, 1)) { demo_flags &= (uint8_t)~1u; return; }
    unsigned c0 = data[0] | ((unsigned)data[1] << 8), c1 = data[2] | ((unsigned)data[3] << 8);
    if (c0 >= 19456 || c1 >= 19456) { demo_flags &= (uint8_t)~1u; return; }
    int lux1 = (100*(int)c0 - 187*(int)c1)*60/5187;
    int lux2 = (63*(int)c0 - 100*(int)c1)*60/5187;
    int lux = lux1 > lux2 ? lux1 : lux2;
    if (lux < 0) lux = 0;
    if (lux > 65535) lux = 65535;
    demo_lux = (uint16_t)lux; demo_flags |= 1u;
}
#endif
#ifdef DT57_PWM
/* Motion owns the relay deadline and event sequence. Lux never enters here. */
static void demo_accept_motion(const uint8_t *p,uint32_t now) {
    demo_motion_sequence=p[14];demo_motion_reply=0;
    if(p[12] && !demo_relay)demo_have_motion=0;
    if((p[1]&10u)==10u && demo_ready_mask(now)==7 && (p[15]&1u)) {
        if(!demo_have_motion || demo_motion_seen!=demo_motion_sequence) {
            demo_have_motion=1;demo_motion_seen=demo_motion_sequence;
            if(!demo_relay) {
                if(!++demo_event_id)++demo_event_id;
                demo_event_elapsed=0;demo_event_anchor=now;
                LINK_IRQ_OFF();demo_relay_remaining=DEMO_ON_TICKS;demo_relay=1;relay_set(1);LINK_IRQ_ON();
            }
        }
        demo_motion_reply=2;
    }
}
/* Brightness owns only the PWM target, regardless of motion validity/state. */
static void demo_accept_lux(const uint8_t *p) {
    if(p[1]&1u) {
        unsigned lux=p[4]|((unsigned)p[5]<<8);
        unsigned divisor=400u+lux;
        demo_target=(uint8_t)((40000u+divisor/2u)/divisor);
    }
}
#endif
static int demo_emit(uint8_t kind, unsigned destination, uint8_t sequence) {
    uint8_t p[16];
    p[0] = kind; p[1] = demo_flags;
    p[2] = (uint8_t)demo_pir; p[3] = (uint8_t)(demo_pir >> 8);
    p[4] = (uint8_t)demo_lux; p[5] = (uint8_t)(demo_lux >> 8);
    p[6] = demo_pwm; p[7] = demo_relay;
    unsigned elapsed=demo_timer_ms(ticks);
    p[8]=demo_event_id;p[9]=(uint8_t)elapsed;p[10]=(uint8_t)(elapsed>>8);p[11]=2;
    p[12]=(uint8_t)((demo_warmup_ms(ticks)+999u)/1000u);p[13]=demo_ready_mask(ticks);
#ifdef SENSOR_PROFILE
    p[14]=demo_motion.sequence;p[15]=demo_motion.pending?1u:0u;
#else
    p[14]=demo_motion_sequence;p[15]=demo_motion_reply;
#endif
    if (!ring_send(destination, sequence, 4, p, sizeof(p))) return 0;
    ++demo_tx_packets; demo_tx_tick = ticks; return 1;
}
static void demo_packet(const uint8_t *frame, unsigned size) {
    if (!demo_active || size != 26 || frame[7] != 16 || frame[19]!=2) return;
    unsigned expected_source = HEADER_RING_NODE_ID == 1 ? 3 : HEADER_RING_NODE_ID-1;
    unsigned expected_kind = HEADER_RING_NODE_ID == 1 ? 0x12 : (HEADER_RING_NODE_ID == 2 ? 0x10 : 0x11);
    if (frame[3] != expected_source || frame[8] != expected_kind || frame[14] > 100 || frame[15] > 1) return;
    if((frame[17]|((unsigned)frame[18]<<8))>10000u) return;
    if(frame[20]>30 || frame[21]>7 || frame[23]>3)return;
    for (unsigned i = 0; i < 16; ++i) demo_incoming[i] = frame[8+i];
    demo_incoming_sequence = frame[6]; demo_incoming_ready = 1;
    ++demo_rx_packets; demo_rx_tick = demo_received_tick = ticks;
}

/* Code-native 128x64 OLED dashboard, redrawn in RAM then flushed one page
 * per main-loop pass so reception/forwarding stays responsive. */
static void demo_pixel(unsigned x, unsigned y, int on) {
    if (x >= 128 || y >= 64) return;
    uint8_t *b = framebuffer + (y/8)*128+x;
    if (on) *b |= (uint8_t)(1u << (y & 7)); else *b &= (uint8_t)~(1u << (y & 7));
}
#ifndef DEMO_PB5_DISPLAY
static void demo_line(unsigned x1, unsigned y1, unsigned x2, unsigned y2) {
    if (y1 == y2) { for (unsigned x=x1; x<=x2; ++x) demo_pixel(x,y1,1); }
    else { for (unsigned y=y1; y<=y2; ++y) demo_pixel(x1,y,1); }
}
#endif
static void demo_text(unsigned x, unsigned y, const char *s, int inverse) {
    for (; *s && x+5 < 128; ++s, x+=6) {
        unsigned c=(unsigned char)*s; if (c<32 || c>126) c='?';
        for (unsigned col=0; col<5; ++col)
            for (unsigned bit=0; bit<7; ++bit)
                demo_pixel(x+col,y+bit,!!(font[c-32][col] & (1u<<bit)) ^ inverse);
    }
}
static void demo_big(unsigned x,unsigned y,const char *s,unsigned scale) {
    for(;*s && x+5*scale<=128;++s,x+=6*scale) {
        unsigned c=(unsigned char)*s;if(c<32||c>126)c='?';
        for(unsigned col=0;col<5;++col)for(unsigned bit=0;bit<7;++bit)
            if(font[c-32][col]&(1u<<bit))
                for(unsigned dx=0;dx<scale;++dx)for(unsigned dy=0;dy<scale;++dy)
                    demo_pixel(x+col*scale+dx,y+bit*scale+dy,1);
    }
}
static char *demo_decimal(char *p, unsigned n) {
    char reverse[11]; unsigned count=0;
    do { reverse[count++]=(char)('0'+n%10); n/=10; } while(n);
    while(count) *p++=reverse[--count];
    *p=0; return p;
}
static char *demo_copy(char *p, const char *s) { while(*s) *p++=*s++; *p=0; return p; }
#ifdef DEMO_PB5_DISPLAY
static char *demo_duration(char *p,unsigned ms) {
    p=demo_decimal(p,ms/1000u);*p++='.';*p++=(char)('0'+ms/100u%10u);*p++='s';*p=0;return p;
}
#endif
static void demo_draw(void) {
    uint8_t previous[1024];
    for(unsigned i=0;i<1024;++i)previous[i]=framebuffer[i];
    for (unsigned i=0; i<1024; ++i) framebuffer[i]=0;
    for (unsigned x=0; x<128; ++x) framebuffer[x]=0xff;
#ifdef DEMO_PB5_DISPLAY
    const char *title="PB5 LIVE / SENSOR";
#else
    const char *title = HEADER_RING_NODE_ID==1 ? "01  SENSOR / ORIGIN" :
                        (HEADER_RING_NODE_ID==2 ? "02  RELAY / ROUTER" : "03  POLICE / ACK");
#endif
    demo_text(2,0,title,1);
    char line[32],*p=line;
    unsigned warm=demo_warmup_ms(ticks);
#ifdef DEMO_PB5_DISPLAY
    if(demo_flags&2u)demo_decimal(line,demo_pir);else demo_copy(line,"----");
    demo_big(2,11,line,4);
    demo_text(101,12,demo_pb5_level==1?"HI":(demo_pb5_level==0?"LO":(demo_pb5_level==2?"MID":"ERR")),0);
    demo_text(101,24,"ADC",0);
    p=demo_copy(line,demo_pb5_level==1?"HIGH ":(demo_pb5_level==0?"LOW ":(demo_pb5_level==2?"MID ":"ERROR ")));
    demo_duration(p,demo_pb5_seen?(ticks-demo_pb5_since)/20u:0u);demo_text(2,41,line,0);
    p=demo_decimal(line,demo_pwm);demo_copy(p,"%");demo_text(101,41,line,0);
    if(warm){p=demo_copy(line,"WARMUP ");demo_duration(p,warm);}
    else if(demo_ready_mask(ticks)!=7)demo_copy(line,"WAITALL / PB5 LIVE");
    else {p=demo_copy(line,"LAST HIGH ");demo_duration(p,demo_pb5_last_high_ms);}
    demo_text(2,49,line,0);
#else
    unsigned elapsed=warm?warm:demo_timer_ms(ticks),seconds=elapsed/1000u;
    if(seconds<10)*p++='0';
    p=demo_decimal(p,seconds);*p++='.';*p++=(char)('0'+(elapsed/100u)%10u);*p=0;
    demo_big(2,11,line,3);
    demo_text(83,12,"SECONDS",0);
    uint8_t ready=demo_ready_mask(ticks);
    demo_text(83,22,warm?"WARMUP":(ready!=7?"WAITALL":(!(demo_flags&8u)?"WAITPIR":
              (demo_relay&&elapsed<10000u?"RUNNING":(demo_event_id?"STOPPED":"ARMED")))),0);
    unsigned progress=elapsed*127u/10000u;
    if(!warm && demo_event_id)demo_line(0,34,progress,34);
    p=demo_decimal(line,demo_pwm);demo_copy(p,"%");demo_big(2,38,line,2);
#ifdef DEMO_FIXED_DIMMER
    demo_text(53,43,"FIX",0);
#else
    demo_text(53,43,"DIM",0);
#endif
    if(ready!=7) {
        char peers[7];peers[0]='S';peers[2]='R';peers[4]='L';peers[6]=0;
        peers[1]=(ready&1u)?'+':'-';peers[3]=(ready&2u)?'+':'-';peers[5]=(ready&4u)?'+':'-';
        demo_text(83,37,peers,0);
    } else demo_text(83,37,demo_relay?"208 ON":"208 OFF",0);
#if HEADER_RING_NODE_ID == 3
    p=demo_copy(line,"W");p=demo_decimal(p,demo_white_warm);p=demo_copy(p," C");demo_decimal(p,demo_white_cool);
#elif HEADER_RING_NODE_ID == 1
    p=demo_copy(line,"PIR");demo_decimal(p,demo_pir);
#else
    p=demo_copy(line,"LX ");demo_decimal(p,demo_lux);
#endif
    demo_text(83,46,line,0);
#endif
    p=demo_copy(line,"S>R>L>S T");p=demo_decimal(p,demo_tx_packets%1000u);p=demo_copy(p," R");demo_decimal(p,demo_rx_packets%1000u);
    demo_text(0,56,line,0);
    unsigned age=(ticks-demo_tx_tick)/20u;
    if(demo_tx_packets && age<500u)for(unsigned dx=0;dx<5;++dx)demo_pixel(age*123u/500u+dx,63,1);
    demo_render_dirty=0;
    for(unsigned page=0;page<8;++page)for(unsigned x=0;x<128;++x)
        if(previous[page*128+x]!=framebuffer[page*128+x]){demo_render_dirty|=(uint8_t)(1u<<page);break;}
    ++demo_oled_frames;
}
static void demo_init(void) {
    dashboard=0; ring_start();
    demo_boot_tick=demo_publish_tick=demo_received_tick=demo_ramp_tick=ticks;
    demo_warm_done=0;
#ifdef DEMO_FIXED_DIMMER
    lamp_set(demo_pwm);
#endif
#if HEADER_RING_NODE_ID == 3
    demo_police_stop();
#endif
#ifdef SENSOR_PROFILE
    demo_pir_sample_tick=demo_lux_sample_tick=demo_ambient_retry=ticks;
    demo_sensor_init();
#endif
}
static void demo_service(void) {
    if(!demo_active) return;
    uint32_t now=ticks;
#ifdef DT57_PWM
    if(demo_relay && !demo_relay_remaining) {
        demo_relay=0;demo_event_elapsed=10000;demo_event_anchor=now;
        (void)demo_emit(0x11,3,demo_sequence);
    }
#endif
#ifdef SENSOR_PROFILE
    if(now-demo_pir_sample_tick>=1000u) {demo_pir_sample_tick=now;demo_pir_sample();}
    if(now-demo_lux_sample_tick>=4000u) {demo_lux_sample_tick=now;demo_lux_sample();}
    if(now-demo_publish_tick>=20000u) {
        if(demo_emit(0x10,2,++demo_sequence)) demo_publish_tick=now;
    }
#endif
    if(demo_incoming_ready) {
        demo_incoming_ready=0;
        demo_peer_mask=demo_incoming[13];
#if HEADER_RING_NODE_ID != 1
        demo_flags=demo_incoming[1];demo_pir=demo_incoming[2]|((uint16_t)demo_incoming[3]<<8);
        demo_lux=demo_incoming[4]|((uint16_t)demo_incoming[5]<<8);
        demo_sequence=demo_incoming_sequence;
#endif
#ifdef DT57_PWM
        demo_accept_motion(demo_incoming,now);
        demo_accept_lux(demo_incoming);
        (void)demo_emit(0x11,3,demo_sequence);
#elif HEADER_RING_NODE_ID == 3
        demo_motion_sequence=demo_incoming[14];demo_motion_reply=demo_incoming[15]&2u;
        demo_pwm=demo_incoming[6];
        demo_white_target=demo_pwm;
        int new_event=demo_apply_timer(demo_incoming,now);
        unsigned remaining=(10000u-demo_timer_ms(now))*20u;
        if(new_event && demo_event_id && demo_relay && remaining) {
            demo_police_start();demo_strobe_remaining=remaining;
        } else if(!demo_relay || !remaining)demo_police_stop();
        else if(demo_strobe_remaining>remaining)demo_strobe_remaining=remaining;
        (void)demo_emit(0x12,1,demo_sequence);
#else
        demo_pwm=demo_incoming[6];(void)demo_apply_timer(demo_incoming,now);++demo_acks;
        demo_motion_ack(&demo_motion,demo_incoming[14],demo_incoming[15]&2u);
#endif
    }
#ifdef DT57_PWM
    relay_set(demo_relay);
    if(now-demo_ramp_tick>=1600u) {
        demo_ramp_tick=now;
#ifdef DEMO_FIXED_DIMMER
        /* Keep sampling/calculating lux targets, but remove light-output
         * changes from the PB5 cover/uncover experiment. */
        demo_pwm=DEMO_FIXED_DIMMER;
#else
        if(demo_pwm<demo_target) ++demo_pwm;else if(demo_pwm>demo_target)--demo_pwm;
#endif
        lamp_set(demo_pwm);
    }
#endif
    if(oled_on && demo_render_dirty && rx_head==rx_tail) {
        for(unsigned page=0;page<8;++page)if(demo_render_dirty&(1u<<page)) {
            (void)flush_page(page);demo_render_dirty&=(uint8_t)~(1u<<page);break;
        }
    }
    if(oled_on && !demo_render_dirty && now-demo_render_tick>=2000u) {
        demo_render_tick=now;demo_draw();
    }
}
