#include "kernel.h"
#include "task.h"
#include "timer.h"
#include "net.h"

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
 * A compact state machine: 3-way handshake (active + passive open), in-order
 * data transfer with cumulative ACKs and a receive-window byte ring, and FIN
 * close. Because the only transport we exercise is loopback (and a low-loss
 * SLIRP link), there is no retransmission queue, RTT estimation, or out-of-order
 * reassembly — segments are assumed to arrive in order. A real WAN TCP would add
 * those; this is enough to prove the protocol and back the socket layer. */

typedef enum {
    T_CLOSED, T_LISTEN, T_SYN_SENT, T_SYN_RCVD, T_ESTABLISHED,
    T_FIN_WAIT_1, T_FIN_WAIT_2, T_CLOSE_WAIT, T_CLOSING, T_LAST_ACK, T_TIME_WAIT
} tcp_state_t;

#define TCP_PCB_N   32
#define TCP_RXBUF   8192
#define TCP_MSS     1460
#define TCP_ACCEPTQ 8

typedef struct tcp_pcb {
    int          used;
    tcp_state_t  state;
    uint16_t     local_port, remote_port;
    uint32_t     local_ip, remote_ip;
    uint32_t     snd_una, snd_nxt, iss;
    uint32_t     rcv_nxt;
    uint16_t     snd_wnd;
    uint8_t      rxbuf[TCP_RXBUF];
    uint32_t     rxhead, rxtail;              /* byte ring (head=producer) */
    int          peer_fin;                    /* received a FIN (EOF) */
    int          reset;                       /* connection reset */
    struct tcp_pcb *listener;                 /* parent, for SYN_RCVD children */
    struct tcp_pcb *acceptq[TCP_ACCEPTQ];     /* established, awaiting accept() */
    int          aq_head, aq_tail;
    wait_queue_t wq;                          /* any state change wakes waiters */
} tcp_pcb_t;

static tcp_pcb_t g_tcp[TCP_PCB_N];
static uint32_t  g_iss = 0x10000;

static inline int seq_lt(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
static inline int seq_gt(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }

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

static tcp_pcb_t *tcp_alloc(void) {
    for (int i = 0; i < TCP_PCB_N; i++) {
        if (g_tcp[i].used) continue;
        kmemset(&g_tcp[i], 0, sizeof g_tcp[i]);
        g_tcp[i].used = 1;
        return &g_tcp[i];
    }
    return nullptr;
}

/* Emit a segment from `t` carrying `flags` and `len` payload bytes. */
static void tcp_xmit(tcp_pcb_t *t, uint8_t flags, const uint8_t *data, uint32_t len) {
    uint8_t seg[sizeof(tcp_hdr_t) + TCP_MSS];
    if (len > TCP_MSS) len = TCP_MSS;
    tcp_hdr_t *th = (tcp_hdr_t *)seg;
    th->src_port = htons_(t->local_port);
    th->dst_port = htons_(t->remote_port);
    th->seq = htonl_(t->snd_nxt);
    th->ack = htonl_(t->rcv_nxt);
    th->data_off = (sizeof(tcp_hdr_t) / 4) << 4;
    th->flags = flags;
    uint32_t freew = tcp_rx_free(t);
    th->window = htons_((uint16_t)(freew > 0xFFFF ? 0xFFFF : freew));
    th->csum = 0; th->urg = 0;
    if (len) kmemcpy(seg + sizeof(tcp_hdr_t), data, len);
    uint32_t tot = sizeof(tcp_hdr_t) + len;
    uint32_t sip = is_loopback(t->remote_ip) ? t->remote_ip : LOCAL_IP;
    th->csum = htons_(l4_checksum(IPPROTO_TCP, sip, t->remote_ip, seg, tot));
    ip_send(t->remote_ip, IPPROTO_TCP, seg, tot);
    if (flags & (TCP_SYN | TCP_FIN)) t->snd_nxt += 1;   /* SYN/FIN take a seq */
    t->snd_nxt += len;
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
    t->iss = g_iss; g_iss += 0x10000;
    t->snd_una = t->snd_nxt = t->iss;
    t->snd_wnd = 0xFFFF;
    tcp_xmit(t, TCP_SYN, nullptr, 0);          /* SYN; snd_nxt -> iss+1 */
    sched_lock();
    while (t->used && t->state == T_SYN_SENT && !t->reset)
        wait_queue_sleep_locked(&t->wq);
    int ok = t->used && t->state == T_ESTABLISHED;
    sched_unlock();
    if (!ok) { t->used = 0; return nullptr; }
    return t;
}

static int tcp_write(tcp_pcb_t *t, const uint8_t *data, uint32_t len) {
    uint32_t sent = 0;
    while (sent < len) {
        sched_lock();
        while (t->used && !t->reset &&
               (t->state == T_ESTABLISHED || t->state == T_CLOSE_WAIT)) {
            uint32_t inflight = t->snd_nxt - t->snd_una;
            uint32_t wnd = t->snd_wnd > inflight ? t->snd_wnd - inflight : 0;
            if (wnd > 0) { sched_unlock(); goto cansend; }
            wait_queue_sleep_locked(&t->wq);     /* wait for the window to open */
        }
        sched_unlock();
        return sent ? (int)sent : -1;            /* connection no longer writable */
    cansend:;
        uint32_t inflight = t->snd_nxt - t->snd_una;
        uint32_t wnd = t->snd_wnd > inflight ? t->snd_wnd - inflight : TCP_MSS;
        uint32_t chunk = len - sent;
        if (chunk > TCP_MSS) chunk = TCP_MSS;
        if (chunk > wnd) chunk = wnd;
        tcp_xmit(t, TCP_PSH | TCP_ACK, data + sent, chunk);
        sent += chunk;
    }
    return (int)sent;
}

/* Blocking read of up to `len` bytes; returns 0 at EOF (peer FIN, buffer drained). */
static int tcp_read(tcp_pcb_t *t, uint8_t *buf, uint32_t len) {
    sched_lock();
    while (t->used && tcp_rx_used(t) == 0 && !t->peer_fin && !t->reset)
        wait_queue_sleep_locked(&t->wq);
    if (!t->used || t->reset) { sched_unlock(); return -1; }
    uint32_t avail = tcp_rx_used(t);
    uint32_t n = avail < len ? avail : len;
    for (uint32_t i = 0; i < n; i++) {
        buf[i] = t->rxbuf[t->rxtail];
        t->rxtail = (t->rxtail + 1) % TCP_RXBUF;
    }
    int eof = (n == 0 && t->peer_fin);
    sched_unlock();
    return eof ? 0 : (int)n;
}

static void tcp_close(tcp_pcb_t *t) {
    /* Decide under the lock, but transmit the FIN *after* releasing it: tcp_close
       runs in task context, and a loopback send re-enters net_rx (which takes
       sched_lock), so emitting while holding it would self-deadlock. */
    int send_fin = 0;
    sched_lock();
    if (t->used && (t->state == T_ESTABLISHED || t->state == T_CLOSE_WAIT)) {
        t->state = (t->state == T_CLOSE_WAIT) ? T_LAST_ACK : T_FIN_WAIT_1;
        send_fin = 1;
    } else {
        t->used = 0;                            /* LISTEN / already closing: drop */
    }
    sched_unlock();
    if (send_fin) tcp_xmit(t, TCP_FIN | TCP_ACK, nullptr, 0);
}

/* ---- TCP receive path (worker context) ---- */

static void tcp_deliver_data(tcp_pcb_t *t, uint32_t seq, const uint8_t *data, uint32_t plen) {
    if (plen == 0 || seq != t->rcv_nxt) return;   /* in-order only */
    uint32_t freew = tcp_rx_free(t);
    uint32_t n = plen < freew ? plen : freew;
    for (uint32_t i = 0; i < n; i++) {
        t->rxbuf[t->rxhead] = data[i];
        t->rxhead = (t->rxhead + 1) % TCP_RXBUF;
    }
    t->rcv_nxt += n;
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
                c->iss = g_iss; g_iss += 0x10000;
                c->snd_una = c->snd_nxt = c->iss;
                c->rcv_nxt = seq + 1;             /* +1 for the SYN */
                c->snd_wnd = win; c->listener = lst;
                tcp_xmit(c, TCP_SYN | TCP_ACK, nullptr, 0);
            }
        } else if (!(flags & TCP_RST)) {
            tcp_send_rst(src, dport, sport, seq + plen + ((flags & TCP_SYN) ? 1 : 0));
        }
        sched_unlock();
        return;
    }

    if (flags & TCP_RST) { t->reset = 1; t->state = T_CLOSED; wait_queue_wake_all_locked(&t->wq); sched_unlock(); return; }

    if (flags & TCP_ACK) {
        if (seq_gt(ack, t->snd_una)) t->snd_una = ack;
        t->snd_wnd = win;
    }

    switch (t->state) {
    case T_SYN_SENT:
        if ((flags & TCP_SYN) && (flags & TCP_ACK)) {
            t->rcv_nxt = seq + 1;
            t->state = T_ESTABLISHED;
            tcp_xmit(t, TCP_ACK, nullptr, 0);
        }
        break;
    case T_SYN_RCVD:
        if (flags & TCP_ACK) {
            t->state = T_ESTABLISHED;
            tcp_pcb_t *l = t->listener;           /* hand to accept() */
            if (l) {
                int next = (l->aq_head + 1) % TCP_ACCEPTQ;
                if (next != l->aq_tail) { l->acceptq[l->aq_head] = t; l->aq_head = next; }
                wait_queue_wake_all_locked(&l->wq);
            }
        }
        break;
    default: break;
    }

    if (t->state == T_ESTABLISHED || t->state == T_FIN_WAIT_1 || t->state == T_FIN_WAIT_2) {
        if (plen && seq == t->rcv_nxt) {
            tcp_deliver_data(t, seq, data, plen);
            tcp_xmit(t, TCP_ACK, nullptr, 0);
        }
    }

    /* FIN handling (only when it is the next in-order byte). */
    if ((flags & TCP_FIN) && seq + plen == t->rcv_nxt) {
        t->rcv_nxt += 1;
        t->peer_fin = 1;
        tcp_xmit(t, TCP_ACK, nullptr, 0);
        if (t->state == T_ESTABLISHED)      t->state = T_CLOSE_WAIT;
        else if (t->state == T_FIN_WAIT_1)  t->state = T_CLOSING;
        else if (t->state == T_FIN_WAIT_2)  t->state = T_CLOSED;
    }

    /* Our FIN being ACKed. */
    if ((flags & TCP_ACK) && t->snd_una == t->snd_nxt) {
        if (t->state == T_FIN_WAIT_1)     t->state = t->peer_fin ? T_CLOSED : T_FIN_WAIT_2;
        else if (t->state == T_CLOSING)   t->state = T_CLOSED;
        else if (t->state == T_LAST_ACK)  { t->state = T_CLOSED; t->used = 0; }
    }

    wait_queue_wake_all_locked(&t->wq);
    sched_unlock();
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

int ksock_sendto(void *vp, const void *buf, uint32_t len, uint32_t ip, uint16_t port) {
    ksocket_t *s = (ksocket_t *)vp;
    if (s->type == SOCK_STREAM) return s->tcp ? tcp_write(s->tcp, (const uint8_t *)buf, len) : -1;
    if (!s->udp) s->udp = udp_bind(0);
    return udp_sendto(s->udp, ip, port, buf, len) < 0 ? -1 : (int)len;
}

int ksock_send(void *vp, const void *buf, uint32_t len) {
    ksocket_t *s = (ksocket_t *)vp;
    return ksock_sendto(vp, buf, len, s->peer_ip, s->peer_port);
}

int ksock_recvfrom(void *vp, void *buf, uint32_t len, uint32_t *ip, uint16_t *port) {
    ksocket_t *s = (ksocket_t *)vp;
    if (s->type == SOCK_STREAM) return s->tcp ? tcp_read(s->tcp, (uint8_t *)buf, len) : -1;
    return udp_recvfrom(s->udp, buf, len, ip, port);
}

int ksock_recv(void *vp, void *buf, uint32_t len) { return ksock_recvfrom(vp, buf, len, nullptr, nullptr); }

int ksock_poll(void *vp, int want) {
    ksocket_t *s = (ksocket_t *)vp;
    int r = 0;
    sched_lock();
    if (s->type == SOCK_STREAM && s->tcp) {
        tcp_pcb_t *t = s->tcp;
        if ((want & NET_POLLIN) &&
            (tcp_rx_used(t) > 0 || t->peer_fin || t->aq_head != t->aq_tail || t->reset))
            r |= NET_POLLIN;
        if ((want & NET_POLLOUT) && t->state == T_ESTABLISHED) r |= NET_POLLOUT;
    } else if (s->udp) {
        if ((want & NET_POLLIN) && s->udp->qhead != s->udp->qtail) r |= NET_POLLIN;
        if (want & NET_POLLOUT) r |= NET_POLLOUT;
    }
    sched_unlock();
    return r;
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
        while (g_rxq_head == g_rxq_tail)
            wait_queue_sleep_locked(&g_rxq_wq);
        rxslot_t *s = &g_rxq[g_rxq_tail];
        static uint8_t local[ETH_FRAME_MAX];
        uint16_t n = s->len;
        kmemcpy(local, s->data, n);
        g_rxq_tail = (g_rxq_tail + 1) % RXQ_N;
        sched_unlock();
        net_input(local, n);

        /* Drain loopback frames this frame generated (and any they generate). */
        while (g_loq_head != g_loq_tail) {
            static uint8_t lb[ETH_FRAME_MAX];
            uint16_t ln = g_loq[g_loq_tail].len;
            kmemcpy(lb, g_loq[g_loq_tail].data, ln);
            g_loq_tail = (g_loq_tail + 1) % LOQ_N;
            net_input(lb, ln);
        }
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
        tcp_write(c, (const uint8_t *)"hello-tcp", 9);
        uint8_t buf[256];
        int n = tcp_read(c, buf, sizeof buf);
        buf[n > 0 ? n : 0] = '\0';
        kprintf("[net] tcp client connected, got %d bytes: \"%s\"\n", n, buf);
        tcp_close(c);
    } else {
        kprintf("[net] tcp connect to :8080 failed\n");
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
        int n = tcp_read(c, req, sizeof req);
        if (n > 0) {
            uint8_t rep[280];
            const char *pfx = "echo: ";
            int k = 0; while (pfx[k]) { rep[k] = (uint8_t)pfx[k]; k++; }
            kmemcpy(rep + k, req, n);
            tcp_write(c, rep, k + n);
        }
        uint8_t drain[64];
        while (tcp_read(c, drain, sizeof drain) > 0) { }   /* read to EOF */
        tcp_close(c);
    }
}

void net_attach(net_device_t *dev) { g_dev = dev; g_up = 1; }
int  net_up(void)        { return g_up; }
uint32_t net_local_ip(void) { return LOCAL_IP; }

void net_init(void) {
    if (!virtio_net_init()) { kprintf("[net] no NIC; networking disabled\n"); return; }
    task_create("netd", net_worker, nullptr);
    task_create("net-tcpd", net_tcp_server, nullptr);
    task_create("net-ping", net_pinger, nullptr);
    kprintf("[net] up: 10.0.2.15/24 gw 10.0.2.2\n");
}
