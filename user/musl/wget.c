/* wget — a compact HTTP/1.0 client for VibeOS (ROADMAP §5 "payoff").
 *
 * A real curl/wget (libcurl, TLS, zlib, distro shared objects) isn't portable to
 * this kernel; this is the equivalent: a standalone HTTP client built on the
 * BSD socket syscalls. Parses http://host[:port]/path, resolves the host
 * (localhost / dotted-quad directly, otherwise a DNS A-query to 10.0.2.3),
 * connects, issues a GET, and writes the response body to stdout or -O <file>.
 *
 * Verified over loopback against the in-guest HTTP server (http://localhost/).
 * Against a real server it would work the same if the host had outbound
 * connectivity (not available in the build sandbox).
 *
 * Usage: wget [-O outfile] http://host[:port]/path
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static unsigned parse_ipv4(const char *s) {     /* dotted quad -> host order, 0 if not */
    unsigned a, b, c, d; int n = 0;
    const char *p = s;
    unsigned *parts[4] = { &a, &b, &c, &d };
    for (int i = 0; i < 4; i++) {
        unsigned v = 0, got = 0;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (*p++ - '0'); got = 1; }
        if (!got || v > 255) return 0;
        *parts[i] = v;
        if (i < 3) { if (*p != '.') return 0; p++; }
    }
    if (*p != '\0') return 0;
    (void)n;
    return (a << 24) | (b << 16) | (c << 8) | d;
}

/* Minimal DNS A-record lookup via 10.0.2.3:53. Returns host-order IPv4 or 0. */
static unsigned dns_resolve(const char *host) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return 0;
    unsigned char q[512]; int n = 0;
    q[n++] = 0x12; q[n++] = 0x34;            /* id */
    q[n++] = 0x01; q[n++] = 0x00;            /* recursion desired */
    q[n++] = 0; q[n++] = 1;                  /* qdcount = 1 */
    q[n++] = 0; q[n++] = 0; q[n++] = 0; q[n++] = 0; q[n++] = 0; q[n++] = 0;
    const char *p = host;
    while (*p) {                              /* labels */
        const char *dot = strchr(p, '.');
        int len = dot ? (int)(dot - p) : (int)strlen(p);
        q[n++] = (unsigned char)len;
        memcpy(q + n, p, len); n += len;
        p += len; if (*p == '.') p++;
    }
    q[n++] = 0;                               /* root */
    q[n++] = 0; q[n++] = 1;                   /* type A */
    q[n++] = 0; q[n++] = 1;                   /* class IN */

    struct sockaddr_in ns;
    memset(&ns, 0, sizeof ns);
    ns.sin_family = AF_INET; ns.sin_port = htons(53);
    ns.sin_addr.s_addr = htonl(0x0A000203);   /* 10.0.2.3 */
    sendto(s, q, n, 0, (struct sockaddr *)&ns, sizeof ns);

    unsigned char r[512];
    int rn = recvfrom(s, r, sizeof r, 0, 0, 0);
    close(s);
    if (rn < 12) return 0;
    int qd = (r[4] << 8) | r[5], an = (r[6] << 8) | r[7];
    int off = 12;
    for (int i = 0; i < qd; i++) {            /* skip questions */
        while (off < rn && r[off]) off += r[off] + 1;
        off += 1 + 4;
    }
    for (int i = 0; i < an && off + 12 <= rn; i++) {
        if ((r[off] & 0xC0) == 0xC0) off += 2; else { while (off < rn && r[off]) off += r[off] + 1; off++; }
        int type = (r[off] << 8) | r[off + 1];
        int rdlen = (r[off + 8] << 8) | r[off + 9];
        off += 10;
        if (type == 1 && rdlen == 4)
            return (r[off] << 24) | (r[off+1] << 16) | (r[off+2] << 8) | r[off+3];
        off += rdlen;
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *outfile = NULL, *url = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-O") && i + 1 < argc) outfile = argv[++i];
        else url = argv[i];
    }
    if (!url) { fprintf(stderr, "usage: wget [-O file] http://host[:port]/path\n"); return 2; }

    if (strncmp(url, "http://", 7) != 0) { fprintf(stderr, "wget: only http:// URLs\n"); return 2; }
    char host[128]; int port = 80; const char *path = "/";
    const char *h = url + 7;
    int hi = 0;
    while (*h && *h != '/' && *h != ':' && hi < (int)sizeof(host) - 1) host[hi++] = *h++;
    host[hi] = '\0';
    if (*h == ':') { port = atoi(h + 1); while (*h && *h != '/') h++; }
    if (*h == '/') path = h;

    unsigned ip = parse_ipv4(host);
    if (!ip && !strcmp(host, "localhost")) ip = 0x7F000001;     /* 127.0.0.1 */
    if (!ip) ip = dns_resolve(host);
    if (!ip) { fprintf(stderr, "wget: cannot resolve %s\n", host); return 1; }

    fprintf(stderr, "wget: %s -> %u.%u.%u.%u:%d%s\n", host,
            (ip>>24)&0xff, (ip>>16)&0xff, (ip>>8)&0xff, ip&0xff, port, path);

    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(ip);
    if (connect(s, (struct sockaddr *)&a, sizeof a) != 0) {
        fprintf(stderr, "wget: connect failed\n"); close(s); return 1;
    }

    char req[256];
    int rl = snprintf(req, sizeof req,
                      "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: vibeos-wget\r\nConnection: close\r\n\r\n",
                      path, host);
    write(s, req, rl);

    /* Read the whole response. */
    char buf[8192]; int total = 0, n;
    while (total < (int)sizeof(buf) - 1 && (n = read(s, buf + total, sizeof(buf) - 1 - total)) > 0)
        total += n;
    close(s);
    buf[total] = '\0';

    char *body = strstr(buf, "\r\n\r\n");
    body = body ? body + 4 : buf;
    int blen = total - (int)(body - buf);
    fprintf(stderr, "wget: %d header+body bytes, %d body bytes\n", total, blen);

    int out = 1;
    if (outfile) {
        out = open(outfile, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (out < 0) { fprintf(stderr, "wget: cannot open %s\n", outfile); return 1; }
    }
    write(out, body, blen);
    if (outfile) { close(out); fprintf(stderr, "wget: saved to %s\n", outfile); }
    return 0;
}
