#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../firmware/demo_startup.h"
int main(void) {
    assert(DEMO_WARMUP_TICKS==15u*20000u);
    DemoMotion m={0};
    demo_motion_sample(&m,2000,0,0,0);
    demo_motion_sample(&m,2000,20000,0,0);
    assert(!m.armed&&!m.pending); /* cold-start HIGH is not a motion event */
    demo_motion_sample(&m,2000,600000,1,0);
    assert(!m.armed&&!m.pending);
    demo_motion_sample(&m,1,601000,1,0);
    demo_motion_sample(&m,1,604999,1,0);assert(!m.armed);
    demo_motion_sample(&m,1,605000,1,0);assert(m.armed&&!m.pending);
    demo_motion_sample(&m,2000,606000,1,0);
    demo_motion_sample(&m,1,607000,1,0);assert(!m.pending); /* reject short spike */
    demo_motion_sample(&m,1,611000,1,0);
    demo_motion_sample(&m,2000,612000,1,0);
    demo_motion_sample(&m,2000,614000,1,0);assert(m.pending&&m.sequence==1);
    demo_motion_sample(&m,1,615000,1,0);demo_motion_sample(&m,1,619000,1,0);
    demo_motion_sample(&m,2000,620000,1,0);demo_motion_sample(&m,2000,622000,1,0);
    assert(m.pending&&m.sequence==1); /* retain the first pending event */
    demo_motion_ack(&m,1,0);assert(m.pending);
    demo_motion_ack(&m,2,1);assert(m.pending);
    demo_motion_ack(&m,1,1);assert(!m.pending);
    demo_motion_sample(&m,1,623000,1,1);demo_motion_sample(&m,1,627000,1,1);
    demo_motion_sample(&m,2000,628000,1,1);demo_motion_sample(&m,2000,630000,1,1);
    assert(!m.pending&&m.sequence==1); /* an active burst is never extended */
    assert(demo_ready_bits(1,0,7,0)==6);
    assert(demo_ready_bits(2,0,7,0)==5);
    assert(demo_ready_bits(3,0,7,0)==3); /* any reboot removes that board's ready bit */
    assert(demo_ready_bits(1,60000,7,1)==1);
    assert(demo_ready_bits(2,60000,7,1)==2);
    assert(demo_ready_bits(3,60000,7,1)==4); /* stale peer readiness cannot latch */
    uint8_t a=0,b=0,c=0;
    for(unsigned i=0;i<4;++i){a=demo_ready_bits(1,0,c,1);b=demo_ready_bits(2,0,a,1);c=demo_ready_bits(3,0,b,1);}
    assert(a==7&&b==7&&c==7);
    c=demo_ready_bits(3,0,b,0);a=demo_ready_bits(1,0,c,1);b=demo_ready_bits(2,0,a,1);
    assert(a==3&&b==3&&c==3);
    puts("PASS: 15-second startup gate; stable-low arming; glitch rejection; pending-event ACK; reboot/readiness loss");
}
