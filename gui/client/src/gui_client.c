/* gui_client implementation — see gui_client.h. */
#include "gui_client.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int read_full(int fd, void *buf, int n) {
    uint8_t *p = (uint8_t *)buf;
    int got = 0;
    while (got < n) {
        int r = read(fd, p + got, n - got);
        if (r <= 0) return -1;
        got += r;
    }
    return 0;
}

static int write_full(int fd, const void *buf, int n) {
    const uint8_t *p = (const uint8_t *)buf;
    int put = 0;
    while (put < n) {
        int r = write(fd, p + put, n - put);
        if (r <= 0) return -1;
        put += r;
    }
    return 0;
}

gui_conn_t *gc_open(int w, int h, const char *title) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(GUI_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) { close(fd); return 0; }

    gmsg_hdr_t hdr = { GMSG_CREATE, sizeof(gmsg_create_t) };
    gmsg_create_t cr;
    memset(&cr, 0, sizeof cr);
    cr.w = w; cr.h = h;
    strncpy(cr.title, title ? title : "", sizeof cr.title - 1);
    if (write_full(fd, &hdr, sizeof hdr) || write_full(fd, &cr, sizeof cr)) { close(fd); return 0; }

    /* await GEVT_CREATED */
    gmsg_hdr_t rh;
    if (read_full(fd, &rh, sizeof rh) || rh.type != GEVT_CREATED) { close(fd); return 0; }
    gevt_created_t cd;
    if (read_full(fd, &cd, sizeof cd)) { close(fd); return 0; }

    gui_conn_t *c = (gui_conn_t *)malloc(sizeof *c);
    c->fd = fd; c->wid = cd.wid; c->w = cd.w; c->h = cd.h;
    return c;
}

int gc_commit(gui_conn_t *c, gfx_surface_t *s) {
    gmsg_hdr_t hdr = { GMSG_FRAME, (uint32_t)(sizeof(gmsg_frame_t) + (size_t)s->w * s->h * 4) };
    gmsg_frame_t fr = { 0, 0, (uint32_t)s->w, (uint32_t)s->h };
    if (write_full(c->fd, &hdr, sizeof hdr)) return -1;
    if (write_full(c->fd, &fr, sizeof fr)) return -1;
    /* surface may be strided; send row by row */
    for (int y = 0; y < s->h; y++)
        if (write_full(c->fd, s->px + (size_t)y * s->stride, s->w * 4)) return -1;
    return 0;
}

int gc_poll(gui_conn_t *c, gevt_input_t *ev) {
    /* peek whether a full header is available without blocking */
    int fl = fcntl(c->fd, F_GETFL, 0);
    fcntl(c->fd, F_SETFL, fl | O_NONBLOCK);
    gmsg_hdr_t hdr;
    int r = read(c->fd, &hdr, sizeof hdr);
    fcntl(c->fd, F_SETFL, fl);
    if (r == 0) return -1;                 /* closed */
    if (r < 0) return 0;                   /* would block: no event */
    if (r < (int)sizeof hdr) {             /* partial header: finish it blocking */
        if (read_full(c->fd, (uint8_t *)&hdr + r, sizeof hdr - r)) return -1;
    }
    if (hdr.type == GEVT_CLOSE) return -1;
    if (hdr.type == GEVT_INPUT) {
        if (read_full(c->fd, ev, sizeof *ev)) return -1;
        return 1;
    }
    /* unknown body: drain it */
    uint8_t scratch[256];
    uint32_t left = hdr.len;
    while (left) { uint32_t n = left < sizeof scratch ? left : sizeof scratch;
                   if (read_full(c->fd, scratch, n)) return -1; left -= n; }
    return 0;
}

void gc_close(gui_conn_t *c) {
    if (!c) return;
    gmsg_hdr_t hdr = { GMSG_CLOSE, 0 };
    write_full(c->fd, &hdr, sizeof hdr);
    close(c->fd);
    free(c);
}
