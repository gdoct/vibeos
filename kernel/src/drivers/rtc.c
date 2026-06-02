/*
 * CMOS RTC reader (ROADMAP: real timekeeping).
 *
 * Reads the MC146818 RTC via the CMOS index/data ports (0x70/0x71) once at boot,
 * converts the broken-down time to a Unix epoch second, and anchors it to the
 * timer tick count. gettimeofday/clock_gettime then return epoch + elapsed ticks
 * without touching the slow CMOS again. UTC is assumed (QEMU's default).
 */

#include "rtc.h"
#include "io.h"
#include "timer.h"
#include "kernel.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static uint64_t g_boot_epoch = 0;   /* Unix seconds sampled at boot */
static uint64_t g_boot_tick  = 0;   /* timer_ticks() at that sample  */

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static int update_in_progress(void) {
    outb(CMOS_ADDR, 0x0A);
    return inb(CMOS_DATA) & 0x80;
}

static uint8_t bcd_to_bin(uint8_t v) { return (uint8_t)((v & 0x0F) + (v >> 4) * 10); }

/* Days from 1970-01-01 to the start of (year, month, day), Gregorian. */
static uint64_t days_from_civil(int y, int m, int d) {
    /* Howard Hinnant's algorithm: days since 1970-01-01. */
    y -= (m <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (uint64_t)(era * 146097 + (int)doe - 719468);
}

void rtc_init(void) {
    /* Read twice (until two consecutive reads agree and no update is in flight)
       so we never latch a half-updated time. */
    uint8_t sec, min, hour, day, mon, year, century, regb;
    uint8_t ls = 0xFF, lm = 0xFF, lh = 0xFF, ld = 0xFF, lmo = 0xFF, ly = 0xFF, lc = 0xFF;
    for (int tries = 0; tries < 100; tries++) {
        while (update_in_progress()) { /* spin briefly */ }
        sec = cmos_read(0x00); min = cmos_read(0x02); hour = cmos_read(0x04);
        day = cmos_read(0x07); mon = cmos_read(0x08); year = cmos_read(0x09);
        century = cmos_read(0x32);
        if (sec == ls && min == lm && hour == lh && day == ld &&
            mon == lmo && year == ly && century == lc) break;
        ls = sec; lm = min; lh = hour; ld = day; lmo = mon; ly = year; lc = century;
    }

    regb = cmos_read(0x0B);
    int bcd = !(regb & 0x04);                 /* bit2 set => binary, clear => BCD */
    int h12 = !(regb & 0x02);                 /* bit1 clear => 12-hour mode */

    int pm = 0;
    if (h12) { pm = hour & 0x80; hour &= 0x7F; }

    if (bcd) {
        sec = bcd_to_bin(sec); min = bcd_to_bin(min); hour = bcd_to_bin(hour);
        day = bcd_to_bin(day); mon = bcd_to_bin(mon); year = bcd_to_bin(year);
        century = bcd_to_bin(century);
    }

    int full_year;
    if (century >= 19 && century <= 21) full_year = century * 100 + year;
    else                                full_year = 2000 + year;   /* no century reg */

    if (h12) { hour = (uint8_t)(hour % 12); if (pm) hour = (uint8_t)(hour + 12); }

    uint64_t days = days_from_civil(full_year, mon, day);
    g_boot_epoch = days * 86400ULL + (uint64_t)hour * 3600 + (uint64_t)min * 60 + sec;
    g_boot_tick  = timer_ticks();

    kprintf("[rtc] %d-%02u-%02u %02u:%02u:%02u UTC (epoch %lu)\n",
            full_year, mon, day, hour, min, sec, (unsigned long)g_boot_epoch);
}

uint64_t rtc_boot_epoch(void) { return g_boot_epoch; }

void rtc_realtime(uint64_t *sec, uint64_t *nsec) {
    uint32_t hz = timer_hz();
    if (!hz) hz = 100;
    uint64_t elapsed = timer_ticks() - g_boot_tick;
    if (sec)  *sec  = g_boot_epoch + elapsed / hz;
    if (nsec) *nsec = (elapsed % hz) * (1000000000ULL / hz);
}
