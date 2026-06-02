/*
 * /bin/multest — exercises the timekeeping + multiplexing syscalls:
 * clock_gettime/gettimeofday, eventfd(2), timerfd_*, poll, select, ppoll.
 * Prints one PASS/FAIL line per check; verified via the serial log.
 */

#include <stdint.h>

static long sc(long n, long a, long b, long c, long d, long e, long f) {
    long r;
    register long r10 __asm__("r10") = d;
    register long r8  __asm__("r8")  = e;
    register long r9  __asm__("r9")  = f;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return r;
}

static unsigned long slen(const char *s){ unsigned long n=0; while(s[n])n++; return n; }
static void puts1(const char *s){ sc(1,1,(long)s,(long)slen(s),0,0,0); }
static void put_ul(unsigned long v){
    char b[24]; int i=24; b[--i]=0; if(!v) b[--i]='0';
    while(v){ b[--i]='0'+(v%10); v/=10; } puts1(&b[i]);
}
static int nfail=0;
static void check(const char *name,int ok){
    puts1(ok?"  PASS ":"  FAIL "); puts1(name); puts1("\n"); if(!ok)nfail++;
}

struct pollfd { int fd; short events; short revents; };

int main(void){
    puts1("[multest] start\n");
    unsigned long val; long r;

    /* ---- timekeeping ---- */
    long ts[2];
    sc(228,0,(long)ts,0,0,0,0);                       /* clock_gettime(CLOCK_REALTIME) */
    puts1("  realtime epoch="); put_ul((unsigned long)ts[0]); puts1("\n");
    check("realtime past 2023", ts[0] > 1700000000L);
    long tv[2];
    sc(96,(long)tv,0,0,0,0,0);                        /* gettimeofday */
    check("gettimeofday past 2023", tv[0] > 1700000000L);
    long mono[2];
    sc(228,1,(long)mono,0,0,0,0);                     /* CLOCK_MONOTONIC */
    check("monotonic since boot small", mono[0] >= 0 && mono[0] < 100000L);

    /* ---- eventfd ---- */
    long efd = sc(290,5,0,0,0,0,0);                   /* eventfd2(5,0) */
    check("eventfd create", efd >= 0);
    val=0; r=sc(0,efd,(long)&val,8,0,0,0);
    check("eventfd initial read==5", r==8 && val==5);
    val=7; sc(1,efd,(long)&val,8,0,0,0);
    val=3; sc(1,efd,(long)&val,8,0,0,0);
    val=0; r=sc(0,efd,(long)&val,8,0,0,0);
    check("eventfd accumulate==10", r==8 && val==10);
    val=1; sc(1,efd,(long)&val,8,0,0,0);              /* make readable */
    struct pollfd pe={ (int)efd, 1, 0 };
    r=sc(7,(long)&pe,1,0,0,0,0);                      /* poll(timeout 0) */
    check("eventfd poll readable", r==1 && (pe.revents&1));
    val=0; sc(0,efd,(long)&val,8,0,0,0);              /* drain */

    long sfd = sc(290,2,1,0,0,0,0);                   /* eventfd2(2, EFD_SEMAPHORE) */
    val=0; r=sc(0,sfd,(long)&val,8,0,0,0);
    check("semaphore read==1", r==8 && val==1);
    val=0; r=sc(0,sfd,(long)&val,8,0,0,0);
    check("semaphore read==1 again", r==8 && val==1);
    sc(3,sfd,0,0,0,0,0); sc(3,efd,0,0,0,0,0);

    /* ---- pipe + select ---- */
    int pp[2]; sc(22,(long)pp,0,0,0,0,0);             /* pipe */
    long tvz[2]={0,0};
    unsigned long rset = 1UL<<pp[0];
    r=sc(23, pp[0]+1, (long)&rset, 0, 0, (long)tvz, 0);   /* select, timeout 0 */
    check("select empty pipe -> 0", r==0);
    char ch='x'; sc(1,pp[1],(long)&ch,1,0,0,0);       /* write 1 byte */
    rset = 1UL<<pp[0];
    r=sc(23, pp[0]+1, (long)&rset, 0, 0, (long)tvz, 0);
    check("select pipe readable", r==1 && (rset&(1UL<<pp[0])));
    sc(3,pp[0],0,0,0,0,0); sc(3,pp[1],0,0,0,0,0);

    /* ---- timerfd (one-shot 100ms) + ppoll ---- */
    long tfd = sc(283,1,0,0,0,0,0);                   /* timerfd_create(CLOCK_MONOTONIC,0) */
    check("timerfd create", tfd >= 0);
    long its[4]={0,0,0,100L*1000*1000};               /* {interval=0}, {value=100ms} */
    r=sc(286, tfd, 0, (long)its, 0, 0, 0);            /* timerfd_settime (relative) */
    check("timerfd settime", r==0);
    long gt[4]={0,0,0,0};
    sc(287, tfd, (long)gt, 0,0,0,0);                  /* timerfd_gettime */
    check("timerfd gettime armed", gt[3] > 0 || gt[2] > 0);
    struct pollfd pt={ (int)tfd, 1, 0 };
    long pts[2]={2,0};                                /* ppoll timeout 2s */
    r=sc(271,(long)&pt,1,(long)pts,0,0,0);            /* ppoll */
    check("timerfd ppoll fired", r==1 && (pt.revents&1));
    val=0; r=sc(0,tfd,(long)&val,8,0,0,0);
    check("timerfd expirations>=1", r==8 && val>=1);
    sc(3,tfd,0,0,0,0,0);

    if (nfail==0) puts1("[multest] ALL PASS\n");
    else          puts1("[multest] FAILURES\n");
    return nfail;
}
