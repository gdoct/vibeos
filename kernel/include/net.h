#ifndef VIBEOS_NET_H
#define VIBEOS_NET_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Networking (ROADMAP §5): a compact, purpose-built IPv4 stack over virtio-net.
 *
 * The driver (virtio_net.c) hands received Ethernet frames to net_rx() from IRQ
 * context, which copies them into a queue and wakes a kernel worker; the worker
 * runs the protocol stack (ARP / IPv4 / ICMP / UDP / TCP) in task context where
 * it may block and send replies. The stack transmits through the driver's tx
 * callback registered with net_attach().
 *
 * Addressing is the static QEMU SLIRP layout (no DHCP): 10.0.2.15/24, gateway
 * 10.0.2.2, DNS 10.0.2.3.
 */

#define ETH_ALEN     6
#define ETH_HDR_LEN  14
#define ETH_FRAME_MAX 1514     /* without FCS */

#define ETHERTYPE_IP   0x0800
#define ETHERTYPE_ARP  0x0806

/* Host byte order helpers (x86 is little-endian; network is big-endian). */
static inline uint16_t htons_(uint16_t x) { return (uint16_t)((x << 8) | (x >> 8)); }
static inline uint16_t ntohs_(uint16_t x) { return htons_(x); }
static inline uint32_t htonl_(uint32_t x) {
    return ((x & 0xFF) << 24) | ((x & 0xFF00) << 8) |
           ((x >> 8) & 0xFF00) | ((x >> 24) & 0xFF);
}
static inline uint32_t ntohl_(uint32_t x) { return htonl_(x); }

#define IPADDR(a,b,c,d) (((uint32_t)(a)<<24)|((uint32_t)(b)<<16)|((uint32_t)(c)<<8)|(uint32_t)(d))

/* A network device the stack transmits through (registered by the driver). */
typedef struct net_device {
    uint8_t mac[ETH_ALEN];
    int   (*tx)(const void *frame, uint32_t len);   /* send a raw Ethernet frame */
    const char *name;
} net_device_t;

/* Driver -> stack. */
void net_attach(net_device_t *dev);                  /* register the NIC */
void net_rx(const uint8_t *frame, uint32_t len);     /* hand up an rx frame (IRQ ctx) */

/* Bring up the network: probe virtio-net, start the worker, configure the
   static address. No-op if there is no NIC. */
void net_init(void);

/* True once a NIC is attached and configured. */
int  net_up(void);

/* This host's configured IPv4 address (host byte order). */
uint32_t net_local_ip(void);

/* Probe + initialise the virtio-net PCI device. Returns 1 if a NIC was found. */
int virtio_net_init(void);

/* ---- kernel socket API (wrapped by the socket syscalls; ROADMAP §5) ----
 * Addresses are host byte order here; the syscall layer converts to/from the
 * network-order sockaddr_in. A socket is an opaque handle stored in the fd
 * table (file kind FD_SOCKET). */
#define AF_INET      2
#define SOCK_STREAM  1
#define SOCK_DGRAM   2
#define NET_POLLIN   0x1
#define NET_POLLOUT  0x4

void *ksock_create(int type);                     /* SOCK_STREAM/DGRAM; NULL on fail */
int   ksock_bind(void *s, uint32_t ip, uint16_t port);
int   ksock_listen(void *s, int backlog);
void *ksock_accept(void *s, uint32_t *peer_ip, uint16_t *peer_port);
int   ksock_connect(void *s, uint32_t ip, uint16_t port);
int   ksock_send(void *s, const void *buf, uint32_t len);             /* connected */
int   ksock_sendto(void *s, const void *buf, uint32_t len, uint32_t ip, uint16_t port);
int   ksock_recv(void *s, void *buf, uint32_t len);                  /* connected */
int   ksock_recvfrom(void *s, void *buf, uint32_t len, uint32_t *ip, uint16_t *port);
int   ksock_poll(void *s, int want);              /* returns ready NET_POLL* bits */
int   ksock_getname(void *s, int peer, uint32_t *ip, uint16_t *port);  /* local/peer addr */
void  ksock_close(void *s);

#ifdef __cplusplus
}
#endif

#endif
