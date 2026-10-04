/*
 * Standalone Wii / GameCube runner for a game built on the native N64 runtime (no engine):
 * boots the asset pack from the SD card and shows the game full screen. Used to bring up and
 * test the console backends in Dolphin and on hardware.
 *
 *   sd:/n64port/game.n64pak   game data (or the path in sd:/n64port/path.txt)
 *   sd:/n64port/frames.txt    optional: stop after this many frames (automated runs)
 *   sd:/n64port/fuzz.txt      optional: seed for random input on two pads (same sequence as
 *                             host/main.c --fuzz, so runs can be compared with other targets)
 *   sd:/n64port/verbose.txt   optional: from this frame on, log every frame and backend detail
 *   sd:/n64port/dump.txt      optional: every this many frames, write the picture to the log
 *                             as hex (tools/dolphin_frames.py turns the log into images)
 *
 * Log lines go to the debugger UART (Dolphin: OSREPORT).
 */
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <fat.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port_host.h"

extern int gPortVerbose;

static GXRModeObj *sMode;
static void *sXfb[2];
static int sXfbIndex;

/* The last few log lines are kept in memory too: the hang report prints them, which shows a
 * line whose output itself never completed. */
#define LOG_RING 6
static char sLogRing[LOG_RING][200];
static unsigned int sLogRingNum;

static void log_sink(const char *line)
{
    strncpy(sLogRing[sLogRingNum % LOG_RING], line, sizeof(sLogRing[0]) - 1);
    sLogRingNum++;
    SYS_Report("%s\n", line);
}

/* N64 pad from GameCube controller port `port`. */
static void read_pad(int port, PortPad *pad)
{
    u16 held = PAD_ButtonsHeld(port);
    s8 x = PAD_StickX(port), y = PAD_StickY(port);
    s8 cx = PAD_SubStickX(port), cy = PAD_SubStickY(port);

    memset(pad, 0, sizeof(*pad));
    pad->connected = 1;
    if (held & PAD_BUTTON_A) pad->buttons |= 0x8000;
    if (held & PAD_BUTTON_B) pad->buttons |= 0x4000;
    if (held & PAD_TRIGGER_Z) pad->buttons |= 0x2000;
    if (held & PAD_BUTTON_START) pad->buttons |= 0x1000;
    if (held & PAD_BUTTON_UP) pad->buttons |= 0x0800;
    if (held & PAD_BUTTON_DOWN) pad->buttons |= 0x0400;
    if (held & PAD_BUTTON_LEFT) pad->buttons |= 0x0200;
    if (held & PAD_BUTTON_RIGHT) pad->buttons |= 0x0100;
    if (held & PAD_TRIGGER_L) pad->buttons |= 0x0020;
    if (held & PAD_TRIGGER_R) pad->buttons |= 0x0010;
    if (cy > 40 || (held & PAD_BUTTON_Y)) pad->buttons |= 0x0008;
    if (cy < -40 || (held & PAD_BUTTON_X)) pad->buttons |= 0x0004;
    if (cx < -40) pad->buttons |= 0x0002;
    if (cx > 40) pad->buttons |= 0x0001;
    /* The N64 stick reports roughly -80..80, the GameCube one roughly -100..100. */
    pad->stick_x = (signed char)(x * 4 / 5);
    pad->stick_y = (signed char)(y * 4 / 5);
}

#ifndef PORT_GFX_GPU
/* Software renderer: put the game's RGBA frame on the external framebuffer (YUY2), doubled. */
static void present_rgba(const unsigned char *rgba, int width, int height, u32 *xfb)
{
    int fb_w = sMode->fbWidth, fb_h = sMode->xfbHeight, x, y;

    for (y = 0; y < fb_h; y++)
    {
        const unsigned char *row = rgba + (y * height / fb_h) * width * 4;
        u32 *dst = xfb + y * (fb_w / 2);

        for (x = 0; x < fb_w; x += 2)
        {
            const unsigned char *p = row + (x * width / fb_w) * 4;
            int r = p[0], g = p[1], b = p[2];
            int luma = (299 * r + 587 * g + 114 * b) / 1000;
            int cb = (-16874 * r - 33126 * g + 50000 * b + 12800000) / 100000;
            int cr = (50000 * r - 41869 * g - 8131 * b + 12800000) / 100000;

            *dst++ = ((u32)luma << 24) | ((u32)cb << 16) | ((u32)luma << 8) | (u32)cr;
        }
    }
}
#endif

/* The current picture (320 x 240 RGB) as hex lines in the log: "FRAME <frame> <row> <hex>". */
static void dump_frame(int frame)
{
    static unsigned char sRgb[320 * 240 * 3];
    static const char digits[] = "0123456789ABCDEF";
    char line[64 + 320 * 2 + 1];
    int row, part, i;

#ifdef PORT_GFX_GPU
    {
        extern void port_gpu_gx_read_frame(unsigned char *rgb);

        port_gpu_gx_read_frame(sRgb);
    }
#else
    {
        int width, height;
        const unsigned char *rgba = n64_framebuffer(&width, &height);

        if (rgba == NULL || width != 320 || height != 240)
        {
            return;
        }
        for (i = 0; i < 320 * 240; i++)
        {
            sRgb[i * 3] = rgba[i * 4]; sRgb[i * 3 + 1] = rgba[i * 4 + 1]; sRgb[i * 3 + 2] = rgba[i * 4 + 2];
        }
    }
#endif
    for (row = 0; row < 240; row++)
    {
        /* three log lines per row: SYS_Report's buffer is small */
        for (part = 0; part < 3; part++)
        {
            const unsigned char *src = &sRgb[(row * 320 + part * 107) * 3];
            int count = (part == 2) ? 320 - 214 : 107;

            for (i = 0; i < count * 3; i++)
            {
                line[i * 2] = digits[src[i] >> 4];
                line[i * 2 + 1] = digits[src[i] & 15];
            }
            line[count * 6] = 0;
            SYS_Report("FRAME %d %d %d %s\n", frame, row, part, line);
        }
    }
}

/*
 * Watchdog: an alarm (interrupt context) that notices when the frame counter stops moving and
 * reports where. Interrupts are taken on the interrupted thread's stack, so walking the stack
 * frames up from here passes through the interrupt handler into the code that hangs; the
 * addresses resolve with powerpc-eabi-addr2line against the .elf.
 */
static volatile int sWatchFrame;
static int sWatchLast = -1, sWatchReported;
static syswd_t sWatchAlarm;

static int watch_valid(u32 addr)
{
    return (addr >= 0x80003000u && addr < 0x81800000u) || (addr >= 0x90000000u && addr < 0x94000000u);
}

static void watchdog_cb(syswd_t alarm, void *arg)
{
    if (sWatchReported == 1)
    {
        /* second look, one period later: counters that moved are inside the loop */
        sWatchReported = 2;
        SYS_Report("n64port: progress later: recv %u send %u dma %u yield %u gfx %u\n", gPortProgress[0], gPortProgress[1],
                   gPortProgress[2], gPortProgress[3], gPortProgress[4]);
        SYS_Report("n64port: stopped (hang)\n");
    }
    if (sWatchFrame == sWatchLast && !sWatchReported)
    {
        int depth;

        sWatchReported = 1;
        SYS_Report("n64port: HANG in frame %d\n", sWatchFrame);
        {
            /*
             * libogc's interrupt entry saves the interrupted thread's registers in the context
             * block SPRG2 points at (r0 at 24, r1 at 28, ...). Its code-like words include the
             * interrupted pc and lr, and r1 gives the thread's stack to walk.
             */
            u32 *ctx, *frame;

            __asm__ volatile("mfsprg %0, 2" : "=r"(ctx));
            for (depth = 0; depth < 64; depth++)
            {
                if (ctx[depth] >= 0x80004000u && ctx[depth] < 0x80300000u && (ctx[depth] & 3) == 0)
                {
                    SYS_Report("n64port:   ctx[%d] %08X\n", depth, (unsigned)ctx[depth]);
                }
            }
            frame = (u32 *)ctx[7];
            for (depth = 0; depth < 40 && watch_valid((u32)frame); depth++)
            {
                SYS_Report("n64port:   %08X\n", (unsigned)frame[1]);
                frame = (u32 *)frame[0];
            }
        }
        for (depth = 0; depth < LOG_RING; depth++)
        {
            SYS_Report("n64port: recent log: %s\n", sLogRing[(sLogRingNum + depth) % LOG_RING]);
        }
        {
            extern void port_plat_dump_trace(void);

            port_plat_dump_trace();
        }
        SYS_Report("n64port: progress: recv %u send %u dma %u yield %u gfx %u\n", gPortProgress[0], gPortProgress[1],
                   gPortProgress[2], gPortProgress[3], gPortProgress[4]);
    }
    sWatchLast = sWatchFrame;
}

static void watchdog_start(void)
{
    struct timespec period;

    period.tv_sec = 4;
    period.tv_nsec = 0;
    SYS_CreateAlarm(&sWatchAlarm);
    SYS_SetPeriodicAlarm(sWatchAlarm, &period, &period, watchdog_cb, NULL);
}

static int read_number(const char *path, int fallback)
{
    FILE *f = fopen(path, "r");
    int value = fallback;

    if (f != NULL)
    {
        if (fscanf(f, "%d", &value) != 1)
        {
            value = fallback;
        }
        fclose(f);
    }
    return value;
}

int main(int argc, char **argv)
{
    char path[256] = "sd:/n64port/game.n64pak";
    int frames_max, dump_every, verbose_from, frame = 0, i;
    unsigned int fuzz_seed, fuzz_state = 0;
    int fuzz_hold[2] = { 0, 0 };
    PortPad fuzz_pad[2] = { { 0, 0, 0, 1 }, { 0, 0, 0, 1 } };
    u64 work_ticks = 0;
    FILE *f;

    SYS_STDIO_Report(true); /* stdout / stderr, including libogc's exception report, to the UART */
    VIDEO_Init();
    PAD_Init();
    sMode = VIDEO_GetPreferredMode(NULL);
    sXfb[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(sMode));
    sXfb[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(sMode));
    VIDEO_Configure(sMode);
    VIDEO_SetNextFramebuffer(sXfb[0]);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();

    port_set_log_sink(log_sink);
    port_set_fault_containment(1);
    if (!fatInitDefault())
    {
        SYS_Report("n64port: no SD card (fatInitDefault failed)\n");
    }
    f = fopen("sd:/n64port/path.txt", "r");
    if (f != NULL)
    {
        if (fgets(path, sizeof(path), f) != NULL)
        {
            path[strcspn(path, "\r\n")] = '\0';
        }
        fclose(f);
    }
    frames_max = read_number("sd:/n64port/frames.txt", 0);
    dump_every = read_number("sd:/n64port/dump.txt", 0);
    fuzz_seed = (unsigned int)read_number("sd:/n64port/fuzz.txt", 0);
    verbose_from = read_number("sd:/n64port/verbose.txt", 0);
    SYS_Report("n64port: starting '%s'\n", path);

#ifdef PORT_GFX_GPU
    {
        extern void port_gpu_gx_init(GXRModeObj *mode);

        port_gpu_gx_init(sMode);
    }
#endif
    if (!n64_boot(path))
    {
        SYS_Report("n64port: could not boot from '%s'\n", path);
        return 1;
    }
    SYS_Report("n64port: booted, running %d frames\n", frames_max);
    watchdog_start();
    while (n64_is_running() && (frames_max == 0 || frame < frames_max))
    {
        PortPad pad;

        u64 start;

        PAD_ScanPads();
        for (i = 0; i < 4; i++)
        {
            read_pad(i, &pad);
            pad.connected = (i == 0) ? 1 : 0; /* TODO: detect the other ports */
            n64_set_pad(i, &pad);
        }
        if (fuzz_seed != 0 && frame >= 400)
        {
            /* Same generator as host/main.c. */
            if (fuzz_state == 0)
            {
                fuzz_state = fuzz_seed * 2654435761u + 1;
            }
            for (i = 0; i < 2; i++)
            {
                if (fuzz_hold[i]-- <= 0)
                {
                    static const unsigned short choices[] = { 0x8000, 0x8000, 0x8000, 0x4000, 0x4000, 0x1000, 0x2000,
                                                              0x0010, 0x0008, 0x0004, 0, 0, 0, 0, 0 };
                    static const signed char sticks[] = { 0, 0, 0, 80, -80, 40, -40 };
                    unsigned int r;

                    fuzz_state = fuzz_state * 1664525u + 1013904223u;
                    r = fuzz_state >> 8;
                    fuzz_pad[i].buttons = choices[r % (sizeof(choices) / sizeof(choices[0]))];
                    fuzz_pad[i].stick_x = sticks[(r >> 5) % sizeof(sticks)];
                    fuzz_pad[i].stick_y = sticks[(r >> 9) % sizeof(sticks)];
                    fuzz_hold[i] = 2 + (int)((r >> 13) % 24);
                    if (frame < 1000 && i == 0 && (r & 3) == 0)
                    {
                        fuzz_pad[i].buttons = 0x1000;
                    }
                }
                n64_set_pad(i, &fuzz_pad[i]);
            }
        }
        if (verbose_from > 0 && frame >= verbose_from)
        {
            /* Bring-up aid: say which frame is about to run, and let the backends talk. */
            SYS_Report("n64port: frame %d begins\n", frame);
            gPortVerbose = 1;
        }
        sWatchFrame = frame;
        start = gettime();
        n64_run_frame();
        work_ticks += gettime() - start;
        /* Numbered like host/main.c: the picture after loop pass `frame`. */
        if (dump_every > 0 && (frame % dump_every) == 0)
        {
            dump_frame(frame);
        }
        frame++;

        sXfbIndex ^= 1;
#ifdef PORT_GFX_GPU
        {
            extern void port_gpu_gx_present(void *xfb);

            port_gpu_gx_present(sXfb[sXfbIndex]);
        }
#else
        {
            int width, height;
            const unsigned char *rgba = n64_framebuffer(&width, &height);

            if (rgba != NULL)
            {
                present_rgba(rgba, width, height, sXfb[sXfbIndex]);
            }
        }
#endif
        VIDEO_SetNextFramebuffer(sXfb[sXfbIndex]);
        VIDEO_Flush();
        VIDEO_WaitVSync();
        if ((frame % 60) == 0)
        {
            SYS_Report("n64port: frame %d, %d.%d ms per game frame\n", frame, (int)(ticks_to_microsecs(work_ticks) / 60000),
                       (int)(ticks_to_microsecs(work_ticks) / 6000 % 10));
            work_ticks = 0;
        }
    }
    SYS_Report("n64port: stopped after %d frames (%s)\n", frame, n64_is_running() ? "frame limit" : "game stopped");
    n64_shutdown();
    return 0;
}
