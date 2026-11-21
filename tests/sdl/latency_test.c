/* The platform's input latency and sound queue, measured under SDL's dummy video and disk sound
 * (which takes the sound at its rate, as a sound card does, and so paces the frames): a thread
 * presses Start (Return) at random moments, 200 times, and the game notes when its frame first
 * sees it; the time from the key's event to that frame, and the frames' spacing. Built for the
 * SDL2 platform (tests/sdl) and, with MDRT_SDL3 defined, the SDL3 one (tests/sdl3 includes this).
 *   latency_test DIR   (DIR: where it runs; it writes settings.ini and sound.raw there) */
#ifdef MDRT_SDL3
#include <SDL3/SDL.h>
#define ATOMIC SDL_AtomicInt
#define AGET SDL_GetAtomicInt
#define ASET SDL_SetAtomicInt
#define KEY_DOWN SDL_EVENT_KEY_DOWN
#define KEY_UP SDL_EVENT_KEY_UP
#else
#include <SDL.h>
#define ATOMIC SDL_atomic_t
#define AGET SDL_AtomicGet
#define ASET SDL_AtomicSet
#define KEY_DOWN SDL_KEYDOWN
#define KEY_UP SDL_KEYUP
#endif
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "check.h"
#include "mdrt/mdrt.h"
#include "mdrt/platform_sdl2.h"

#define PRESSES 200
static ATOMIC frames, seen, ended;
static Uint64 pushed[PRESSES], noticed[PRESSES], frame_at[20000];
static int frame_n;

static void key(int down)
{
    SDL_Event e;
    SDL_zero(e);
    e.type = down ? KEY_DOWN : KEY_UP;
#ifdef MDRT_SDL3
    e.key.down = down != 0;
    e.key.scancode = SDL_SCANCODE_RETURN;
    e.key.key = SDL_GetKeyFromScancode(SDL_SCANCODE_RETURN, SDL_KMOD_NONE, false);
#else
    e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.key.keysym.scancode = SDL_SCANCODE_RETURN;
    e.key.keysym.sym = SDL_GetKeyFromScancode(SDL_SCANCODE_RETURN);
#endif
    SDL_PushEvent(&e);
}

static int presser(void *user)
{
    (void)user;
    while (AGET(&frames) < 60) SDL_Delay(5);
    srand(7);
    for (int i = 0; i < PRESSES; i++) {
        SDL_Delay(40 + (Uint32)(rand() % 60));            /* (at any point of a frame) */
        pushed[i] = SDL_GetPerformanceCounter();
        key(1);
        for (int t = 0; t < 500 && AGET(&seen) <= i; t++) SDL_Delay(1);
        key(0);
        SDL_Delay(40);
    }
    ASET(&ended, 1);
    SDL_Event q;
    SDL_zero(q);
#ifdef MDRT_SDL3
    q.type = SDL_EVENT_QUIT;
#else
    q.type = SDL_QUIT;
#endif
    SDL_PushEvent(&q);
    return 0;
}

static void game(mdrt *m, void *user)
{
    (void)user;
    mdrt_vdp *v = mdrt_get_vdp(m);
    mdrt_vdp_ctrl16(v, 0x8144);                           /* (the display on) */
    mdrt_psg_port(m, 0, 0x80 | 14);                       /* a tone: the sound queue never runs dry for want of sound */
    mdrt_psg_port(m, 100, 15);
    mdrt_psg_port(m, 200, 0x90);
    SDL_Thread *t = SDL_CreateThread(presser, "presser", NULL);
    int was = 0;
    for (;;) {
        mdrt_vblank_end(m);
        if (mdrt_wait_vblank(m)) break;
        Uint64 now = SDL_GetPerformanceCounter();
        if (frame_n < 20000) frame_at[frame_n++] = now;
        int start = (mdrt_get_pads(m)->buttons[0] & MDRT_PAD_START) != 0;
        int n = AGET(&seen);
        if (start && !was && n < PRESSES) { noticed[n] = now; ASET(&seen, n + 1); }
        was = start;
        ASET(&frames, AGET(&frames) + 1);
    }
    for (int i = 0; i < 2000 && !AGET(&ended); i++) SDL_Delay(5);   /* (SDL_WaitThread does not wait after SDL3's SDL_Quit) */
    SDL_WaitThread(t, NULL);
}

static int by(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

int main(int argc, char **argv)
{
    if (chdir(argc > 1 ? argv[1] : ".")) { perror("chdir"); return 1; }
    remove("settings.ini");
    FILE *f = fopen("settings.ini", "w");                 /* (run-ahead off: the platform's latency alone) */
    if (f) { fputs("general.run_ahead = 0\n", f); fclose(f); }
#ifdef MDRT_SDL3
    SDL_setenv_unsafe("SDL_VIDEO_DRIVER", "dummy", 1);
    SDL_setenv_unsafe("SDL_AUDIO_DRIVER", "disk", 1);
    SDL_setenv_unsafe("SDL_AUDIO_DISK_OUTPUT_FILE", "sound.raw", 1);
    const char *which = "SDL3";
#else
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    SDL_setenv("SDL_AUDIODRIVER", "disk", 1);
    SDL_setenv("SDL_DISKAUDIOFILE", "sound.raw", 1);
    const char *which = "SDL2";
#endif
    mdrt_config c = { 0 };
    c.audio_rate = 48000;
    mdrt_sdl_options o = { .title = "latency test", .app = "latency-test" };
    CHECK_EQ(mdrt_sdl_run(&c, &o, game, NULL), 0);
    double freq = (double)SDL_GetPerformanceFrequency(), ms[PRESSES], gap[20000];
    int n = AGET(&seen);
    for (int i = 0; i < n; i++) ms[i] = (double)(noticed[i] - pushed[i]) * 1000.0 / freq;
    int g = 0;
    for (int i = 61; i < frame_n; i++) gap[g++] = (double)(frame_at[i] - frame_at[i - 1]) * 1000.0 / freq;
    CHECK_EQ(n, PRESSES);
    if (n) {
        qsort(ms, (size_t)n, sizeof *ms, by);
        double s = 0; for (int i = 0; i < n; i++) s += ms[i];
        printf("  %s, a key's event to the frame that sees it (ms): median %.2f, mean %.2f, min %.2f, p95 %.2f, max %.2f (%d presses)\n",
               which, ms[n / 2], s / n, ms[0], ms[n * 95 / 100], ms[n - 1], n);
    }
    if (g) {
        qsort(gap, (size_t)g, sizeof *gap, by);
        double s = 0; for (int i = 0; i < g; i++) s += gap[i];
        printf("  %s, the frames' spacing (ms): median %.3f, mean %.3f, p1 %.3f, p99 %.3f, max %.3f (%d frames)\n",
               which, gap[g / 2], s / g, gap[g / 100], gap[g * 99 / 100], gap[g - 1], g);
    }
    return check_done("latency_test");
}
