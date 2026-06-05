#include "doomkeys.h"
#include "doomgeneric.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "gui_client.h"

#define KEYQUEUE_SIZE 64

static gui_conn_t *s_Conn;
static gfx_surface_t s_Surface;
static unsigned short s_KeyQueue[KEYQUEUE_SIZE];
static unsigned int s_KeyRead;
static unsigned int s_KeyWrite;
static int s_ShouldQuit;

static unsigned char convert_to_doom_key(uint32_t key, uint32_t mods)
{
    unsigned char ascii = (unsigned char)tolower((int)(key & 0xff));

    if (mods & GIN_MOD_CTRL) return KEY_FIRE;

    switch (key) {
    case '\r':
    case '\n': return KEY_ENTER;
    case 27: return KEY_ESCAPE;
    case '\t': return KEY_TAB;
    case 8:
    case 127: return KEY_BACKSPACE;
    case ' ': return KEY_USE;
    case ',': return KEY_STRAFE_L;
    case '.': return KEY_STRAFE_R;
    case '-': return KEY_MINUS;
    case '=': return KEY_EQUALS;
    default: break;
    }

    switch (ascii) {
    case 'w': return KEY_UPARROW;
    case 's': return KEY_DOWNARROW;
    case 'a': return KEY_LEFTARROW;
    case 'd': return KEY_RIGHTARROW;
    case 'z': return KEY_STRAFE_L;
    case 'c': return KEY_STRAFE_R;
    case 'f': return KEY_FIRE;
    case 'r': return KEY_RSHIFT;
    default: return ascii;
    }
}

static void queue_key(int pressed, uint32_t key, uint32_t mods)
{
    unsigned char doom_key = convert_to_doom_key(key, mods);
    unsigned int next;

    if (doom_key == 0) return;
    next = (s_KeyWrite + 1) % KEYQUEUE_SIZE;
    if (next == s_KeyRead) return;

    s_KeyQueue[s_KeyWrite] = (unsigned short)((pressed ? 0x100 : 0) | doom_key);
    s_KeyWrite = next;
}

static void reset_surface(int w, int h)
{
    gfx_free(&s_Surface);
    s_Surface = gfx_alloc(w, h);
    if (!s_Surface.px) {
        fprintf(stderr, "doom: surface allocation failed\n");
        exit(1);
    }
}

static void pump_events(void)
{
    gevt_input_t ev;
    int r;

    if (!s_Conn) return;

    while ((r = gc_poll(s_Conn, &ev)) > 0) {
        switch (ev.ev) {
        case GE_KEY:
            queue_key(1, ev.key, ev.buttons);
            break;
        case GE_RESIZE:
            reset_surface(s_Conn->w, s_Conn->h);
            break;
        default:
            break;
        }
    }

    if (r < 0) s_ShouldQuit = 1;
}

void DG_Init(void)
{
    memset(s_KeyQueue, 0, sizeof s_KeyQueue);
    memset(&s_Surface, 0, sizeof s_Surface);

    s_Conn = gc_open(DOOMGENERIC_RESX, DOOMGENERIC_RESY, "DOOM");
    if (!s_Conn) {
        fprintf(stderr, "doom: cannot reach the window manager\n");
        exit(1);
    }

    reset_surface(s_Conn->w, s_Conn->h);
}

void DG_DrawFrame(void)
{
    int dx;
    int dy;

    pump_events();
    if (s_ShouldQuit || !s_Surface.px) return;

    gfx_clear(&s_Surface, GFX_RGB(0, 0, 0));
    dx = (s_Surface.w - DOOMGENERIC_RESX) / 2;
    dy = (s_Surface.h - DOOMGENERIC_RESY) / 2;
    gfx_blit(&s_Surface, DG_ScreenBuffer, DOOMGENERIC_RESX, DOOMGENERIC_RESY, dx, dy);
    gc_commit(s_Conn, &s_Surface);
}

void DG_SleepMs(uint32_t ms)
{
    usleep(ms * 1000);
}

uint32_t DG_GetTicksMs(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

int DG_GetKey(int *pressed, unsigned char *doomKey)
{
    unsigned short key_data;

    pump_events();
    if (s_KeyRead == s_KeyWrite) return 0;

    key_data = s_KeyQueue[s_KeyRead];
    s_KeyRead = (s_KeyRead + 1) % KEYQUEUE_SIZE;
    *pressed = (key_data >> 8) & 1;
    *doomKey = key_data & 0xff;
    return 1;
}

void DG_SetWindowTitle(const char *title)
{
    (void)title;
}

int main(int argc, char **argv)
{
    doomgeneric_Create(argc, argv);

    while (!s_ShouldQuit) {
        doomgeneric_Tick();
    }

    gfx_free(&s_Surface);
    gc_close(s_Conn);
    return 0;
}