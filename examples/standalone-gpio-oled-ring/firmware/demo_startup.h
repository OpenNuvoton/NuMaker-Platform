/* Startup and motion capture independent of USB, OLED work and packet rate. */
#define DEMO_WARMUP_TICKS 300000u
#define DEMO_PEER_TIMEOUT 60000u
typedef struct {
    uint8_t have_sample,candidate,high,armed,sequence,pending;
    uint32_t candidate_since;
} DemoMotion;
static inline uint8_t demo_ready_bits(unsigned node,uint32_t age,uint8_t peer,int warm) {
    unsigned own=1u<<(node-1u);
    return (uint8_t)((age<DEMO_PEER_TIMEOUT ? peer&~own : 0u)|(warm?own:0u));
}
static inline void demo_motion_sample(DemoMotion *m,int value,uint32_t now,int warm,int busy) {
    if(value<0)return;
    unsigned candidate=value>1800 ? 1u : (value<100 ? 0u : m->candidate);
    if(!m->have_sample || candidate!=m->candidate) {
        m->have_sample=1;m->candidate=(uint8_t)candidate;m->candidate_since=now;
    }
    unsigned stable=now-m->candidate_since;
    if(!candidate && stable>=4000u) {
        m->high=0;
        if(warm)m->armed=1;
    } else if(candidate && stable>=2000u && !m->high) {
        m->high=1;
        if(warm && m->armed && !busy && !m->pending) {
            if(!++m->sequence)++m->sequence;
            m->pending=1;
        }
    }
}
static inline void demo_motion_ack(DemoMotion *m,unsigned sequence,unsigned accepted) {
    if(accepted && sequence==m->sequence)m->pending=0;
}
