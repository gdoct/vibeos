#define _GNU_SOURCE 1
// libFuzzer harness for VibeOS TCP receive path.
// Includes net.c directly to reach its static functions and globals.
#include <stdint.h>
#include <stddef.h>
#include <string.h>

uint64_t g_now_ticks = 1;                 // backing store for timer_ticks()

// Pull the kernel TCP/IP stack in as source so we can call statics.
#include "net.c"

extern "C" int virtio_net_init(void){ return 0; }

// ---- fixed test topology ----
#define PEER_IP   IPADDR(10,0,2,50)
#define PEER_ISS  0x20000000u
static const uint8_t PEER_MAC[6] = {0x52,0x54,0,1,2,3};

static int tx_sink(const void *frame, uint32_t len){ (void)frame; (void)len; return 0; }
static net_device_t g_fuzz_dev = { {0x52,0x54,0,0,0,1}, tx_sink, "fuzz0" };

// Reset all stack state to a clean, deterministic baseline each run.
static tcp_pcb_t *seed_state(void) {
    g_dev = &g_fuzz_dev; g_up = 1; g_net_worker = nullptr;
    g_now_ticks = 1; g_ephemeral = 49152;
    g_rxq_head = g_rxq_tail = 0; g_loq_head = g_loq_tail = 0;
    g_tcp_work = g_tcp_timer_pending = 0;
    kmemset(g_tcp, 0, sizeof g_tcp);
    kmemset(g_arp, 0, sizeof g_arp);
    arp_cache_put(PEER_IP, PEER_MAC);
    arp_cache_put(GATEWAY_IP, PEER_MAC);

    // Slot 0: an ESTABLISHED connection with 1000 bytes of data in flight,
    // so the ack/RTT/cwnd/fast-rexmit/snd_copyout paths are all reachable.
    tcp_pcb_t *a = &g_tcp[0];
    a->used = 1; a->state = T_ESTABLISHED;
    a->local_ip = LOCAL_IP; a->local_port = 4000;
    a->remote_ip = PEER_IP; a->remote_port = 5000;
    a->iss = 0x10000000u;
    a->snd_buf_seq = a->iss + 1;
    for (int i = 0; i < 1000; i++) a->sndbuf[i] = (uint8_t)i;
    a->snd_buf_off = 0; a->snd_buf_len = 1000;
    a->snd_una = a->iss + 1;
    a->snd_nxt = a->snd_buf_seq + 1000;        // all queued data is in flight
    a->snd_mss = TCP_MSS; a->cwnd = 4 * TCP_MSS; a->ssthresh = 0xFFFF;
    a->rto = TCP_RTO_INIT; a->snd_wnd = 65535;
    a->snd_wl1 = PEER_ISS; a->snd_wl2 = a->snd_una;
    a->rcv_nxt = PEER_ISS + 1;

    // Slot 1: a listener, so SYN/handshake/accept-queue code is reachable.
    tcp_listen(80);
    return a;
}

namespace {
struct Cur {
    const uint8_t *p; size_t n, i;
    Cur(const uint8_t *d, size_t l): p(d), n(l), i(0) {}
    size_t rem() const { return i < n ? n - i : 0; }
    uint8_t u8(){ return i < n ? p[i++] : 0; }
    uint16_t u16(){ uint16_t v = u8(); return (uint16_t)((v << 8) | u8()); }
    uint32_t u32(){ uint32_t v = 0; for (int k=0;k<4;k++) v=(v<<8)|u8(); return v; }
};
}

// Find a live pcb matching a 4-tuple (for state-relative seq/ack crafting).
static tcp_pcb_t *find_pcb(uint32_t rip, uint16_t rport, uint16_t lport){
    for (int i=0;i<TCP_PCB_N;i++){
        tcp_pcb_t *c=&g_tcp[i];
        if (c->used && c->state!=T_LISTEN &&
            c->remote_ip==rip && c->remote_port==rport && c->local_port==lport)
            return c;
    }
    return nullptr;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    seed_state();
    Cur c(data, size);

    for (int step = 0; step < 48 && c.rem() >= 2; step++) {
        // choose the 4-tuple this segment targets
        uint32_t src; uint16_t sport, dport;
        switch (c.u8() & 3) {
            case 0: src=PEER_IP; sport=5000; dport=4000; break;          // established conn
            case 1: src=PEER_IP; sport=(uint16_t)(6000+(c.u8()&7)); dport=80; break; // listener / its children
            case 2: src=PEER_IP; sport=c.u16(); dport=c.u16(); break;    // partial control
            default: src=(IPADDR(10,0,2,0))|(c.u8()); sport=c.u16(); dport=c.u16(); break;
        }

        tcp_pcb_t *m = find_pcb(src, sport, dport);

        // seq, relative to the connection's expected rcv_nxt to reach the
        // in-order / out-of-order / overlap branches, or absolute.
        uint32_t seq;
        switch (c.u8() % 4) {
            case 0: seq = m ? m->rcv_nxt + (int8_t)c.u8() : c.u32(); break;
            case 1: seq = m ? m->rcv_nxt + (int16_t)c.u16() : c.u32(); break;
            case 2: seq = m ? m->rcv_nxt : c.u32(); break;
            default: seq = c.u32(); break;
        }
        // ack, relative to snd_una/snd_nxt or absolute.
        uint32_t ack;
        switch (c.u8() % 4) {
            case 0: ack = m ? m->snd_una + (int8_t)c.u8() : c.u32(); break;
            case 1: ack = m ? m->snd_nxt + (int8_t)c.u8() : c.u32(); break;
            case 2: ack = m ? m->snd_una : c.u32(); break;
            default: ack = c.u32(); break;
        }

        uint8_t  flags = c.u8();
        uint16_t win   = c.u16();

        // header length: usually valid (20..40), sometimes raw-garbage nibble
        uint8_t  optwords = c.u8() & 7;          // 0..7 option words
        uint32_t doff = sizeof(tcp_hdr_t) + optwords*4;
        uint8_t  raw_doff_nibble = 0; int use_raw = (c.u8() & 7) == 0;

        uint32_t paylen = c.rem();
        if (paylen > 1400) paylen = 1400;
        // cap payload by an occasional small bound to vary segment sizes
        if (c.u8() & 1) { uint32_t cap = c.u8()*4u; if (paylen > cap) paylen = cap; }

        static uint8_t seg[2048];
        if (doff + paylen > sizeof seg) paylen = (uint32_t)sizeof seg - doff;
        tcp_hdr_t *th = (tcp_hdr_t *)seg;
        th->src_port = htons_(sport);
        th->dst_port = htons_(dport);
        th->seq = htonl_(seq);
        th->ack = htonl_(ack);
        if (use_raw) { raw_doff_nibble = c.u8() & 0xF; th->data_off = (uint8_t)(raw_doff_nibble<<4); }
        else         th->data_off = (uint8_t)((doff/4) << 4);
        th->flags = flags;
        th->window = htons_(win);
        th->csum = 0; th->urg = 0;
        // option bytes (may encode MSS option kind=2 if the fuzzer hits it)
        for (uint32_t k = sizeof(tcp_hdr_t); k < doff && k < sizeof seg; k++) seg[k] = c.u8();
        for (uint32_t k = 0; k < paylen; k++) seg[doff + k] = c.u8();

        uint32_t seglen = doff + paylen;
        if (seglen > sizeof seg) seglen = sizeof seg;
        tcp_input(src, seg, seglen);

        // occasionally advance time and run the slow/fast timer
        if ((step & 3) == 3) { g_now_ticks += (c.u8() | 1); tcp_timer(); }
        tcp_pump();
    }
    return 0;
}

#ifdef STANDALONE
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
static void on_alarm(int){
    void *bt[32]; int n = backtrace(bt, 32);
    fprintf(stderr, "\n==== SIGALRM: still running, backtrace (%d frames) ====\n", n);
    backtrace_symbols_fd(bt, n, 2);
    _exit(42);
}
int main(int argc, char **argv){
    signal(SIGALRM, on_alarm);
    FILE *f = fopen(argv[1], "rb");
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    uint8_t *buf=(uint8_t*)malloc(sz); fread(buf,1,sz,f); fclose(f);
    alarm(3);
    LLVMFuzzerTestOneInput(buf, sz);
    fprintf(stderr, "returned normally (no hang)\n");
    return 0;
}
#endif
