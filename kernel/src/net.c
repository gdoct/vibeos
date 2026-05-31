#include "kernel.h"
#include "task.h"
#include "timer.h"
#include "net.h"
#include "csprng.h"

/*
 * Compact IPv4 stack (ROADMAP §5): Ethernet demux, ARP, IPv4, ICMP — enough to
 * answer and originate pings. UDP/TCP/sockets layer on top in later rungs.
 *
 * Received frames are queued by net_rx() (IRQ context) and processed by a kernel
 * worker (net_input) in task context, so the stack may block (e.g. waiting for
 * an ARP reply) and transmit freely. Addressing is the static SLIRP layout.
 */

#define LOCAL_IP    IPADDR(10,0,2,15)
#define GATEWAY_IP  IPADDR(10,0,2,2)
#define NETMASK     IPADDR(255,255,255,0)

#define IPPROTO_ICMP 1
#define IPPROTO_UDP  17
#define IPPROTO_TCP  6

static net_device_t *g_dev = nullptr;
static int           g_up  = 0;

/* ---- packed wire structures (network byte order on the wire) ---- */

typedef struct { uint8_t  dst[6], src[6]; uint16_t type; } __attribute__((packed)) eth_hdr_t;

typedef struct {
    uint16_t htype, ptype;
    uint8_t  hlen, plen;
    uint16_t op;
    uint8_t  sha[6]; uint8_t spa[4];
    uint8_t  tha[6]; uint8_t tpa[4];
} __attribute__((packed)) arp_pkt_t;

typedef struct {
    uint8_t  ver_ihl, tos;
    uint16_t total_len, id, frag;
    uint8_t  ttl, proto;
    uint16_t csum;
    uint32_t src, dst;
} __attribute__((packed)) ip_hdr_t;

typedef struct {
    uint8_t  type, code;
    uint16_t csum, id, seq;
} __attribute__((packed)) icmp_hdr_t;

typedef struct {
    uint16_t src_port, dst_port, len, csum;
} __attribute__((packed)) udp_hdr_t;

typedef struct {
    uint16_t src_port, dst_port;
    uint32_t seq, ack;
    uint8_t  data_off;       /* high nibble: header words */
    uint8_t  flags;          /* FIN/SYN/RST/PSH/ACK */
    uint16_t window, csum, urg;
} __attribute__((packed)) tcp_hdr_t;

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

static void tcp_input(uint32_t src, const uint8_t *p, uint32_t len);

#define ARP_REQUEST 1
#define ARP_REPLY   2
#define ICMP_ECHO_REQUEST 8
#define ICMP_ECHO_REPLY   0

/* ---- rx queue (IRQ -> worker) ---- */

#define RXQ_N 64
typedef struct { uint8_t data[ETH_FRAME_MAX]; uint16_t len; } rxslot_t;
static rxslot_t     g_rxq[RXQ_N];
static uint32_t     g_rxq_head, g_rxq_tail;     /* head==tail: empty */
static wait_queue_t g_rxq_wq;

/* Loopback frames generated *by the worker* (e.g. a TCP ACK while tcp_input
   holds sched_lock) are deferred to this worker-private queue and drained after
   the current frame, so the stack never re-enters the sched_lock-protected rx
   path while already holding it. Only the worker touches it -> no lock. */
#define LOQ_N 32
static rxslot_t  g_loq[LOQ_N];
static uint32_t  g_loq_head, g_loq_tail;
static task_t   *g_net_worker = nullptr;

static void loq_enqueue(const uint8_t *frame, uint32_t len) {
    uint32_t next = (g_loq_head + 1) % LOQ_N;
    if (next == g_loq_tail) return;             /* drop on overflow */
    if (len > ETH_FRAME_MAX) len = ETH_FRAME_MAX;
    kmemcpy(g_loq[g_loq_head].data, frame, len);
    g_loq[g_loq_head].len = (uint16_t)len;
    g_loq_head = next;
}

void net_rx(const uint8_t *frame, uint32_t len) {
    if (len > ETH_FRAME_MAX) len = ETH_FRAME_MAX;
    sched_lock();
    uint32_t next = (g_rxq_head + 1) % RXQ_N;
    if (next != g_rxq_tail) {                    /* drop on overflow */
        kmemcpy(g_rxq[g_rxq_head].data, frame, len);
        g_rxq[g_rxq_head].len = (uint16_t)len;
        g_rxq_head = next;
        wait_queue_wake_all_locked(&g_rxq_wq);
    }
    sched_unlock();
}

/* ---- ARP cache ---- */

typedef struct { uint32_t ip; uint8_t mac[6]; int valid; } arp_entry_t;
#define ARP_N 16
static arp_entry_t  g_arp[ARP_N];
static wait_queue_t g_arp_wq;                    /* sleepers waiting for a reply */

static void arp_cache_put(uint32_t ip, const uint8_t *mac) {
    int slot = -1;
    for (int i = 0; i < ARP_N; i++) {
        if (g_arp[i].valid && g_arp[i].ip == ip) { slot = i; break; }
        if (slot < 0 && !g_arp[i].valid) slot = i;
    }
    if (slot < 0) slot = 0;
    g_arp[slot].ip = ip;
    kmemcpy(g_arp[slot].mac, mac, 6);
    g_arp[slot].valid = 1;
}
static int arp_cache_get(uint32_t ip, uint8_t *mac) {
    for (int i = 0; i < ARP_N; i++)
        if (g_arp[i].valid && g_arp[i].ip == ip) { kmemcpy(mac, g_arp[i].mac, 6); return 1; }
    return 0;
}

/* ---- transmit helpers ---- */

static const uint8_t BCAST[6] = { 0xff,0xff,0xff,0xff,0xff,0xff };

static void eth_send(const uint8_t *dstmac, uint16_t ethertype,
                     const void *payload, uint32_t plen) {
    if (!g_dev) return;
    uint8_t frame[ETH_FRAME_MAX];
    eth_hdr_t *eh = (eth_hdr_t *)frame;
    kmemcpy(eh->dst, dstmac, 6);
    kmemcpy(eh->src, g_dev->mac, 6);
    eh->type = htons_(ethertype);
    uint32_t n = plen;
    if (ETH_HDR_LEN + n > ETH_FRAME_MAX) n = ETH_FRAME_MAX - ETH_HDR_LEN;
    kmemcpy(frame + ETH_HDR_LEN, payload, n);
    uint32_t total = ETH_HDR_LEN + n;
    if (total < 60) { kmemset(frame + total, 0, 60 - total); total = 60; }  /* pad */
    g_dev->tx(frame, total);
}

static void arp_send(uint16_t op, const uint8_t *target_mac, uint32_t target_ip) {
    arp_pkt_t a;
    a.htype = htons_(1); a.ptype = htons_(ETHERTYPE_IP);
    a.hlen = 6; a.plen = 4; a.op = htons_(op);
    kmemcpy(a.sha, g_dev->mac, 6);
    uint32_t sip = htonl_(LOCAL_IP); kmemcpy(a.spa, &sip, 4);
    kmemcpy(a.tha, target_mac, 6);
    uint32_t tip = htonl_(target_ip); kmemcpy(a.tpa, &tip, 4);
    eth_send(op == ARP_REQUEST ? BCAST : target_mac, ETHERTYPE_ARP, &a, sizeof a);
}

/* Resolve `ip` to a MAC, sending ARP requests and waiting (caller must NOT be
   the net worker — the worker processes the reply). Returns 1 on success. */
static int arp_resolve(uint32_t ip, uint8_t *mac) {
    if (arp_cache_get(ip, mac)) return 1;
    for (int tries = 0; tries < 5; tries++) {
        arp_send(ARP_REQUEST, BCAST, ip);
        uint64_t deadline = timer_ticks() + 50;          /* ~0.5s */
        sched_lock();
        while (!arp_cache_get(ip, mac) && timer_ticks() < deadline)
            wait_queue_sleep_locked(&g_arp_wq);
        int ok = arp_cache_get(ip, mac);
        sched_unlock();
        if (ok) return 1;
    }
    return 0;
}

/* ---- checksums ---- */

static uint16_t ip_checksum(const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;
    while (len > 1) { sum += (uint32_t)((p[0] << 8) | p[1]); p += 2; len -= 2; }
    if (len) sum += (uint32_t)(p[0] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* ---- IPv4 / ICMP ---- */

/* Send an IPv4 packet (payload already in network order). For a destination off
   our subnet, route via the gateway. */
/* True for our own address and 127.0.0.0/8 — these are looped back into the
   stack instead of hitting the wire, so in-guest clients/servers can talk over
   the full protocol path with no external network. */
static int is_loopback(uint32_t ip) {
    return ip == LOCAL_IP || (ip >> 24) == 127;
}

static int ip_send(uint32_t dst, uint8_t proto, const void *payload, uint32_t plen) {
    uint8_t pkt[ETH_FRAME_MAX - ETH_HDR_LEN];
    ip_hdr_t *ip = (ip_hdr_t *)pkt;
    uint32_t total = sizeof(ip_hdr_t) + plen;
    if (total > sizeof pkt) return -1;
    ip->ver_ihl = 0x45; ip->tos = 0;
    ip->total_len = htons_((uint16_t)total);
    ip->id = htons_(0); ip->frag = htons_(0x4000);    /* don't fragment */
    ip->ttl = 64; ip->proto = proto; ip->csum = 0;
    ip->src = htonl_(is_loopback(dst) ? dst : LOCAL_IP); ip->dst = htonl_(dst);
    ip->csum = htons_(ip_checksum(ip, sizeof(ip_hdr_t)));
    kmemcpy(pkt + sizeof(ip_hdr_t), payload, plen);

    if (is_loopback(dst)) {                            /* loop back through the stack */
        uint8_t frame[ETH_FRAME_MAX];
        eth_hdr_t *eh = (eth_hdr_t *)frame;
        kmemset(eh->dst, 0, 6); kmemset(eh->src, 0, 6);
        eh->type = htons_(ETHERTYPE_IP);
        kmemcpy(frame + ETH_HDR_LEN, pkt, total);
        if (task_current() == g_net_worker)     /* inside worker: defer, don't relock */
            loq_enqueue(frame, ETH_HDR_LEN + total);
        else
            net_rx(frame, ETH_HDR_LEN + total); /* task context: hand to the worker */
        return 0;
    }

    uint32_t nexthop = ((dst & NETMASK) == (LOCAL_IP & NETMASK)) ? dst : GATEWAY_IP;
    uint8_t mac[6];
    if (!arp_resolve(nexthop, mac)) return -1;
    eth_send(mac, ETHERTYPE_IP, pkt, total);
    return 0;
}

/* Echo-reply state for the originated-ping self-test. */
static volatile int g_ping_got = 0;
static volatile uint16_t g_ping_seq = 0;
static wait_queue_t g_ping_wq;

static void icmp_input(uint32_t src, const uint8_t *p, uint32_t len) {
    if (len < sizeof(icmp_hdr_t)) return;
    const icmp_hdr_t *ih = (const icmp_hdr_t *)p;
    if (ih->type == ICMP_ECHO_REQUEST) {
        /* Build an echo reply: same payload, type 0, fresh checksum. */
        uint8_t buf[ETH_FRAME_MAX];
        if (len > sizeof buf) return;
        kmemcpy(buf, p, len);
        icmp_hdr_t *r = (icmp_hdr_t *)buf;
        r->type = ICMP_ECHO_REPLY; r->code = 0; r->csum = 0;
        r->csum = htons_(ip_checksum(buf, len));
        ip_send(src, IPPROTO_ICMP, buf, len);
    } else if (ih->type == ICMP_ECHO_REPLY) {
        sched_lock();
        g_ping_got = 1; g_ping_seq = ntohs_(ih->seq);
        wait_queue_wake_all_locked(&g_ping_wq);
        sched_unlock();
    }
}

/* ---- UDP ---- */

#define UDP_PCB_N   16
#define UDP_RXN     8
#define UDP_MAXDATA 1472
typedef struct {
    uint32_t src_ip; uint16_t src_port; uint16_t len;
    uint8_t  data[UDP_MAXDATA];
} udp_dgram_t;
typedef struct {
    int          used;
    uint16_t     local_port;
    udp_dgram_t  q[UDP_RXN];
    uint32_t     qhead, qtail;
    wait_queue_t wq;
} udp_pcb_t;
static udp_pcb_t g_udp[UDP_PCB_N];
static uint16_t  g_ephemeral = 49152;

static uint16_t udp_checksum(uint32_t src, uint32_t dst, const uint8_t *udp, uint32_t len) {
    uint32_t sum = 0;
    sum += (src >> 16) & 0xFFFF; sum += src & 0xFFFF;
    sum += (dst >> 16) & 0xFFFF; sum += dst & 0xFFFF;
    sum += IPPROTO_UDP; sum += len;
    for (uint32_t i = 0; i + 1 < len; i += 2) sum += (udp[i] << 8) | udp[i + 1];
    if (len & 1) sum += udp[len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    uint16_t c = (uint16_t)~sum;
    return c ? c : 0xFFFF;       /* 0 means "no checksum"; send all-ones instead */
}

static udp_pcb_t *udp_bind(uint16_t port) {
    if (port != 0) {
        for (int i = 0; i < UDP_PCB_N; i++)
            if (g_udp[i].used && g_udp[i].local_port == port) return nullptr;  /* in use */
    }
    for (int i = 0; i < UDP_PCB_N; i++) {
        if (g_udp[i].used) continue;
        kmemset(&g_udp[i], 0, sizeof g_udp[i]);
        g_udp[i].used = 1;
        g_udp[i].local_port = port ? port : g_ephemeral++;
        return &g_udp[i];
    }
    return nullptr;
}

static void udp_close(udp_pcb_t *p) { if (p) p->used = 0; }

static int udp_sendto(udp_pcb_t *p, uint32_t dip, uint16_t dport,
                      const void *data, uint32_t len) {
    if (len > UDP_MAXDATA) return -1;
    uint8_t seg[sizeof(udp_hdr_t) + UDP_MAXDATA];
    udp_hdr_t *uh = (udp_hdr_t *)seg;
    uint32_t tot = sizeof(udp_hdr_t) + len;
    uh->src_port = htons_(p->local_port);
    uh->dst_port = htons_(dport);
    uh->len = htons_((uint16_t)tot);
    uh->csum = 0;
    kmemcpy(seg + sizeof(udp_hdr_t), data, len);
    uint32_t sip = is_loopback(dip) ? dip : LOCAL_IP;
    uh->csum = htons_(udp_checksum(sip, dip, seg, tot));
    return ip_send(dip, IPPROTO_UDP, seg, tot);
}

/* Blocking receive of one datagram. Returns payload length, fills src ip/port. */
static int udp_recvfrom(udp_pcb_t *p, void *buf, uint32_t maxlen,
                        uint32_t *src_ip, uint16_t *src_port) {
    sched_lock();
    while (p->used && p->qhead == p->qtail)
        wait_queue_sleep_locked(&p->wq);
    if (!p->used) { sched_unlock(); return -1; }
    udp_dgram_t *d = &p->q[p->qtail];
    uint32_t n = d->len; if (n > maxlen) n = maxlen;
    kmemcpy(buf, d->data, n);
    if (src_ip)   *src_ip = d->src_ip;
    if (src_port) *src_port = d->src_port;
    p->qtail = (p->qtail + 1) % UDP_RXN;
    sched_unlock();
    return (int)n;
}

static void udp_input(uint32_t src, const uint8_t *p, uint32_t len) {
    if (len < sizeof(udp_hdr_t)) return;
    const udp_hdr_t *uh = (const udp_hdr_t *)p;
    uint16_t dport = ntohs_(uh->dst_port);
    uint16_t sport = ntohs_(uh->src_port);
    uint32_t dlen = ntohs_(uh->len);
    if (dlen < sizeof(udp_hdr_t) || dlen > len) dlen = len;
    uint32_t plen = dlen - sizeof(udp_hdr_t);

    sched_lock();
    for (int i = 0; i < UDP_PCB_N; i++) {
        udp_pcb_t *pcb = &g_udp[i];
        if (!pcb->used || pcb->local_port != dport) continue;
        uint32_t next = (pcb->qhead + 1) % UDP_RXN;
        if (next != pcb->qtail) {                     /* drop if full */
            udp_dgram_t *d = &pcb->q[pcb->qhead];
            d->src_ip = src; d->src_port = sport;
            d->len = (uint16_t)(plen > UDP_MAXDATA ? UDP_MAXDATA : plen);
            kmemcpy(d->data, p + sizeof(udp_hdr_t), d->len);
            pcb->qhead = next;
            wait_queue_wake_all_locked(&pcb->wq);
        }
        break;
    }
    sched_unlock();
}

static void ip_input(const uint8_t *p, uint32_t len) {
    if (len < sizeof(ip_hdr_t)) return;
    const ip_hdr_t *ip = (const ip_hdr_t *)p;
    if ((ip->ver_ihl >> 4) != 4) return;
    uint32_t ihl = (ip->ver_ihl & 0xF) * 4;
    if (ihl < sizeof(ip_hdr_t) || ihl > len) return;
    uint32_t dst = ntohl_(ip->dst);
    if (dst != LOCAL_IP && dst != 0xFFFFFFFFu && !is_loopback(dst)) return;   /* not for us */
    uint32_t src = ntohl_(ip->src);
    uint16_t total = ntohs_(ip->total_len);
    if (total > len) total = (uint16_t)len;
    const uint8_t *payload = p + ihl;
    uint32_t plen = total - ihl;
    if (ip->proto == IPPROTO_ICMP)     icmp_input(src, payload, plen);
    else if (ip->proto == IPPROTO_UDP) udp_input(src, payload, plen);
    else if (ip->proto == IPPROTO_TCP) tcp_input(src, payload, plen);
}

static void arp_input(const uint8_t *p, uint32_t len) {
    if (len < sizeof(arp_pkt_t)) return;
    const arp_pkt_t *a = (const arp_pkt_t *)p;
    if (ntohs_(a->ptype) != ETHERTYPE_IP) return;
    uint32_t spa, tpa;
    kmemcpy(&spa, a->spa, 4); spa = ntohl_(spa);
    kmemcpy(&tpa, a->tpa, 4); tpa = ntohl_(tpa);

    arp_cache_put(spa, a->sha);                  /* learn the sender */
    sched_lock(); wait_queue_wake_all_locked(&g_arp_wq); sched_unlock();

    if (ntohs_(a->op) == ARP_REQUEST && tpa == LOCAL_IP)
        arp_send(ARP_REPLY, a->sha, spa);        /* "I am LOCAL_IP" */
}

/* ---- TCP ----
 *
 * A WAN-grade state machine: 3-way handshake (active + passive open), a send
 * buffer with go-back-N retransmission driven by an RTO derived from Jacobson/
 * Karels RTT estimation (Karn's algorithm on retransmits), congestion control
 * (slow start + congestion avoidance + fast retransmit on triple dup ACKs),
 * out-of-order reassembly, delayed ACKs, MSS-option negotiation, a zero-window
 * persist probe, and a 2*MSL TIME-WAIT.
 *
 * Timing is driven by tcp_timer(), run in the net-worker context (where loopback
 * sends defer to the worker-private loq, never re-entering sched_lock) by a
 * 50 ms ticker that wakes the worker. All PCB state is mutated under sched_lock:
 * tcp_input(), tcp_timer(), and the socket-API helpers all hold it, so the
 * receive path, the timers, and app calls never race. */

typedef enum {
    T_CLOSED, T_LISTEN, T_SYN_SENT, T_SYN_RCVD, T_ESTABLISHED,
    T_FIN_WAIT_1, T_FIN_WAIT_2, T_CLOSE_WAIT, T_CLOSING, T_LAST_ACK, T_TIME_WAIT
} tcp_state_t;

#define TCP_PCB_N   32
#define TCP_RXBUF   16384
#define TCP_SNDBUF  16384
#define TCP_MSS     1460
#define TCP_ACCEPTQ 8
#define TCP_OOO_N   8                 /* out-of-order reassembly slots */

/* Timers, in 100 Hz ticks (1 tick = 10 ms). */
#define TCP_RTO_INIT   100            /* 1 s before the first RTT sample */
#define TCP_RTO_MIN    20             /* 200 ms floor */
#define TCP_RTO_MAX    6000           /* 60 s ceiling */
#define TCP_2MSL       200            /* TIME-WAIT linger (2 s; real WAN = 2*MSL) */
#define TCP_DELACK     10             /* delayed-ACK delay (100 ms) */
#define TCP_PERSIST_MAX 6             /* zero-window probe backoff cap */
#define TCP_REXMIT_MAX 8              /* drop a connection after this many RTOs */

/* An out-of-order segment held until the gap before it is filled. */
typedef struct { int used; uint32_t seq; uint32_t len; uint8_t data[TCP_MSS]; } tcp_ooo_t;

typedef struct tcp_pcb {
    int          used;
    tcp_state_t  state;
    uint16_t     local_port, remote_port;
    uint32_t     local_ip, remote_ip;

    /* send sequence space */
    uint32_t     iss, snd_una, snd_nxt;
    uint32_t     snd_wnd;                      /* peer's advertised window */
    uint32_t     snd_wl1, snd_wl2;             /* seq/ack of last window update */
    uint16_t     snd_mss;                      /* min(our MSS, peer's MSS option) */
    int          syn_acked;

    /* send buffer: unacked + unsent DATA bytes, seq [snd_buf_seq, +snd_buf_len) */
    uint8_t      sndbuf[TCP_SNDBUF];
    uint32_t     snd_buf_seq;                  /* seq of sndbuf[snd_buf_off] */
    uint32_t     snd_buf_off, snd_buf_len;
    int          need_fin;                     /* app closed: emit FIN after data */
    uint32_t     fin_seq;                      /* seq the FIN occupies */
    int          need_output;                  /* app queued work; worker must pump */

    /* congestion control + RTT/RTO (Jacobson/Karels) */
    uint32_t     cwnd, ssthresh;
    uint32_t     rto, srtt, rttvar;            /* srtt scaled x8, rttvar scaled x4 */
    int          rtt_active; uint32_t rtt_seq; uint64_t rtt_start;
    int          rexmit_running, backoff, dupacks;
    uint64_t     rto_deadline;

    /* receive sequence space */
    uint32_t     rcv_nxt;
    uint8_t      rxbuf[TCP_RXBUF];
    uint32_t     rxhead, rxtail;               /* byte ring (head=producer) */
    tcp_ooo_t    ooo[TCP_OOO_N];
    int          peer_fin;                     /* received a FIN (EOF) */
    int          reset;                        /* connection reset */
    int          delack; uint64_t delack_deadline;
    int          need_ack;                     /* owe a window-update ACK (rx drained) */

    uint64_t     tw_deadline;                  /* TIME-WAIT expiry */

    struct tcp_pcb *listener;                  /* parent, for SYN_RCVD children */
    struct tcp_pcb *acceptq[TCP_ACCEPTQ];      /* established, awaiting accept() */
    int          aq_head, aq_tail;
    int          app_closed;                   /* app called close(); free once dead */
    wait_queue_t wq;                           /* any state change wakes waiters */
} tcp_pcb_t;

static tcp_pcb_t g_tcp[TCP_PCB_N];
static volatile int g_tcp_timer_pending;       /* ticker -> worker: run tcp_timer() */
static volatile int g_tcp_work;                /* app -> worker: pcbs need_output */

static inline int seq_lt(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
static inline int seq_gt(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }
static inline int seq_le(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }
static inline int seq_ge(uint32_t a, uint32_t b) { return (int32_t)(a - b) >= 0; }

static uint16_t l4_checksum(uint8_t proto, uint32_t src, uint32_t dst,
                            const uint8_t *seg, uint32_t len) {
    uint32_t sum = 0;
    sum += (src >> 16) & 0xFFFF; sum += src & 0xFFFF;
    sum += (dst >> 16) & 0xFFFF; sum += dst & 0xFFFF;
    sum += proto; sum += len;
    for (uint32_t i = 0; i + 1 < len; i += 2) sum += (seg[i] << 8) | seg[i + 1];
    if (len & 1) sum += seg[len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static uint32_t tcp_rx_used(tcp_pcb_t *t) { return (t->rxhead - t->rxtail) % TCP_RXBUF; }
static uint32_t tcp_rx_free(tcp_pcb_t *t) { return TCP_RXBUF - 1 - tcp_rx_used(t); }
static uint32_t tcp_snd_free(tcp_pcb_t *t) { return TCP_SNDBUF - t->snd_buf_len; }
static uint32_t tcp_flight(tcp_pcb_t *t) { return t->snd_nxt - t->snd_una; }

static tcp_pcb_t *tcp_alloc(void) {
    for (int i = 0; i < TCP_PCB_N; i++) {
        if (g_tcp[i].used) continue;
        kmemset(&g_tcp[i], 0, sizeof g_tcp[i]);
        g_tcp[i].used = 1;
        g_tcp[i].snd_mss = TCP_MSS;
        g_tcp[i].cwnd = 4 * TCP_MSS;           /* RFC 6928-ish initial window */
        g_tcp[i].ssthresh = 0xFFFF;
        g_tcp[i].rto = TCP_RTO_INIT;
        return &g_tcp[i];
    }
    return nullptr;
}

/* Copy `n` send-buffer bytes for sequence `seq` into `dst`. */
static void snd_copyout(tcp_pcb_t *t, uint32_t seq, uint8_t *dst, uint32_t n) {
    uint32_t off = seq - t->snd_buf_seq;
    uint32_t idx = (t->snd_buf_off + off) % TCP_SNDBUF;
    for (uint32_t i = 0; i < n; i++) { dst[i] = t->sndbuf[idx]; idx = (idx + 1) % TCP_SNDBUF; }
}

/* Emit one segment with explicit seq/flags/payload. Does NOT advance snd_nxt;
   tcp_output() owns sequence advancement. Sending any segment clears delack
   (the segment carries our current rcv_nxt). With `mss_opt`, append an MSS
   option (used on SYN / SYN-ACK). */
static void tcp_send_seg(tcp_pcb_t *t, uint32_t seq, uint8_t flags,
                         const uint8_t *data, uint32_t len, int mss_opt) {
    uint8_t seg[sizeof(tcp_hdr_t) + 4 + TCP_MSS];
    tcp_hdr_t *th = (tcp_hdr_t *)seg;
    uint32_t hlen = sizeof(tcp_hdr_t);
    th->src_port = htons_(t->local_port);
    th->dst_port = htons_(t->remote_port);
    th->seq = htonl_(seq);
    th->ack = htonl_(t->rcv_nxt);
    th->flags = flags;
    uint32_t freew = tcp_rx_free(t);
    th->window = htons_((uint16_t)(freew > 0xFFFF ? 0xFFFF : freew));
    th->csum = 0; th->urg = 0;
    if (mss_opt) {                              /* kind=2, len=4, value */
        seg[hlen + 0] = 2; seg[hlen + 1] = 4;
        seg[hlen + 2] = (uint8_t)(TCP_MSS >> 8); seg[hlen + 3] = (uint8_t)TCP_MSS;
        hlen += 4;
    }
    th->data_off = (uint8_t)((hlen / 4) << 4);
    if (len) kmemcpy(seg + hlen, data, len);
    uint32_t tot = hlen + len;
    uint32_t sip = is_loopback(t->remote_ip) ? t->remote_ip : LOCAL_IP;
    th->csum = htons_(l4_checksum(IPPROTO_TCP, sip, t->remote_ip, seg, tot));
    ip_send(t->remote_ip, IPPROTO_TCP, seg, tot);
    t->delack = 0;
}

/* Arm/disarm the retransmission timer based on whether data is in flight. */
static void tcp_set_rexmit(tcp_pcb_t *t) {
    if (seq_lt(t->snd_una, t->snd_nxt)) {
        t->rexmit_running = 1;
        t->rto_deadline = timer_ticks() + t->rto;
    } else {
        t->rexmit_running = 0;
        t->backoff = 0;
    }
}

/* The sequence one past the last byte we have to send (data + an optional FIN). */
static uint32_t tcp_send_high(tcp_pcb_t *t) {
    uint32_t hi = t->snd_buf_seq + t->snd_buf_len;
    if (t->need_fin) hi = t->fin_seq + 1;
    return hi;
}

/* Send whatever the window allows: SYN, new data segments, and/or FIN, advancing
   snd_nxt. Re-arms the RTO timer. Called after every state change that may free
   window or queue data. Runs under sched_lock. */
static void tcp_output(tcp_pcb_t *t) {
    if (t->state == T_SYN_SENT) {               /* (re)send the SYN */
        if (t->snd_nxt == t->iss) { tcp_send_seg(t, t->iss, TCP_SYN, nullptr, 0, 1); t->snd_nxt = t->iss + 1; }
        tcp_set_rexmit(t);
        return;
    }
    if (t->state == T_SYN_RCVD) {               /* (re)send the SYN-ACK */
        if (t->snd_nxt == t->iss) { tcp_send_seg(t, t->iss, TCP_SYN | TCP_ACK, nullptr, 0, 1); t->snd_nxt = t->iss + 1; }
        tcp_set_rexmit(t);
        return;
    }
    if (t->state != T_ESTABLISHED && t->state != T_CLOSE_WAIT &&
        t->state != T_FIN_WAIT_1 && t->state != T_CLOSING && t->state != T_LAST_ACK)
        return;

    uint32_t mss = t->snd_mss ? t->snd_mss : TCP_MSS;
    uint32_t cwnd_win = t->cwnd;
    uint32_t awnd = t->snd_wnd;
    uint32_t win = cwnd_win < awnd ? cwnd_win : awnd;
    uint32_t win_end = t->snd_una + win;
    uint32_t data_hi = t->snd_buf_seq + t->snd_buf_len;
    int sent_any = 0;

    /* New data segments. */
    while (seq_lt(t->snd_nxt, data_hi) && seq_lt(t->snd_nxt, win_end)) {
        uint32_t n = data_hi - t->snd_nxt;
        uint32_t room = win_end - t->snd_nxt;
        if (n > room) n = room;
        if (n > mss) n = mss;
        if (n == 0) break;
        uint8_t buf[TCP_MSS];
        snd_copyout(t, t->snd_nxt, buf, n);
        uint8_t fl = TCP_ACK | TCP_PSH;
        tcp_send_seg(t, t->snd_nxt, fl, buf, n, 0);
        if (!t->rtt_active) { t->rtt_active = 1; t->rtt_seq = t->snd_nxt; t->rtt_start = timer_ticks(); }
        t->snd_nxt += n;
        sent_any = 1;
    }

    /* FIN, once all data has been sent and the window admits it. */
    if (t->need_fin && t->snd_nxt == t->fin_seq && seq_lt(t->snd_nxt, win_end + 1)) {
        uint8_t fl = TCP_FIN | TCP_ACK;
        tcp_send_seg(t, t->snd_nxt, fl, nullptr, 0, 0);
        t->snd_nxt += 1;
        sent_any = 1;
    }
    /* Window-update ACK: the app drained the rxbuf and reopened the receive
       window, but we had no data/FIN to piggyback it on — send a bare ACK so a
       blocked sender learns the window is open again (else large transfers that
       fill RCVBUF stall forever). */
    if (t->need_ack && !sent_any) tcp_send_seg(t, t->snd_nxt, TCP_ACK, nullptr, 0, 0);
    t->need_ack = 0;
    tcp_set_rexmit(t);
}

/* App-context request to transmit: mark the pcb and wake the worker. Because a
   loopback send re-enters the sched_lock-protected rx path, app threads (which
   hold sched_lock) must never call tcp_output() directly — they defer it to the
   worker, where loopback sends queue to the worker-private loq instead. Caller
   holds sched_lock. */
static void tcp_kick(tcp_pcb_t *t) {
    t->need_output = 1;
    g_tcp_work = 1;
    wait_queue_wake_all_locked(&g_rxq_wq);
}

/* Worker-context: flush every pcb an app thread queued output on. Under lock. */
static void tcp_pump(void) {
    for (int i = 0; i < TCP_PCB_N; i++) {
        tcp_pcb_t *t = &g_tcp[i];
        if (!t->used || !t->need_output) continue;
        t->need_output = 0;
        tcp_output(t);
        wait_queue_wake_all_locked(&t->wq);
    }
}

/* Send a bare RST in response to a segment with no matching connection. */
static void tcp_send_rst(uint32_t dst, uint16_t dport, uint16_t sport, uint32_t ack) {
    tcp_hdr_t th;
    kmemset(&th, 0, sizeof th);
    th.src_port = htons_(dport); th.dst_port = htons_(sport);
    th.seq = htonl_(0); th.ack = htonl_(ack);
    th.data_off = (sizeof(tcp_hdr_t) / 4) << 4;
    th.flags = TCP_RST | TCP_ACK;
    uint32_t sip = is_loopback(dst) ? dst : LOCAL_IP;
    th.csum = htons_(l4_checksum(IPPROTO_TCP, sip, dst, (uint8_t *)&th, sizeof th));
    ip_send(dst, IPPROTO_TCP, &th, sizeof th);
}

/* Parse a peer SYN's options for the MSS option; clamp our send MSS. */
static void tcp_parse_mss(tcp_pcb_t *t, const uint8_t *p, uint32_t doff) {
    const uint8_t *o = p + sizeof(tcp_hdr_t);
    const uint8_t *end = p + doff;
    while (o < end) {
        uint8_t kind = o[0];
        if (kind == 0) break;                   /* EOL */
        if (kind == 1) { o++; continue; }       /* NOP */
        if (o + 1 >= end) break;
        uint8_t olen = o[1];
        if (olen < 2 || o + olen > end) break;
        if (kind == 2 && olen == 4) {
            uint16_t pm = (uint16_t)((o[2] << 8) | o[3]);
            if (pm && pm < t->snd_mss) t->snd_mss = pm;
        }
        o += olen;
    }
}

/* ---- kernel-side TCP API (wrapped by socket syscalls in the next rung) ---- */

static tcp_pcb_t *tcp_listen(uint16_t port) {
    tcp_pcb_t *t = tcp_alloc();
    if (!t) return nullptr;
    t->state = T_LISTEN;
    t->local_port = port;
    t->local_ip = LOCAL_IP;
    return t;
}

static tcp_pcb_t *tcp_accept(tcp_pcb_t *l) {
    sched_lock();
    while (l->used && l->aq_head == l->aq_tail)
        wait_queue_sleep_locked(&l->wq);
    tcp_pcb_t *c = nullptr;
    if (l->aq_head != l->aq_tail) {
        c = l->acceptq[l->aq_tail];
        l->aq_tail = (l->aq_tail + 1) % TCP_ACCEPTQ;
    }
    sched_unlock();
    return c;
}

static tcp_pcb_t *tcp_connect(uint32_t dip, uint16_t dport) {
    tcp_pcb_t *t = tcp_alloc();
    if (!t) return nullptr;
    t->state = T_SYN_SENT;
    t->local_ip = is_loopback(dip) ? dip : LOCAL_IP;
    t->local_port = g_ephemeral++;
    t->remote_ip = dip; t->remote_port = dport;
    t->iss = csprng_tcp_isn(t->local_ip, t->local_port, t->remote_ip, t->remote_port);
    t->snd_una = t->snd_nxt = t->iss;
    t->snd_buf_seq = t->iss + 1;                /* first data byte lands here */
    t->snd_wnd = TCP_MSS;                        /* until the SYN-ACK tells us more */
    sched_lock();
    tcp_kick(t);                                /* worker emits the SYN */
    while (t->used && t->state == T_SYN_SENT && !t->reset)
        wait_queue_sleep_locked(&t->wq);
    int ok = t->used && t->state == T_ESTABLISHED;
    sched_unlock();
    if (!ok) { t->used = 0; return nullptr; }
    return t;
}

static int tcp_write(tcp_pcb_t *t, const uint8_t *data, uint32_t len, int nonblock) {
    uint32_t sent = 0;
    while (sent < len) {
        sched_lock();
        if (nonblock && t->used && !t->reset &&
            (t->state == T_ESTABLISHED || t->state == T_CLOSE_WAIT) &&
            tcp_snd_free(t) == 0) {
            sched_unlock();
            return sent ? (int)sent : -11;       /* -EAGAIN: send buffer full */
        }
        while (t->used && !t->reset &&
               (t->state == T_ESTABLISHED || t->state == T_CLOSE_WAIT) &&
               tcp_snd_free(t) == 0)
            wait_queue_sleep_locked(&t->wq);     /* wait for buffer room */
        if (!t->used || t->reset ||
            (t->state != T_ESTABLISHED && t->state != T_CLOSE_WAIT)) {
            sched_unlock();
            return sent ? (int)sent : -1;        /* connection no longer writable */
        }
        uint32_t room = tcp_snd_free(t);
        uint32_t chunk = len - sent;
        if (chunk > room) chunk = room;
        for (uint32_t i = 0; i < chunk; i++) {
            uint32_t idx = (t->snd_buf_off + t->snd_buf_len) % TCP_SNDBUF;
            t->sndbuf[idx] = data[sent + i];
            t->snd_buf_len++;
        }
        sent += chunk;
        tcp_kick(t);                             /* worker pushes within the window */
        sched_unlock();
    }
    return (int)sent;
}

/* Blocking read of up to `len` bytes; returns 0 at EOF (peer FIN, buffer drained). */
static int tcp_read(tcp_pcb_t *t, uint8_t *buf, uint32_t len, int nonblock) {
    sched_lock();
    if (nonblock && t->used && tcp_rx_used(t) == 0 && !t->peer_fin && !t->reset) {
        sched_unlock();
        return -11;                              /* -EAGAIN: nothing buffered */
    }
    while (t->used && tcp_rx_used(t) == 0 && !t->peer_fin && !t->reset)
        wait_queue_sleep_locked(&t->wq);
    if (!t->used || t->reset) { sched_unlock(); return -1; }
    uint32_t avail = tcp_rx_used(t);
    uint32_t n = avail < len ? avail : len;
    int was_full = (tcp_rx_free(t) == 0);
    for (uint32_t i = 0; i < n; i++) {
        buf[i] = t->rxbuf[t->rxtail];
        t->rxtail = (t->rxtail + 1) % TCP_RXBUF;
    }
    int eof = (n == 0 && t->peer_fin);
    if (was_full && n > 0) { t->need_ack = 1; tcp_kick(t); }   /* window opened: advertise it */
    sched_unlock();
    return eof ? 0 : (int)n;
}

static void tcp_close(tcp_pcb_t *t) {
    sched_lock();
    t->app_closed = 1;
    if (t->used && (t->state == T_ESTABLISHED || t->state == T_CLOSE_WAIT)) {
        t->need_fin = 1;
        t->fin_seq = t->snd_buf_seq + t->snd_buf_len;   /* after all queued data */
        t->state = (t->state == T_CLOSE_WAIT) ? T_LAST_ACK : T_FIN_WAIT_1;
        tcp_kick(t);                            /* worker emits the FIN once data drains */
    } else if (t->state != T_TIME_WAIT && t->state != T_FIN_WAIT_1 &&
               t->state != T_FIN_WAIT_2 && t->state != T_CLOSING &&
               t->state != T_LAST_ACK) {
        t->used = 0;                            /* LISTEN / SYN_SENT: drop */
    }
    sched_unlock();
}

/* ---- TCP receive path (worker context, under sched_lock) ---- */

/* Append in-order bytes [seq, seq+plen) to the rx ring; returns bytes accepted. */
static uint32_t tcp_rx_append(tcp_pcb_t *t, const uint8_t *data, uint32_t plen) {
    uint32_t freew = tcp_rx_free(t);
    uint32_t n = plen < freew ? plen : freew;
    for (uint32_t i = 0; i < n; i++) {
        t->rxbuf[t->rxhead] = data[i];
        t->rxhead = (t->rxhead + 1) % TCP_RXBUF;
    }
    t->rcv_nxt += n;
    return n;
}

/* After rcv_nxt advances, splice in any OOO segments that are now contiguous. */
static void tcp_reassemble(tcp_pcb_t *t) {
    int progress = 1;
    while (progress) {
        progress = 0;
        for (int i = 0; i < TCP_OOO_N; i++) {
            tcp_ooo_t *o = &t->ooo[i];
            if (!o->used) continue;
            if (seq_le(o->seq, t->rcv_nxt) && seq_gt(o->seq + o->len, t->rcv_nxt)) {
                uint32_t skip = t->rcv_nxt - o->seq;
                tcp_rx_append(t, o->data + skip, o->len - skip);
                o->used = 0; progress = 1;
            } else if (seq_le(o->seq + o->len, t->rcv_nxt)) {
                o->used = 0;                    /* wholly stale */
            }
        }
    }
}

/* Buffer an out-of-order segment for later reassembly (best effort). */
static void tcp_ooo_store(tcp_pcb_t *t, uint32_t seq, const uint8_t *data, uint32_t plen) {
    if (plen > TCP_MSS) plen = TCP_MSS;
    for (int i = 0; i < TCP_OOO_N; i++)         /* drop exact duplicates */
        if (t->ooo[i].used && t->ooo[i].seq == seq && t->ooo[i].len >= plen) return;
    for (int i = 0; i < TCP_OOO_N; i++) {
        if (t->ooo[i].used) continue;
        t->ooo[i].used = 1; t->ooo[i].seq = seq; t->ooo[i].len = plen;
        kmemcpy(t->ooo[i].data, data, plen);
        return;
    }
    /* table full: drop — the sender will retransmit */
}

/* Receive-side data handling: in-order delivery, OOO buffering, and the ACK
   policy (immediate dup-ACK out of order, else delayed ACK). */
static void tcp_recv_data(tcp_pcb_t *t, uint32_t seq, const uint8_t *data, uint32_t plen) {
    if (plen == 0) return;
    uint32_t seg_end = seq + plen;
    if (seq_le(seg_end, t->rcv_nxt)) {          /* fully old: ACK to re-sync peer */
        tcp_send_seg(t, t->snd_nxt, TCP_ACK, nullptr, 0, 0);
        return;
    }
    if (seq == t->rcv_nxt) {                     /* in order */
        tcp_rx_append(t, data, plen);
        tcp_reassemble(t);
        if (++t->delack >= 2) {                  /* ACK every second segment */
            tcp_send_seg(t, t->snd_nxt, TCP_ACK, nullptr, 0, 0);
            t->delack = 0;
        } else {
            t->delack_deadline = timer_ticks() + TCP_DELACK;
        }
    } else if (seq_gt(seq, t->rcv_nxt)) {        /* gap: buffer + immediate dup ACK */
        tcp_ooo_store(t, seq, data, plen);
        tcp_send_seg(t, t->snd_nxt, TCP_ACK, nullptr, 0, 0);
    } else {                                     /* partial overlap below rcv_nxt */
        uint32_t skip = t->rcv_nxt - seq;
        tcp_rx_append(t, data + skip, plen - skip);
        tcp_reassemble(t);
        tcp_send_seg(t, t->snd_nxt, TCP_ACK, nullptr, 0, 0);
        t->delack = 0;
    }
}

/* Process an incoming ACK: advance snd_una, free the send buffer, sample RTT,
   grow the congestion window, and fast-retransmit on triple dup ACKs. */
static void tcp_recv_ack(tcp_pcb_t *t, uint32_t ack, uint16_t win, uint32_t seq) {
    uint32_t mss = t->snd_mss ? t->snd_mss : TCP_MSS;

    if (seq_gt(ack, t->snd_una) && seq_le(ack, t->snd_nxt)) {
        uint32_t acked = ack - t->snd_una;

        /* Free acknowledged DATA from the send buffer. */
        if (seq_gt(ack, t->snd_buf_seq)) {
            uint32_t fb = ack - t->snd_buf_seq;
            if (fb > t->snd_buf_len) fb = t->snd_buf_len;
            t->snd_buf_off = (t->snd_buf_off + fb) % TCP_SNDBUF;
            t->snd_buf_len -= fb;
            t->snd_buf_seq += fb;
        }
        t->snd_una = ack;
        t->dupacks = 0;
        t->backoff = 0;

        /* RTT sample (Karn: only for non-retransmitted segments). */
        if (t->rtt_active && seq_ge(ack, t->rtt_seq + 1)) {
            uint32_t m = (uint32_t)(timer_ticks() - t->rtt_start);
            if ((int32_t)m < 1) m = 1;
            if (!t->srtt) { t->srtt = m << 3; t->rttvar = m << 1; }
            else {
                int32_t err = (int32_t)m - (int32_t)(t->srtt >> 3);
                t->srtt = (uint32_t)((int32_t)t->srtt + err);
                if (err < 0) err = -err;
                t->rttvar = (uint32_t)((int32_t)t->rttvar + (err - (int32_t)(t->rttvar >> 2)));
            }
            t->rto = (t->srtt >> 3) + t->rttvar;
            if (t->rto < TCP_RTO_MIN) t->rto = TCP_RTO_MIN;
            if (t->rto > TCP_RTO_MAX) t->rto = TCP_RTO_MAX;
            t->rtt_active = 0;
        }

        /* Congestion window growth. */
        if (t->cwnd < t->ssthresh) t->cwnd += mss;                  /* slow start */
        else t->cwnd += mss * mss / (t->cwnd ? t->cwnd : mss);      /* cong. avoidance */
        if (t->cwnd > 0xFFFFFF) t->cwnd = 0xFFFFFF;
        (void)acked;
    } else if (ack == t->snd_una && seq_lt(t->snd_una, t->snd_nxt)) {
        /* Duplicate ACK: count, fast-retransmit on the third. */
        if (++t->dupacks == 3) {
            uint32_t flight = tcp_flight(t);
            t->ssthresh = (flight / 2 > 2u * mss) ? flight / 2 : 2u * mss;
            t->cwnd = t->ssthresh;
            t->snd_nxt = t->snd_una;             /* go-back-N from the gap */
            t->rtt_active = 0;
            tcp_output(t);
        }
    }

    /* Update the send window (SND.WND) on newer info. */
    if (seq_lt(t->snd_wl1, seq) || (t->snd_wl1 == seq && seq_le(t->snd_wl2, ack))) {
        t->snd_wnd = win; t->snd_wl1 = seq; t->snd_wl2 = ack;
    }
    if (win == 0 && seq_lt(t->snd_una, tcp_send_high(t))) {
        /* zero window: leave the RTO running as a persist timer */
    }
}

static void tcp_enter_timewait(tcp_pcb_t *t) {
    t->state = T_TIME_WAIT;
    t->rexmit_running = 0;
    t->tw_deadline = timer_ticks() + TCP_2MSL;
}

static void tcp_input(uint32_t src, const uint8_t *p, uint32_t len) {
    if (len < sizeof(tcp_hdr_t)) return;
    const tcp_hdr_t *th = (const tcp_hdr_t *)p;
    uint16_t sport = ntohs_(th->src_port), dport = ntohs_(th->dst_port);
    uint32_t seq = ntohl_(th->seq), ack = ntohl_(th->ack);
    uint8_t  flags = th->flags;
    uint16_t win = ntohs_(th->window);
    uint32_t doff = (th->data_off >> 4) * 4;
    if (doff < sizeof(tcp_hdr_t) || doff > len) return;
    const uint8_t *data = p + doff;
    uint32_t plen = len - doff;

    sched_lock();

    /* Find an exact connection match, else a LISTEN on the port. */
    tcp_pcb_t *t = nullptr, *lst = nullptr;
    for (int i = 0; i < TCP_PCB_N; i++) {
        tcp_pcb_t *c = &g_tcp[i];
        if (!c->used) continue;
        if (c->state != T_LISTEN && c->local_port == dport &&
            c->remote_port == sport && c->remote_ip == src) { t = c; break; }
        if (c->state == T_LISTEN && c->local_port == dport) lst = c;
    }

    if (!t) {
        if (lst && (flags & TCP_SYN) && !(flags & TCP_ACK)) {
            tcp_pcb_t *c = tcp_alloc();
            if (c) {
                c->state = T_SYN_RCVD;
                c->local_ip = lst->local_ip; c->local_port = dport;
                c->remote_ip = src; c->remote_port = sport;
                c->iss = csprng_tcp_isn(c->local_ip, c->local_port, c->remote_ip, c->remote_port);
                c->snd_una = c->snd_nxt = c->iss;
                c->snd_buf_seq = c->iss + 1;
                c->rcv_nxt = seq + 1;            /* +1 for the SYN */
                c->snd_wnd = win ? win : TCP_MSS;
                c->snd_wl1 = seq; c->snd_wl2 = c->iss;
                c->listener = lst;
                tcp_parse_mss(c, p, doff);
                tcp_output(c);                   /* SYN-ACK */
            }
        } else if (!(flags & TCP_RST)) {
            tcp_send_rst(src, dport, sport, seq + plen + ((flags & TCP_SYN) ? 1 : 0));
        }
        sched_unlock();
        return;
    }

    if (flags & TCP_RST) {
        t->reset = 1; t->state = T_CLOSED;
        wait_queue_wake_all_locked(&t->wq);
        if (t->app_closed) t->used = 0;
        sched_unlock();
        return;
    }

    /* A retransmitted SYN-ACK or FIN reaching TIME-WAIT: re-ACK and restart 2MSL. */
    if (t->state == T_TIME_WAIT) {
        tcp_send_seg(t, t->snd_nxt, TCP_ACK, nullptr, 0, 0);
        t->tw_deadline = timer_ticks() + TCP_2MSL;
        sched_unlock();
        return;
    }

    if (flags & TCP_ACK) tcp_recv_ack(t, ack, win, seq);

    switch (t->state) {
    case T_SYN_SENT:
        if ((flags & TCP_SYN) && (flags & TCP_ACK)) {
            t->rcv_nxt = seq + 1;
            t->snd_wnd = win ? win : TCP_MSS;
            t->snd_wl1 = seq; t->snd_wl2 = ack;
            t->syn_acked = 1;
            tcp_parse_mss(t, p, doff);
            t->state = T_ESTABLISHED;
            tcp_send_seg(t, t->snd_nxt, TCP_ACK, nullptr, 0, 0);
            tcp_output(t);                       /* flush any queued data */
        } else if (flags & TCP_SYN) {            /* simultaneous open */
            t->rcv_nxt = seq + 1;
            t->state = T_SYN_RCVD;
            tcp_output(t);
        }
        break;
    case T_SYN_RCVD:
        if (flags & TCP_ACK) {
            t->state = T_ESTABLISHED;
            t->syn_acked = 1;
            tcp_pcb_t *l = t->listener;          /* hand to accept() */
            if (l) {
                int next = (l->aq_head + 1) % TCP_ACCEPTQ;
                if (next != l->aq_tail) { l->acceptq[l->aq_head] = t; l->aq_head = next; }
                wait_queue_wake_all_locked(&l->wq);
            }
        }
        break;
    default: break;
    }

    if (t->state == T_ESTABLISHED || t->state == T_FIN_WAIT_1 ||
        t->state == T_FIN_WAIT_2 || t->state == T_CLOSING) {
        if (plen) tcp_recv_data(t, seq, data, plen);
    }

    /* FIN handling (only once it is the next in-order byte). */
    if ((flags & TCP_FIN) && seq_le(seq, t->rcv_nxt) &&
        seq + plen == t->rcv_nxt && !t->peer_fin) {
        t->rcv_nxt += 1;
        t->peer_fin = 1;
        tcp_send_seg(t, t->snd_nxt, TCP_ACK, nullptr, 0, 0);
        if (t->state == T_ESTABLISHED)      t->state = T_CLOSE_WAIT;
        else if (t->state == T_FIN_WAIT_1)  t->state = T_CLOSING;
        else if (t->state == T_FIN_WAIT_2)  tcp_enter_timewait(t);
    }

    /* Our FIN being ACKed (snd_una has reached snd_nxt with the FIN counted). */
    if ((flags & TCP_ACK) && t->snd_una == t->snd_nxt && t->need_fin &&
        seq_gt(t->snd_una, t->fin_seq)) {
        if (t->state == T_FIN_WAIT_1)     t->state = t->peer_fin ? (tcp_enter_timewait(t), T_TIME_WAIT) : T_FIN_WAIT_2;
        else if (t->state == T_CLOSING)   tcp_enter_timewait(t);
        else if (t->state == T_LAST_ACK)  { t->state = T_CLOSED; if (t->app_closed) t->used = 0; }
    }

    /* Otherwise push any newly-admitted data/window forward. */
    if (t->used && (t->state == T_ESTABLISHED || t->state == T_CLOSE_WAIT))
        tcp_output(t);

    wait_queue_wake_all_locked(&t->wq);
    sched_unlock();
}

/* The TCP slow/fast timer: retransmission, delayed-ACK flush, zero-window
   persist, and TIME-WAIT expiry. Runs in the worker context under sched_lock,
   so its loopback sends defer to the worker's loq like any other rx-path send. */
static void tcp_timer(void) {
    uint64_t now = timer_ticks();
    for (int i = 0; i < TCP_PCB_N; i++) {
        tcp_pcb_t *t = &g_tcp[i];
        if (!t->used) continue;

        if (t->state == T_TIME_WAIT) {
            if (seq_ge((uint32_t)now, (uint32_t)t->tw_deadline) && now >= t->tw_deadline) {
                t->used = 0; wait_queue_wake_all_locked(&t->wq);
            }
            continue;
        }

        /* Delayed-ACK flush. */
        if (t->delack && now >= t->delack_deadline) {
            tcp_send_seg(t, t->snd_nxt, TCP_ACK, nullptr, 0, 0);
            t->delack = 0;
        }

        /* Retransmission timeout. */
        if (t->rexmit_running && now >= t->rto_deadline) {
            if (++t->backoff > TCP_REXMIT_MAX) {        /* give up */
                t->reset = 1; t->state = T_CLOSED;
                wait_queue_wake_all_locked(&t->wq);
                if (t->app_closed) t->used = 0;
                continue;
            }
            t->rto <<= 1;
            if (t->rto > TCP_RTO_MAX) t->rto = TCP_RTO_MAX;
            uint32_t flight = tcp_flight(t);
            uint32_t mss = t->snd_mss ? t->snd_mss : TCP_MSS;
            t->ssthresh = (flight / 2 > 2u * mss) ? flight / 2 : 2u * mss;
            t->cwnd = mss;                              /* collapse to slow start */
            t->rtt_active = 0;                          /* Karn */
            t->dupacks = 0;
            t->snd_nxt = t->snd_una;                    /* go-back-N */

            if (t->snd_wnd == 0 && seq_lt(t->snd_una, tcp_send_high(t))) {
                /* Zero-window persist: probe one byte past the window. */
                uint32_t hi = t->snd_buf_seq + t->snd_buf_len;
                if (seq_lt(t->snd_una, hi)) {
                    uint8_t b; snd_copyout(t, t->snd_una, &b, 1);
                    tcp_send_seg(t, t->snd_una, TCP_ACK | TCP_PSH, &b, 1, 0);
                    if (t->snd_una == t->snd_nxt) t->snd_nxt = t->snd_una + 1;
                }
                t->rexmit_running = 1;
                t->rto_deadline = now + t->rto;
            } else {
                tcp_output(t);                          /* resend from snd_una */
                t->rexmit_running = 1;
                t->rto_deadline = now + t->rto;
            }
            wait_queue_wake_all_locked(&t->wq);
        }
    }
}

/* ---- socket layer (wraps the UDP/TCP PCBs for the syscall interface) ---- */

typedef struct ksocket {
    int        used;
    int        type;            /* SOCK_STREAM / SOCK_DGRAM */
    uint16_t   bind_port;       /* requested local port (bind) */
    udp_pcb_t *udp;
    tcp_pcb_t *tcp;
    uint32_t   peer_ip;         /* connected UDP peer */
    uint16_t   peer_port;
    int        connected;
} ksocket_t;

#define KSOCK_N 32
static ksocket_t g_sock[KSOCK_N];

static ksocket_t *sk_alloc(void) {
    for (int i = 0; i < KSOCK_N; i++)
        if (!g_sock[i].used) { kmemset(&g_sock[i], 0, sizeof g_sock[i]); g_sock[i].used = 1; return &g_sock[i]; }
    return nullptr;
}

void *ksock_create(int type) {
    if (type != SOCK_STREAM && type != SOCK_DGRAM) return nullptr;
    ksocket_t *s = sk_alloc();
    if (!s) return nullptr;
    s->type = type;
    if (type == SOCK_DGRAM) s->udp = udp_bind(0);   /* ephemeral until bind */
    return s;
}

int ksock_bind(void *vp, uint32_t ip, uint16_t port) {
    (void)ip;
    ksocket_t *s = (ksocket_t *)vp;
    s->bind_port = port;
    if (s->type == SOCK_DGRAM) {
        if (s->udp) udp_close(s->udp);
        s->udp = udp_bind(port);
        return s->udp ? 0 : -1;
    }
    return 0;                                        /* STREAM binds at listen */
}

int ksock_listen(void *vp, int backlog) {
    (void)backlog;
    ksocket_t *s = (ksocket_t *)vp;
    if (s->type != SOCK_STREAM) return -1;
    s->tcp = tcp_listen(s->bind_port);
    return s->tcp ? 0 : -1;
}

void *ksock_accept(void *vp, uint32_t *pip, uint16_t *pport) {
    ksocket_t *s = (ksocket_t *)vp;
    if (s->type != SOCK_STREAM || !s->tcp) return nullptr;
    tcp_pcb_t *c = tcp_accept(s->tcp);
    if (!c) return nullptr;
    ksocket_t *ns = sk_alloc();
    if (!ns) { return nullptr; }
    ns->type = SOCK_STREAM; ns->tcp = c; ns->connected = 1;
    if (pip)   *pip = c->remote_ip;
    if (pport) *pport = c->remote_port;
    return ns;
}

int ksock_connect(void *vp, uint32_t ip, uint16_t port) {
    ksocket_t *s = (ksocket_t *)vp;
    if (s->type == SOCK_STREAM) {
        s->tcp = tcp_connect(ip, port);
        if (!s->tcp) return -1;
        s->connected = 1;
        return 0;
    }
    s->peer_ip = ip; s->peer_port = port; s->connected = 1;  /* UDP: just remember */
    if (!s->udp) s->udp = udp_bind(0);
    return 0;
}

int ksock_sendto(void *vp, const void *buf, uint32_t len, uint32_t ip, uint16_t port, int nonblock) {
    ksocket_t *s = (ksocket_t *)vp;
    if (s->type == SOCK_STREAM) return s->tcp ? tcp_write(s->tcp, (const uint8_t *)buf, len, nonblock) : -1;
    if (!s->udp) s->udp = udp_bind(0);
    return udp_sendto(s->udp, ip, port, buf, len) < 0 ? -1 : (int)len;
}

int ksock_send(void *vp, const void *buf, uint32_t len, int nonblock) {
    ksocket_t *s = (ksocket_t *)vp;
    return ksock_sendto(vp, buf, len, s->peer_ip, s->peer_port, nonblock);
}

int ksock_recvfrom(void *vp, void *buf, uint32_t len, uint32_t *ip, uint16_t *port, int nonblock) {
    ksocket_t *s = (ksocket_t *)vp;
    if (s->type == SOCK_STREAM) return s->tcp ? tcp_read(s->tcp, (uint8_t *)buf, len, nonblock) : -1;
    return udp_recvfrom(s->udp, buf, len, ip, port);
}

int ksock_recv(void *vp, void *buf, uint32_t len, int nonblock) {
    return ksock_recvfrom(vp, buf, len, nullptr, nullptr, nonblock);
}

int ksock_poll(void *vp, int want) {
    ksocket_t *s = (ksocket_t *)vp;
    int r = 0;
    sched_lock();
    if (s->type == SOCK_STREAM && s->tcp) {
        tcp_pcb_t *t = s->tcp;
        if ((want & NET_POLLIN) &&
            (tcp_rx_used(t) > 0 || t->peer_fin || t->aq_head != t->aq_tail || t->reset))
            r |= NET_POLLIN;
        if ((want & NET_POLLOUT) &&
            (t->state == T_ESTABLISHED || t->state == T_CLOSE_WAIT) &&
            tcp_snd_free(t) > 0)
            r |= NET_POLLOUT;
    } else if (s->udp) {
        if ((want & NET_POLLIN) && s->udp->qhead != s->udp->qtail) r |= NET_POLLIN;
        if (want & NET_POLLOUT) r |= NET_POLLOUT;
    }
    sched_unlock();
    return r;
}

/* Report a socket's local (peer=0) or remote (peer=1) address in host order. */
int ksock_getname(void *vp, int peer, uint32_t *ip, uint16_t *port) {
    ksocket_t *s = (ksocket_t *)vp;
    if (!s || !s->used) return -1;
    if (s->type == SOCK_STREAM && s->tcp) {
        if (peer) { *ip = s->tcp->remote_ip; *port = s->tcp->remote_port; }
        else      { *ip = s->tcp->local_ip;  *port = s->tcp->local_port;  }
        return 0;
    }
    if (s->type == SOCK_DGRAM) {
        if (peer) {
            if (!s->connected) return -1;
            *ip = s->peer_ip; *port = s->peer_port;
        } else {
            *ip = LOCAL_IP; *port = s->udp ? s->udp->local_port : 0;
        }
        return 0;
    }
    return -1;
}

void ksock_close(void *vp) {
    ksocket_t *s = (ksocket_t *)vp;
    if (!s || !s->used) return;
    if (s->tcp) tcp_close(s->tcp);
    if (s->udp) udp_close(s->udp);
    s->used = 0;
}

static void net_input(const uint8_t *frame, uint32_t len) {
    if (len < ETH_HDR_LEN) return;
    const eth_hdr_t *eh = (const eth_hdr_t *)frame;
    uint16_t type = ntohs_(eh->type);
    const uint8_t *payload = frame + ETH_HDR_LEN;
    uint32_t plen = len - ETH_HDR_LEN;
    if (type == ETHERTYPE_ARP)      arp_input(payload, plen);
    else if (type == ETHERTYPE_IP)  ip_input(payload, plen);
}

/* ---- worker + self-test tasks ---- */

static void net_worker(void *arg) {
    (void)arg;
    g_net_worker = task_current();
    for (;;) {
        sched_lock();
        while (g_rxq_head == g_rxq_tail && !g_tcp_timer_pending && !g_tcp_work)
            wait_queue_sleep_locked(&g_rxq_wq);
        int run_timer = g_tcp_timer_pending; g_tcp_timer_pending = 0;
        g_tcp_work = 0;
        static uint8_t local[ETH_FRAME_MAX];
        uint16_t n = 0;
        if (g_rxq_head != g_rxq_tail) {
            rxslot_t *s = &g_rxq[g_rxq_tail];
            n = s->len;
            kmemcpy(local, s->data, n);
            g_rxq_tail = (g_rxq_tail + 1) % RXQ_N;
        }
        if (run_timer) tcp_timer();              /* drives RTO / delack / TIME-WAIT */
        tcp_pump();                              /* flush app-queued output */
        sched_unlock();
        if (n) net_input(local, n);

        /* Drain loopback frames this frame (and the timer) generated. */
        while (g_loq_head != g_loq_tail) {
            static uint8_t lb[ETH_FRAME_MAX];
            uint16_t ln = g_loq[g_loq_tail].len;
            kmemcpy(lb, g_loq[g_loq_tail].data, ln);
            g_loq_tail = (g_loq_tail + 1) % LOQ_N;
            net_input(lb, ln);
        }
    }
}

/* A 50 ms ticker that nudges the worker to run tcp_timer(). Kept separate from
   the worker so the worker can keep blocking on the rx queue between ticks. */
static void net_tcp_ticker(void *arg) {
    (void)arg;
    for (;;) {
        ksleep_ms(50);
        sched_lock();
        g_tcp_timer_pending = 1;
        wait_queue_wake_all_locked(&g_rxq_wq);
        sched_unlock();
    }
}

/* Originate a few pings to the gateway to prove TX/RX/ARP/IP/ICMP end to end. */
static void net_pinger(void *arg) {
    (void)arg;
    ksleep_ms(300);                              /* let the link settle */
    uint8_t gw[6];
    if (!arp_resolve(GATEWAY_IP, gw)) {
        kprintf("[net] ARP for gateway 10.0.2.2 failed\n");
        return;
    }
    kprintf("[net] gateway 10.0.2.2 is at %02x:%02x:%02x:%02x:%02x:%02x\n",
            gw[0], gw[1], gw[2], gw[3], gw[4], gw[5]);
    for (int i = 0; i < 3; i++) {
        icmp_hdr_t echo;
        echo.type = ICMP_ECHO_REQUEST; echo.code = 0; echo.csum = 0;
        echo.id = htons_(1); echo.seq = htons_((uint16_t)(i + 1));
        echo.csum = htons_(ip_checksum(&echo, sizeof echo));
        sched_lock(); g_ping_got = 0; sched_unlock();
        uint64_t t0 = timer_ticks();
        ip_send(GATEWAY_IP, IPPROTO_ICMP, &echo, sizeof echo);
        uint64_t deadline = t0 + 100;
        sched_lock();
        while (!g_ping_got && timer_ticks() < deadline)
            wait_queue_sleep_locked(&g_ping_wq);
        int got = g_ping_got; uint16_t seq = g_ping_seq;
        sched_unlock();
        if (got) kprintf("[net] ping 10.0.2.2: reply seq=%u (%lu ticks)\n",
                         seq, (unsigned long)(timer_ticks() - t0));
        else     kprintf("[net] ping 10.0.2.2: timeout (seq %d)\n", i + 1);
        ksleep_ms(200);
    }

    /* UDP over loopback: client -> server request, server -> client reply. */
    udp_pcb_t *srv = udp_bind(7777);
    udp_pcb_t *cli = udp_bind(0);
    if (srv && cli) {
        char buf[64]; uint32_t sip; uint16_t sport;
        udp_sendto(cli, LOCAL_IP, 7777, "ping-udp", 8);
        int n = udp_recvfrom(srv, buf, sizeof buf, &sip, &sport);
        kprintf("[net] udp server got %d bytes from :%u\n", n, sport);
        udp_sendto(srv, sip, sport, "pong-udp", 8);
        int m = udp_recvfrom(cli, buf, sizeof buf, &sip, &sport);
        buf[m > 0 ? m : 0] = '\0';
        kprintf("[net] udp client got %d bytes: \"%s\"\n", m, buf);
    }
    udp_close(srv); udp_close(cli);

    /* TCP over loopback: connect to the in-guest echo server, exchange, close. */
    tcp_pcb_t *c = tcp_connect(LOCAL_IP, 8080);
    if (c) {
        tcp_write(c, (const uint8_t *)"hello-tcp", 9, 0);
        uint8_t buf[256];
        int n = tcp_read(c, buf, sizeof buf, 0);
        buf[n > 0 ? n : 0] = '\0';
        kprintf("[net] tcp client connected, got %d bytes: \"%s\"\n", n, buf);
        tcp_close(c);
    } else {
        kprintf("[net] tcp connect to :8080 failed\n");
    }

    /* Bulk transfer: stream several MSS-worth through the send buffer / window. */
    tcp_pcb_t *b = tcp_connect(LOCAL_IP, 8081);
    if (b) {
        static uint8_t blk[6000];
        for (uint32_t i = 0; i < sizeof blk; i++) blk[i] = (uint8_t)('A' + (i % 26));
        int w = tcp_write(b, blk, sizeof blk, 0);
        kprintf("[net] tcp bulk wrote %d bytes to :8081\n", w);
        tcp_close(b);
    }
}

/* Tiny in-guest HTTP/1.0 server on port 80, so a userspace HTTP client (our
   ported wget) has something to fetch over loopback. Answers any request with a
   fixed page and closes. */
static void net_http_server(void *arg) {
    (void)arg;
    static const char body[] =
        "Hello from VibeOS!\n"
        "This page was fetched over our own virtio-net + TCP/IP stack.\n";
    tcp_pcb_t *l = tcp_listen(80);
    if (!l) return;
    for (;;) {
        tcp_pcb_t *c = tcp_accept(l);
        if (!c) continue;
        uint8_t req[512];
        tcp_read(c, req, sizeof req, 0);          /* consume the request line/headers */
        char resp[512];
        int blen = (int)sizeof(body) - 1;
        int hlen = 0;
        const char *hfmt =
            "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: ";
        for (const char *q = hfmt; *q; q++) resp[hlen++] = *q;
        /* itoa(blen) */
        char num[8]; int ni = 0, v = blen; if (!v) num[ni++] = '0';
        while (v) { num[ni++] = (char)('0' + v % 10); v /= 10; }
        while (ni) resp[hlen++] = num[--ni];
        const char *tail = "\r\nConnection: close\r\n\r\n";
        for (const char *q = tail; *q; q++) resp[hlen++] = *q;
        kmemcpy(resp + hlen, body, blen);
        tcp_write(c, (const uint8_t *)resp, hlen + blen, 0);
        tcp_close(c);                          /* HTTP/1.0: close right after the response */
    }
}

/* In-guest TCP echo server for the loopback self-test. */
static void net_tcp_server(void *arg) {
    (void)arg;
    tcp_pcb_t *l = tcp_listen(8080);
    if (!l) return;
    for (;;) {
        tcp_pcb_t *c = tcp_accept(l);
        if (!c) continue;
        uint8_t req[256];
        int n = tcp_read(c, req, sizeof req, 0);
        if (n > 0) {
            uint8_t rep[280];
            const char *pfx = "echo: ";
            int k = 0; while (pfx[k]) { rep[k] = (uint8_t)pfx[k]; k++; }
            kmemcpy(rep + k, req, n);
            tcp_write(c, rep, k + n, 0);
        }
        uint8_t drain[64];
        while (tcp_read(c, drain, sizeof drain, 0) > 0) { }   /* read to EOF */
        tcp_close(c);
    }
}

/* Bulk sink on port 8081: drain to EOF and report the byte count. Exercises the
   multi-segment send-buffer / window path (the echo test never exceeds one MSS). */
static void net_tcp_sink(void *arg) {
    (void)arg;
    tcp_pcb_t *l = tcp_listen(8081);
    if (!l) return;
    for (;;) {
        tcp_pcb_t *c = tcp_accept(l);
        if (!c) continue;
        uint8_t buf[1024];
        uint32_t total = 0;
        for (;;) {
            int n = tcp_read(c, buf, sizeof buf, 0);
            if (n <= 0) break;
            total += (uint32_t)n;
        }
        kprintf("[net] tcp sink received %u bytes (multi-segment)\n", total);
        tcp_close(c);
    }
}

void net_attach(net_device_t *dev) { g_dev = dev; g_up = 1; }
int  net_up(void)        { return g_up; }
uint32_t net_local_ip(void) { return LOCAL_IP; }

void net_init(void) {
    if (!virtio_net_init()) { kprintf("[net] no NIC; networking disabled\n"); return; }
    task_create("netd", net_worker, nullptr);
    task_create("net-tcptmr", net_tcp_ticker, nullptr);
    task_create("net-tcpd", net_tcp_server, nullptr);
    task_create("net-sinkd", net_tcp_sink, nullptr);
    task_create("net-httpd", net_http_server, nullptr);
    task_create("net-ping", net_pinger, nullptr);
    kprintf("[net] up: 10.0.2.15/24 gw 10.0.2.2\n");
}
