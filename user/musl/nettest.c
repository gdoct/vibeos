/* §5 socket test (static musl): exercises the BSD socket syscalls over the
 * loopback path. A forked child runs a TCP echo server (socket/bind/listen/
 * accept/read/write); the parent connects and round-trips a message. Then a UDP
 * datagram is sent between two sockets in-process (sendto/recvfrom). */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>

static void msleep(int ms) { struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&ts, 0); }

static struct sockaddr_in lo(int port) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* 127.0.0.1 */
    return a;
}

static int tcp_test(void) {
    pid_t pid = fork();
    if (pid == 0) {                               /* child: echo server */
        int s = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a = lo(8090);
        bind(s, (struct sockaddr *)&a, sizeof a);
        listen(s, 1);
        int c = accept(s, 0, 0);
        char b[128];
        int n = read(c, b, sizeof b);
        if (n > 0) write(c, b, n);                /* echo */
        close(c); close(s);
        _exit(0);
    }
    msleep(250);                                  /* let the server listen */
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = lo(8090);
    if (connect(s, (struct sockaddr *)&a, sizeof a) != 0) {
        printf("tcp: connect failed\n"); close(s); return 1;
    }
    const char *msg = "socket-tcp-hello";
    write(s, msg, strlen(msg));
    char buf[128];
    int n = read(s, buf, sizeof buf);
    if (n < 0) n = 0;
    buf[n] = '\0';
    printf("tcp: sent \"%s\", got %d bytes back: \"%s\"\n", msg, n, buf);
    close(s);
    int st = 0; waitpid(pid, &st, 0);
    return 0;
}

static int udp_test(void) {
    int srv = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = lo(7001);
    bind(srv, (struct sockaddr *)&a, sizeof a);

    int cli = socket(AF_INET, SOCK_DGRAM, 0);
    const char *msg = "socket-udp-hello";
    sendto(cli, msg, strlen(msg), 0, (struct sockaddr *)&a, sizeof a);

    char buf[128];
    struct sockaddr_in from; socklen_t fl = sizeof from;
    int n = recvfrom(srv, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
    if (n < 0) n = 0;
    buf[n] = '\0';
    printf("udp: got %d bytes from port %d: \"%s\"\n", n, ntohs(from.sin_port), buf);
    close(srv); close(cli);
    return 0;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("nettest: BSD sockets over loopback\n");
    tcp_test();
    udp_test();
    printf("nettest done\n");
    return 0;
}
