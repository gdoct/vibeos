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
        net_rx(frame, ETH_HDR_LEN + total);
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
    /* TCP demux added in the next rung. */
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
}

void net_attach(net_device_t *dev) { g_dev = dev; g_up = 1; }
int  net_up(void)        { return g_up; }
uint32_t net_local_ip(void) { return LOCAL_IP; }

void net_init(void) {
    if (!virtio_net_init()) { kprintf("[net] no NIC; networking disabled\n"); return; }
    task_create("netd", net_worker, nullptr);
    task_create("net-ping", net_pinger, nullptr);
    kprintf("[net] up: 10.0.2.15/24 gw 10.0.2.2\n");
}
