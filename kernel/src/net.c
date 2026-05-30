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
static int ip_send(uint32_t dst, uint8_t proto, const void *payload, uint32_t plen) {
    uint32_t nexthop = ((dst & NETMASK) == (LOCAL_IP & NETMASK)) ? dst : GATEWAY_IP;
    uint8_t mac[6];
    if (!arp_resolve(nexthop, mac)) return -1;

    uint8_t pkt[ETH_FRAME_MAX - ETH_HDR_LEN];
    ip_hdr_t *ip = (ip_hdr_t *)pkt;
    uint32_t total = sizeof(ip_hdr_t) + plen;
    if (total > sizeof pkt) return -1;
    ip->ver_ihl = 0x45; ip->tos = 0;
    ip->total_len = htons_((uint16_t)total);
    ip->id = htons_(0); ip->frag = htons_(0x4000);    /* don't fragment */
    ip->ttl = 64; ip->proto = proto; ip->csum = 0;
    ip->src = htonl_(LOCAL_IP); ip->dst = htonl_(dst);
    ip->csum = htons_(ip_checksum(ip, sizeof(ip_hdr_t)));
    kmemcpy(pkt + sizeof(ip_hdr_t), payload, plen);
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

static void ip_input(const uint8_t *p, uint32_t len) {
    if (len < sizeof(ip_hdr_t)) return;
    const ip_hdr_t *ip = (const ip_hdr_t *)p;
    if ((ip->ver_ihl >> 4) != 4) return;
    uint32_t ihl = (ip->ver_ihl & 0xF) * 4;
    if (ihl < sizeof(ip_hdr_t) || ihl > len) return;
    uint32_t dst = ntohl_(ip->dst);
    if (dst != LOCAL_IP && dst != 0xFFFFFFFFu) return;   /* not for us */
    uint32_t src = ntohl_(ip->src);
    uint16_t total = ntohs_(ip->total_len);
    if (total > len) total = (uint16_t)len;
    const uint8_t *payload = p + ihl;
    uint32_t plen = total - ihl;
    if (ip->proto == IPPROTO_ICMP) icmp_input(src, payload, plen);
    /* UDP/TCP demux added in later rungs. */
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
