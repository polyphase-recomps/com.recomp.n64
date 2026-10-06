/*
 * Standalone 3DS runner (no engine): boots the game, drives it with the pad or with the same
 * random input as host/main.c, and draws it with the citro3d backend on the top screen.
 * A test harness for Azahar and real hardware.
 *
 * Reads sdmc:/n64port/run.txt (all optional, one per line):
 *   rom=<path>      default sdmc:/n64port/ssb64.n64pak
 *   frames=<n>      stop after n frames (0 = run until START + SELECT)
 *   fuzz=<seed>     random input on two pads from frame 400, like host/main.c --fuzz
 *   every=<n>       write the top screen to sdmc:/n64port/frames/frame_<n>.bin every n frames
 *                   (240 x 400 portrait RGB8 as the screen gets it, see tools/ctr_frames.py)
 *   verbose=<n>     backend logging from frame n on
 *   pattern=1       draw a test picture through the backend instead of running the game
 * Log: sdmc:/n64port/log.txt, and the debug output (svcOutputDebugString) Azahar shows.
 */
#include <3ds.h>
#include <citro3d.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "port_host.h"
#include "port_gpu.h"

void port_gpu_c3d_render(int screen_w, int screen_h);

#define DISPLAY_TRANSFER_FLAGS \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))
#define DUMP_TRANSFER_FLAGS \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

static FILE *sLog;

static void log_sink(const char *line)
{
    svcOutputDebugString(line, (s32)strlen(line));
    if (sLog != NULL)
    {
        fputs(line, sLog);
        fputc('\n', sLog);
        fflush(sLog);
    }
}

static void say(const char *fmt, ...)
{
    char line[256];
    va_list args;

    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    log_sink(line);
}

static double now_ms(void)
{
    return (double)svcGetSystemTick() / (double)SYSCLOCK_ARM11 * 1000.0;
}

/* A test picture: a gradient / checker texture on the left half, vertex colours on the right. */
static void draw_pattern(void)
{
    static unsigned int handle;
    static unsigned char pixels[64 * 32 * 4];
    PortGpuState st;
    PortGpuVtx v[4];
    int x, y, i;

    port_gpu_frame_begin();
    if (handle == 0)
    {
        for (y = 0; y < 32; y++)
        {
            for (x = 0; x < 64; x++)
            {
                unsigned char *p = &pixels[(y * 64 + x) * 4];

                p[0] = (unsigned char)(x * 4);
                p[1] = (unsigned char)(y * 8);
                p[2] = (((x >> 3) ^ (y >> 3)) & 1) ? 255 : 0;
                p[3] = 255;
            }
        }
        handle = port_gpu_texture_create(0x1234, 64, 32, pixels);
    }
    memset(&st, 0, sizeof(st));
    st.cycles = 1;
    st.cycle[0].d = PORT_GPU_IN_TEXEL;
    st.cycle[0].ad = PORT_GPU_IN_ONE;
    st.texture = handle;
    st.wrap_s = st.wrap_t = PORT_GPU_WRAP_CLAMP;
    st.ortho = 1;
    for (i = 0; i < 4; i++)
    {
        int right = (i == 1 || i == 2), bottom = (i >= 2);

        v[i].x = right ? -0.05f : -0.95f;
        v[i].y = bottom ? -0.9f : 0.9f;
        v[i].z = 0.0f;
        v[i].w = 1.0f;
        v[i].s = right ? 1.0f : 0.0f;
        v[i].t = bottom ? 1.0f : 0.0f;
        v[i].r = v[i].g = v[i].b = v[i].a = 255;
    }
    port_gpu_viewport(0.0f, 0.0f, 320.0f, 240.0f);
    port_gpu_scissor(0, 0, 320, 240);
    port_gpu_draw(&st, v, 4);
    st.texture = 0;
    st.cycle[0].d = PORT_GPU_IN_SHADE;
    for (i = 0; i < 3; i++)
    {
        v[i].x = (i == 0) ? 0.05f : 0.95f;
        v[i].y = (i == 1) ? -0.9f : 0.9f;
        v[i].r = (i == 0) ? 255 : 0;
        v[i].g = (i == 1) ? 255 : 0;
        v[i].b = (i == 2) ? 255 : 0;
    }
    port_gpu_draw(&st, v, 3);
    port_gpu_frame_end();
}

static void dump_frame(C3D_RenderTarget *target, int frame)
{
    static u32 *buffer;
    char path[96];
    FILE *f;

    if (buffer == NULL)
    {
        buffer = linearAlloc(240 * 400 * 4);
        if (buffer == NULL)
        {
            return;
        }
    }
    C3D_SyncDisplayTransfer((u32 *)target->frameBuf.colorBuf, GX_BUFFER_DIM(240, 400), buffer,
                            GX_BUFFER_DIM(240, 400), DISPLAY_TRANSFER_FLAGS); /* as the screen gets it: RGB8 */
    snprintf(path, sizeof(path), "sdmc:/n64port/frames/frame_%05d.bin", frame);
    f = fopen(path, "wb");
    if (f != NULL)
    {
        fwrite(buffer, 1, 240 * 400 * 3, f);
        fclose(f);
    }
}

/* The 3DS pad as an N64 controller. */
static void read_pad(PortPad *pad)
{
    u32 held = hidKeysHeld();
    circlePosition circle;
    circlePosition cstick;

    memset(pad, 0, sizeof(*pad));
    pad->connected = 1;
    if (held & KEY_A) pad->buttons |= 0x8000;      /* A */
    if (held & KEY_B) pad->buttons |= 0x4000;      /* B */
    if (held & (KEY_Y | KEY_ZL)) pad->buttons |= 0x2000; /* Z: grab */
    if (held & KEY_START) pad->buttons |= 0x1000;
    if (held & KEY_DUP) pad->buttons |= 0x0800;
    if (held & KEY_DDOWN) pad->buttons |= 0x0400;
    if (held & KEY_DLEFT) pad->buttons |= 0x0200;
    if (held & KEY_DRIGHT) pad->buttons |= 0x0100;
    if (held & (KEY_L | KEY_R | KEY_ZR)) pad->buttons |= 0x0010; /* R: shield */
    if (held & KEY_X) pad->buttons |= 0x0008;      /* C up: jump */
    hidCircleRead(&circle);
    pad->stick_x = (signed char)(circle.dx * 80 / 156);
    pad->stick_y = (signed char)(circle.dy * 80 / 156);
    hidCstickRead(&cstick);
    if (cstick.dy > 40) pad->buttons |= 0x0008;
    if (cstick.dy < -40) pad->buttons |= 0x0004;
    if (cstick.dx < -40) pad->buttons |= 0x0002;
    if (cstick.dx > 40) pad->buttons |= 0x0001;
}

int main(int argc, char **argv)
{
    char rom[128] = "sdmc:/n64port/ssb64.n64pak", line[160];
    int frames = 0, every = 0, verbose_from = -1, frame, pattern = 0;
    unsigned int fuzz_seed = 0, fuzz_state = 0;
    int fuzz_hold[2] = { 0, 0 };
    PortPad fuzz_pad[2] = { { 0, 0, 0, 1 }, { 0, 0, 0, 1 } };
    double t_start, t_game = 0.0, t_draw = 0.0, t_mark;
    C3D_RenderTarget *target;
    FILE *cfg;

    osSetSpeedupEnable(true);
    gfxInitDefault();
    mkdir("sdmc:/n64port", 0777);
    mkdir("sdmc:/n64port/frames", 0777);
    sLog = fopen("sdmc:/n64port/log.txt", "w");
    cfg = fopen("sdmc:/n64port/run.txt", "r");
    if (cfg != NULL)
    {
        while (fgets(line, sizeof(line), cfg) != NULL)
        {
            line[strcspn(line, "\r\n")] = 0;
            if (strncmp(line, "rom=", 4) == 0) snprintf(rom, sizeof(rom), "%s", line + 4);
            else if (strncmp(line, "frames=", 7) == 0) frames = atoi(line + 7);
            else if (strncmp(line, "fuzz=", 5) == 0) fuzz_seed = (unsigned int)strtoul(line + 5, NULL, 10);
            else if (strncmp(line, "every=", 6) == 0) every = atoi(line + 6);
            else if (strncmp(line, "verbose=", 8) == 0) verbose_from = atoi(line + 8);
            else if (strncmp(line, "pattern=", 8) == 0) pattern = atoi(line + 8);
        }
        fclose(cfg);
    }
    say("n64port: 3DS runner, rom %s, frames %d, fuzz %u, every %d", rom, frames, fuzz_seed, every);

    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 4);
    target = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    C3D_RenderTargetSetOutput(target, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);

    port_set_log_sink(log_sink);
    if (pattern)
    {
        for (frame = 0; frame < ((frames > 0) ? frames : 3) && aptMainLoop(); frame++)
        {
            draw_pattern();
            C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
            C3D_RenderTargetClear(target, C3D_CLEAR_ALL, 0x404040FF, 0);
            C3D_FrameDrawOn(target);
            port_gpu_c3d_render(400, 240);
            C3D_FrameEnd(0);
            if (frame < 3) dump_frame(target, frame);
        }
        say("n64port: stopped after pattern");
        C3D_Fini();
        gfxExit();
        return 0;
    }
    if (!n64_boot(rom))
    {
        say("n64port: boot failed");
        for (;;)
        {
            svcSleepThread(100000000LL);
            if (!aptMainLoop()) break;
        }
        return 1;
    }
    say("n64port: booted, arena %llu KB used", port_arena_used() >> 10);

    t_start = now_ms();
    for (frame = 0; aptMainLoop() && (frames == 0 || frame < frames); frame++)
    {
        PortPad pad;

        hidScanInput();
        if ((hidKeysHeld() & (KEY_START | KEY_SELECT)) == (KEY_START | KEY_SELECT))
        {
            break;
        }
        gPortVerbose = (verbose_from >= 0 && frame >= verbose_from);
        if (fuzz_seed != 0)
        {
            if (frame >= 400)
            {
                int p;

                if (fuzz_state == 0)
                {
                    fuzz_state = fuzz_seed * 2654435761u + 1;
                }
                for (p = 0; p < 2; p++)
                {
                    if (fuzz_hold[p]-- <= 0)
                    {
                        static const unsigned short choices[] = { 0x8000, 0x8000, 0x8000, 0x4000, 0x4000, 0x1000, 0x2000,
                                                                  0x0010, 0x0008, 0x0004, 0, 0, 0, 0, 0 };
                        static const signed char sticks[] = { 0, 0, 0, 80, -80, 40, -40 };
                        unsigned int r;

                        fuzz_state = fuzz_state * 1664525u + 1013904223u;
                        r = fuzz_state >> 8;
                        fuzz_pad[p].buttons = choices[r % (sizeof(choices) / sizeof(choices[0]))];
                        fuzz_pad[p].stick_x = sticks[(r >> 5) % sizeof(sticks)];
                        fuzz_pad[p].stick_y = sticks[(r >> 9) % sizeof(sticks)];
                        fuzz_hold[p] = 2 + (int)((r >> 13) % 24);
                        if (frame < 1000 && p == 0 && (r & 3) == 0)
                        {
                            fuzz_pad[p].buttons = 0x1000;
                        }
                    }
                    n64_set_pad(p, &fuzz_pad[p]);
                }
            }
        }
        else
        {
            read_pad(&pad);
            n64_set_pad(0, &pad);
        }

        t_mark = now_ms();
        n64_run_frame();
        t_game += now_ms() - t_mark;
        if (!n64_is_running())
        {
            say("n64port: game stopped at frame %d", frame);
            break;
        }

        t_mark = now_ms();
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C3D_RenderTargetClear(target, C3D_CLEAR_ALL, 0x000000FF, 0);
        C3D_FrameDrawOn(target);
        port_gpu_c3d_render(400, 240);
        C3D_FrameEnd(0);
        t_draw += now_ms() - t_mark;

        if (every > 0 && frame % every == 0)
        {
            dump_frame(target, frame);
        }
        if (frame % 60 == 59)
        {
            say("n64port: frame %d, game %.1f ms, draw %.1f ms per frame, %.1f s total", frame + 1, t_game / 60.0,
                t_draw / 60.0, (now_ms() - t_start) / 1000.0);
            t_game = t_draw = 0.0;
        }
    }
    say("n64port: stopped after %d frames", frame);
    if (sLog != NULL)
    {
        fclose(sLog);
    }
    C3D_Fini();
    gfxExit();
    return 0;
}
