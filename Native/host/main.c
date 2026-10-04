/* Headless runner for a game built on the native N64 runtime (no engine needed). */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#else
#include <time.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port_host.h"
#include "port_bridge.h"

static void write_ppm(const char *path, const unsigned char *rgba, int width, int height)
{
    FILE *f = fopen(path, "wb");
    int i;

    if (f == NULL)
    {
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", width, height);
    for (i = 0; i < width * height; i++)
    {
        fwrite(rgba + i * 4, 1, 3, f);
    }
    fclose(f);
}

#ifdef _WIN32
/* Print a symbolised call stack for the given register context. */
static void print_stack(CONTEXT ctx)
{
    HANDLE process = GetCurrentProcess();
    char buffer[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *symbol = (SYMBOL_INFO *)buffer;
    int depth;

    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(process, NULL, TRUE);
    for (depth = 0; depth < 40 && ctx.Rip != 0; depth++)
    {
        DWORD64 displacement = 0, image_base = 0, pc = ctx.Rip;
        DWORD line_displacement = 0;
        IMAGEHLP_LINE64 line;
        RUNTIME_FUNCTION *function = RtlLookupFunctionEntry(pc, &image_base, NULL);

        /* Unwind first so `ctx` describes the caller, then report `pc`. */
        if (function == NULL)
        {
            ctx.Rip = *(DWORD64 *)ctx.Rsp;
            ctx.Rsp += 8;
        }
        else
        {
            void *handler_data;
            DWORD64 establisher;

            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, pc, function, &ctx, &handler_data, &establisher, NULL);
        }
        memset(buffer, 0, sizeof(buffer));
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        line.SizeOfStruct = sizeof(line);
        if (SymFromAddr(process, pc, &displacement, symbol))
        {
            if (SymGetLineFromAddr64(process, pc, &line_displacement, &line))
            {
                const char *file = strrchr(line.FileName, '\\');
                fprintf(stderr, "  #%d %s (%s:%lu)\n", depth, symbol->Name, file ? file + 1 : line.FileName, line.LineNumber);
            }
            else
            {
                fprintf(stderr, "  #%d %s+0x%llX\n", depth, symbol->Name, displacement);
            }
        }
        else
        {
            fprintf(stderr, "  #%d %p\n", depth, (void *)pc);
        }
    }
    fflush(stderr);
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *info)
{
    if (info->ExceptionRecord->ExceptionCode < 0xC0000000 || gPortFaultGuard != 0)
    {
        return EXCEPTION_CONTINUE_SEARCH; /* debug output, C++ exceptions, faults the port handles itself */
    }
    fprintf(stderr, "CRASH: exception 0x%08lX at %p", info->ExceptionRecord->ExceptionCode,
            info->ExceptionRecord->ExceptionAddress);
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
    {
        fprintf(stderr, " (%s address %p)", info->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                (void *)info->ExceptionRecord->ExceptionInformation[1]);
    }
    fprintf(stderr, "\n");
    print_stack(*info->ContextRecord);
    TerminateProcess(GetCurrentProcess(), info->ExceptionRecord->ExceptionCode);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Watchdog: if a frame takes too long, show where the game is stuck and exit. */
static volatile LONG sFrameCounter;
static HANDLE sMainThread;
static int sWatchdogSeconds = 10;

static DWORD WINAPI watchdog_thread(void *param)
{
    LONG last = -1;

    for (;;)
    {
        Sleep(sWatchdogSeconds * 1000);
        if (sFrameCounter == last)
        {
            CONTEXT ctx;

            SuspendThread(sMainThread);
            memset(&ctx, 0, sizeof(ctx));
            ctx.ContextFlags = CONTEXT_FULL;
            GetThreadContext(sMainThread, &ctx);
            fprintf(stderr, "HANG: frame %ld did not finish within %d s; stuck at:\n", last + 1, sWatchdogSeconds);
            print_stack(ctx);
            TerminateProcess(GetCurrentProcess(), 3);
        }
        last = sFrameCounter;
    }
}

static void diagnostics_start(void)
{
    AddVectoredExceptionHandler(1, crash_filter);
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &sMainThread, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    CreateThread(NULL, 0, watchdog_thread, NULL, 0, NULL);
}

static double now_ms(void)
{
    LARGE_INTEGER count, freq;

    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (double)count.QuadPart * 1000.0 / (double)freq.QuadPart;
}
#else
/* Elsewhere crashes and hangs are left to the platform's own tools (core dumps, timeout). */
static volatile long sFrameCounter;
static int sWatchdogSeconds = 10;

#ifdef __GLIBC__
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>

static void crash_handler(int sig)
{
    void *frames[48];
    int count = backtrace(frames, 48);

    fprintf(stderr, "CRASH: signal %d\n", sig);
    backtrace_symbols_fd(frames, count, 2);
    _exit(128 + sig);
}
#endif

static void diagnostics_start(void)
{
#ifdef __GLIBC__
    signal(SIGSEGV, crash_handler);
    signal(SIGBUS, crash_handler);
    signal(SIGFPE, crash_handler);
    signal(SIGILL, crash_handler);
    signal(SIGABRT, crash_handler);
#endif
}

static double now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}
#endif

int main(int argc, char **argv)
{
    const char *rom = "baserom.us.z64";
    const char *dump = NULL;
    int frames = 60, dump_every = 30, i;
    int press_from = 0, press_every = 0;
    unsigned int press_buttons = 0x1000; /* START */
    const char *input_path = NULL;
    static struct { int frame; unsigned int buttons; int sx, sy; } script[4096];
    int script_num = 0, script_pos = 0, dump_from = 0;
    PortPad script_pad = { 0, 0, 0, 1 };
    unsigned int fuzz_seed = 0, fuzz_state = 0;
    int bridge_log = 0, request_at = -1, request_id = 0; /* --bridge: log events; --request-at F "name a b" */
    const char *request_text = NULL;
    int fuzz_until = 0x7FFFFFFF; /* random input stops here; an --input script takes over */
    int fuzz_hold[2] = { 0, 0 };
    PortPad fuzz_pad[2] = { { 0, 0, 0, 1 }, { 0, 0, 0, 1 } };
    double time_start, time_end;
    const char *wav_path = NULL;
    FILE *wav = NULL;
    unsigned int wav_bytes = 0, audio_frames_total = 0;

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--rom") == 0 && i + 1 < argc) rom = argv[++i];
        else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) frames = atoi(argv[++i]);
        else if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc) dump = argv[++i];
        else if (strcmp(argv[i], "--every") == 0 && i + 1 < argc) dump_every = atoi(argv[++i]);
        else if (strcmp(argv[i], "--watchdog") == 0 && i + 1 < argc) sWatchdogSeconds = atoi(argv[++i]);
        /* Scripted input: from frame F on, tap the buttons (hex mask, default START) every N frames. */
        else if (strcmp(argv[i], "--press-from") == 0 && i + 1 < argc) press_from = atoi(argv[++i]);
        else if (strcmp(argv[i], "--press-every") == 0 && i + 1 < argc) press_every = atoi(argv[++i]);
        else if (strcmp(argv[i], "--press-buttons") == 0 && i + 1 < argc) press_buttons = strtoul(argv[++i], NULL, 16);
        /* Input script: lines of "frame buttons_hex stick_x stick_y"; each state holds until the next line. */
        else if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) input_path = argv[++i];
        else if (strcmp(argv[i], "--dump-from") == 0 && i + 1 < argc) dump_from = atoi(argv[++i]);
        /* Random input on two controllers from frame 400 on (soak testing); the seed picks the run. */
        else if (strcmp(argv[i], "--fuzz") == 0 && i + 1 < argc) fuzz_seed = strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--fuzz-until") == 0 && i + 1 < argc) fuzz_until = atoi(argv[++i]);
        else if (strcmp(argv[i], "--bridge") == 0) bridge_log = 1;
        else if (strcmp(argv[i], "--request-at") == 0 && i + 2 < argc) { request_at = atoi(argv[++i]); request_text = argv[++i]; }
        /* Write everything the game plays to a 16-bit stereo WAV file. */
        else if (strcmp(argv[i], "--wav") == 0 && i + 1 < argc) wav_path = argv[++i];
        /* Keep the cartridge save in this file (loaded at boot, written when the game saves). */
        else if (strcmp(argv[i], "--save") == 0 && i + 1 < argc) n64_set_save_path(argv[++i]);
        else
        {
            fprintf(stderr, "usage: <game>_host [--rom path] [--frames N] [--dump dir] [--every N] [--watchdog sec]\n"
                            "                  [--press-from F --press-every N [--press-buttons HEX]]\n"
                            "                  [--input script.txt] [--dump-from F] [--fuzz seed] [--wav out.wav]\n");
            return 2;
        }
    }
    if (input_path != NULL)
    {
        FILE *f = fopen(input_path, "r");
        char line[256];

        if (f == NULL)
        {
            fprintf(stderr, "cannot open input script '%s'\n", input_path);
            return 2;
        }
        while (fgets(line, sizeof(line), f) != NULL && script_num < 4096)
        {
            if (sscanf(line, "%d %x %d %d", &script[script_num].frame, &script[script_num].buttons,
                       &script[script_num].sx, &script[script_num].sy) == 4)
            {
                script_num++;
            }
        }
        fclose(f);
    }
    diagnostics_start();

    if (!n64_boot(rom))
    {
        return 1;
    }
    if (wav_path != NULL && (wav = fopen(wav_path, "wb")) != NULL)
    {
        unsigned char header[44] = { 0 };

        fwrite(header, 1, sizeof(header), wav); /* filled in at the end */
    }
    time_start = now_ms();
    for (i = 0; i < frames; i++)
    {
        if (press_every > 0)
        {
            PortPad pad = { 0, 0, 0, 1 };

            if (i >= press_from && ((i - press_from) % press_every) < 2)
            {
                pad.buttons = (unsigned short)press_buttons;
            }
            n64_set_pad(0, &pad);
        }
        if (fuzz_seed != 0 && i == fuzz_until)
        {
            PortPad idle = { 0, 0, 0, 1 };

            n64_set_pad(1, &idle);
        }
        if (script_num > 0 && (fuzz_seed == 0 || i >= fuzz_until))
        {
            while (script_pos < script_num && script[script_pos].frame <= i)
            {
                script_pad.buttons = (unsigned short)script[script_pos].buttons;
                script_pad.stick_x = (signed char)script[script_pos].sx;
                script_pad.stick_y = (signed char)script[script_pos].sy;
                script_pos++;
            }
            n64_set_pad(0, &script_pad);
        }
        if (fuzz_seed != 0 && i >= 400 && i < fuzz_until)
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
                    /* Buttons weighted towards the ones that move through menus and fight. */
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
                    if (i < 1000 && p == 0 && (r & 3) == 0)
                    {
                        fuzz_pad[p].buttons = 0x1000; /* get past the intro and title */
                    }
                }
                n64_set_pad(p, &fuzz_pad[p]);
            }
        }
        n64_run_frame();
        sFrameCounter++;
        if (i == request_at && request_text != NULL)
        {
            char name[64];
            int args[PB_MAX_ARGS], nargs = 0, used = 0;

            if (sscanf(request_text, "%63s%n", name, &used) == 1)
            {
                const char *rest = request_text + used;
                int step;

                if (strcmp(name, "set") == 0 && sscanf(rest, " %58s%n", name + 4, &step) == 1)
                {
                    memcpy(name, "set ", 4);
                    rest += step;
                }
                while (nargs < PB_MAX_ARGS && sscanf(rest, " %d%n", &args[nargs], &step) == 1)
                {
                    nargs++;
                    rest += step;
                }
                request_id = n64_bridge_request(name, args, nargs);
                fprintf(stderr, "bridge: frame %d request '%s' (%d args) -> id %d\n", i, name, nargs, request_id);
            }
        }
        if (bridge_log)
        {
            char name[64];
            int args[PB_MAX_ARGS], nargs, result, k;

            while (n64_bridge_poll_event(name, sizeof(name), args, PB_MAX_ARGS, &nargs))
            {
                fprintf(stderr, "bridge: frame %d event %s", i, name);
                for (k = 0; k < nargs; k++) fprintf(stderr, " %d", args[k]);
                fprintf(stderr, "\n");
            }
            if (request_id != 0 && n64_bridge_result(request_id, &result))
            {
                fprintf(stderr, "bridge: frame %d request %d result %d\n", i, request_id, result);
                request_id = 0;
            }
            if (i == frames - 1 || (request_at >= 0 && (i == request_at - 1 || i == request_at + 120)))
            {
                for (k = 0; k < n64_bridge_var_count(); k++)
                {
                    const PortBridgeVar *var = n64_bridge_var(k);
                    char text[128];
                    double value;
                    int e;

                    fprintf(stderr, "bridge: frame %d %s =", i, var->name);
                    for (e = 0; e < ((var->type == PB_STR) ? 1 : var->count); e++)
                    {
                        int kind = n64_bridge_get(var->name, e, &value, text, sizeof(text));

                        if (kind == 1) fprintf(stderr, " %g", value);
                        else if (kind == 2) fprintf(stderr, " \"%s\"", text);
                    }
                    fprintf(stderr, "\n");
                }
            }
        }
        {
            int audio_frames, audio_rate;
            const short *pcm = n64_audio(&audio_frames, &audio_rate);

            audio_frames_total += audio_frames;
            if (wav != NULL && pcm != NULL && audio_frames > 0)
            {
                /* WAV data is little-endian whatever the host is. */
                int n;

                for (n = 0; n < audio_frames * 2; n++)
                {
                    unsigned char le[2];

                    le[0] = (unsigned char)(pcm[n] & 0xFF);
                    le[1] = (unsigned char)((pcm[n] >> 8) & 0xFF);
                    wav_bytes += (unsigned int)fwrite(le, 1, 2, wav);
                }
            }
        }
        if (dump != NULL && i >= dump_from && (i % dump_every) == 0)
        {
            char path[512];
            int width, height;
            const unsigned char *fb = n64_framebuffer(&width, &height);

            if (fb != NULL)
            {
                snprintf(path, sizeof(path), "%s/frame_%05d.ppm", dump, i);
                write_ppm(path, fb, width, height);
            }
        }
    }
    time_end = now_ms();
    if (wav != NULL)
    {
        unsigned int rate = 32000, byte_rate = rate * 4, riff = wav_bytes + 36, fmt_size = 16;
        unsigned short pcm_tag = 1, channels = 2, align = 4, bits = 16;

        fseek(wav, 0, SEEK_SET);
        fwrite("RIFF", 1, 4, wav); fwrite(&riff, 4, 1, wav); fwrite("WAVEfmt ", 1, 8, wav);
        fwrite(&fmt_size, 4, 1, wav); fwrite(&pcm_tag, 2, 1, wav); fwrite(&channels, 2, 1, wav);
        fwrite(&rate, 4, 1, wav); fwrite(&byte_rate, 4, 1, wav); fwrite(&align, 2, 1, wav); fwrite(&bits, 2, 1, wav);
        fwrite("data", 1, 4, wav); fwrite(&wav_bytes, 4, 1, wav);
        fclose(wav);
    }
    printf("audio: %u sample frames (%.1f per video frame)\n", audio_frames_total,
           (double)audio_frames_total / (frames > 0 ? frames : 1));
    printf("arena: %.1f MB used\n", (double)port_arena_used() / (1024.0 * 1024.0));
    n64_shutdown();
    printf("ran %d frames, %.2f ms per frame\n", frames,
           (time_end - time_start) / (frames > 0 ? frames : 1));
    return 0;
}
