/* GUI phase-2 probe: validate the kernel primitives the userspace WM relies on.
 * Opens /dev/fb0, reads its geometry, mmaps the framebuffer, paints a pattern
 * straight from userspace (a screendump should show it), then grabs /dev/input
 * and prints the mouse/keyboard events the kernel delivers. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>

struct fbinfo { uint32_t width, height, pitch, bpp; uint64_t size; };
struct iev    { uint8_t type, buttons, code, pressed; int16_t x, y; };

static void msleep(int ms){ struct timespec t={ms/1000,(long)(ms%1000)*1000000L}; nanosleep(&t,0); }

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int fb = open("/dev/fb0", O_RDWR);
    if (fb < 0) { printf("guiprobe: open /dev/fb0 failed\n"); return 1; }
    struct fbinfo fi;
    if (read(fb, &fi, sizeof fi) != (int)sizeof fi) { printf("guiprobe: fbinfo read failed\n"); return 1; }
    printf("guiprobe: fb %ux%u pitch=%u bpp=%u size=%lu\n",
           fi.width, fi.height, fi.pitch, fi.bpp, (unsigned long)fi.size);

    uint32_t *px = mmap(0, fi.size, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
    if (px == (void *)-1) { printf("guiprobe: mmap /dev/fb0 failed\n"); return 1; }
    int stride = fi.pitch / 4;
    /* paint: dark-blue field + a red square top-left + a green diagonal */
    for (uint32_t y = 0; y < fi.height; y++)
        for (uint32_t x = 0; x < fi.width; x++)
            px[y * stride + x] = 0x00102844;
    for (uint32_t y = 0; y < 200 && y < fi.height; y++)
        for (uint32_t x = 0; x < 200 && x < fi.width; x++)
            px[y * stride + x] = 0x00ff3020;
    for (uint32_t d = 0; d < fi.height && d < fi.width; d++)
        px[d * stride + d] = 0x0030ff60;
    printf("guiprobe: painted framebuffer from userspace (mmap ok)\n");

    int in = open("/dev/input", O_RDONLY);
    if (in < 0) { printf("guiprobe: open /dev/input failed\n"); return 1; }
    printf("guiprobe: reading /dev/input for ~8s\n");
    int mouse_evs = 0, key_evs = 0;
    for (int i = 0; i < 80; i++) {
        struct iev ev[16];
        int n = read(in, ev, sizeof ev);
        for (int k = 0; k + 1 <= n / (int)sizeof(struct iev); k++) {
            if (ev[k].type == 1) {
                if (mouse_evs++ < 4)
                    printf("  mouse x=%d y=%d btn=%u\n", ev[k].x, ev[k].y, ev[k].buttons);
            } else if (ev[k].type == 2) {
                if (key_evs++ < 8) printf("  key '%c'\n", ev[k].code ? ev[k].code : '?');
            }
        }
        msleep(100);
    }
    printf("guiprobe: done (%d mouse, %d key events)\n", mouse_evs, key_evs);
    return 0;
}
