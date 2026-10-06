/*
 * Guest half of the wasm build (compiled into the module with the game).
 *
 * The runtime's guest code (runtime/guest) calls the port_host.h services. In a native build
 * those are host functions; here the guest is a wasm module with its own memory, so the
 * services that only touch memory (arena, overlays, formatting) are implemented in the
 * module, and the rest are thin wrappers around imports (n64w_imports.h) the host provides
 * (runtime/wasm/n64w_host.c).
 *
 * Memory seen by this code is big-endian (tools/wasm_be_cc.py), like the N64's.
 */
#include <port_types.h>
#include <port_host.h>
#include <port_bridge.h>
#include "n64w_imports.h"

/* ---- globals the runtime expects from the host side ------------------------------------ */
int gPortFaultGuard;
int gPortVerbose;
unsigned int gPortProgress[8];

int port_faulted(void)
{
    return n64w_faulted();
}

int port_env_int(const char *name)
{
    return n64w_env_int(name);
}

/* Profiling clock: not measured inside the guest. */
unsigned long long port_ticks(void)
{
    return 0;
}

/* Guest pointers are guest addresses (0x80xxxxxx), never host memory. */
int port_addr_is_native(const void *p)
{
    (void)p;
    return 0;
}

void port_set_log_sink(void (*sink)(const char *line)) {}
void port_set_fault_containment(int enable) {}

#ifdef PORT_RSP_HOST_TASKS
/* ---- RSP tasks: port_gfx.c and port_audio_abi1.c run in the host (PORT_RSP_HOST) ---------- */
#include <PR/sptask.h>
#include "port_guest.h"

extern u32 gPortFrameCount;
u32 gPortGfxTraceFrame; /* (set by port_boot.c; the host reads the same variable itself) */

void port_gfx_run_task(OSTask *task)
{
    gPortProgress[4]++;
    n64w_gfx_task(task, gPortFrameCount);
}

void port_gfx_set_framebuffer(void *fb)
{
    n64w_gfx_swap();
}

void port_gfx_set_lod(s32 mode)
{
    n64w_gfx_set_lod(mode);
}

s32 port_gfx_lod(void)
{
    return n64w_gfx_lod();
}

void port_audio_abi1_run(const void *cmds, u32 count)
{
    n64w_audio_task(cmds, count);
}

/* the host presents the picture */
int n64_draws_to_screen(void)
{
    return 0;
}

const unsigned char *n64_framebuffer(int *width, int *height)
{
    *width = *height = 0;
    return NULL;
}
#endif

/* ---- formatting ---------------------------------------------------------------------- */
typedef struct FmtOut
{
    char *buf;
    u32 len, cap;
} FmtOut;

static void fmt_put(FmtOut *o, char c)
{
    if (o->len + 1 < o->cap)
    {
        o->buf[o->len] = c;
    }
    o->len++;
}

static void fmt_num(FmtOut *o, unsigned long long v, s32 neg, u32 base, s32 upper, s32 width, s32 zero)
{
    char tmp[24];
    s32 n = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";

    do
    {
        tmp[n++] = digits[v % base];
        v /= base;
    } while (v != 0);
    if (neg)
    {
        if (zero)
        {
            fmt_put(o, '-');
        }
        else
        {
            tmp[n++] = '-';
        }
        width--;
    }
    while (width-- > n)
    {
        fmt_put(o, zero ? '0' : ' ');
    }
    while (n > 0)
    {
        fmt_put(o, tmp[--n]);
    }
}

/* printf subset for log lines: %d %i %u %x %X %p %s %c %f %%, flags 0 and -, width, l/ll/z. */
static void fmt_v(FmtOut *o, const char *fmt, __builtin_va_list ap)
{
    for (; *fmt != '\0'; fmt++)
    {
        s32 width = 0, zero = 0, longs = 0, left = 0;
        char c;

        if (*fmt != '%')
        {
            fmt_put(o, *fmt);
            continue;
        }
        fmt++;
        for (; *fmt == '0' || *fmt == '-'; fmt++)
        {
            zero |= *fmt == '0';
            left |= *fmt == '-';
        }
        for (; *fmt >= '0' && *fmt <= '9'; fmt++)
        {
            width = width * 10 + (*fmt - '0');
        }
        if (*fmt == '.')
        {
            for (fmt++; *fmt >= '0' && *fmt <= '9'; fmt++) {}
        }
        for (; *fmt == 'l' || *fmt == 'z' || *fmt == 'h'; fmt++)
        {
            longs += *fmt == 'l';
        }
        c = *fmt;
        if (c == '\0')
        {
            break;
        }
        switch (c)
        {
        case 'd':
        case 'i':
        {
            long long v = (longs >= 2) ? __builtin_va_arg(ap, long long) : __builtin_va_arg(ap, int);

            fmt_num(o, (v < 0) ? (unsigned long long)-v : (unsigned long long)v, v < 0, 10, 0, width, zero);
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        {
            unsigned long long v = (longs >= 2) ? __builtin_va_arg(ap, unsigned long long) : __builtin_va_arg(ap, unsigned int);

            fmt_num(o, v, 0, (c == 'u') ? 10 : 16, c == 'X', width, zero);
            break;
        }
        case 'p':
            fmt_put(o, '0');
            fmt_put(o, 'x');
            fmt_num(o, (unsigned long long)(u32)__builtin_va_arg(ap, void *), 0, 16, 1, 8, 1);
            break;
        case 'c':
            fmt_put(o, (char)__builtin_va_arg(ap, int));
            break;
        case 's':
        {
            const char *s = __builtin_va_arg(ap, const char *);
            s32 n = 0;

            if (s == NULL)
            {
                s = "(null)";
            }
            while (s[n] != '\0')
            {
                n++;
            }
            if (!left)
            {
                while (width-- > n)
                {
                    fmt_put(o, ' ');
                }
            }
            for (; *s != '\0'; s++)
            {
                fmt_put(o, *s);
            }
            if (left)
            {
                while (width-- > n)
                {
                    fmt_put(o, ' ');
                }
            }
            break;
        }
        case 'f':
        case 'g':
        {
            double v = __builtin_va_arg(ap, double);
            unsigned long long whole;
            s32 frac;

            if (v < 0)
            {
                fmt_put(o, '-');
                v = -v;
            }
            whole = (unsigned long long)v;
            frac = (s32)((v - (double)whole) * 1000.0 + 0.5);
            if (frac >= 1000)
            {
                whole++;
                frac -= 1000;
            }
            fmt_num(o, whole, 0, 10, 0, 0, 0);
            fmt_put(o, '.');
            fmt_num(o, (unsigned long long)frac, 0, 10, 0, 3, 1);
            break;
        }
        default:
            fmt_put(o, c);
            break;
        }
    }
    if (o->cap > 0)
    {
        o->buf[(o->len < o->cap) ? o->len : o->cap - 1] = '\0';
    }
}

static void log_v(const char *prefix, const char *fmt, __builtin_va_list ap, s32 fatal)
{
    char line[512];
    FmtOut o = { line, 0, sizeof(line) };

    for (; *prefix != '\0'; prefix++)
    {
        fmt_put(&o, *prefix);
    }
    fmt_v(&o, fmt, ap);
    if (fatal)
    {
        n64w_fatal(line);
    }
    else
    {
        n64w_log(line);
    }
}

void port_log(const char *fmt, ...)
{
    __builtin_va_list ap;

    __builtin_va_start(ap, fmt);
    log_v("[n64] ", fmt, ap, 0);
    __builtin_va_end(ap);
}

void port_logv(const char *prefix, const char *fmt, void *args)
{
    log_v(prefix, fmt, *(__builtin_va_list *)args, 0);
}

void port_fatal(const char *fmt, ...)
{
    __builtin_va_list ap;

    __builtin_va_start(ap, fmt);
    log_v("[n64] FATAL: ", fmt, ap, 1);
    __builtin_va_end(ap);
}

/* libultra's assert() (debug builds of the library) */
void __assert(const char *expr, const char *file, int line)
{
    port_fatal("assertion failed: %s (%s:%d)", expr, file, line);
}

/* ---- maths the game takes from the C library -------------------------------------------- *
 * (compiled with -fno-math-errno, so the builtins are instructions, not calls back here) */
float sqrtf(float x)
{
    return __builtin_sqrtf(x);
}

double sqrt(double x)
{
    return __builtin_sqrt(x);
}

float fabsf(float x)
{
    return __builtin_fabsf(x);
}

/* ---- memory ------------------------------------------------------------------------------ */
#ifndef PORT_ARENA_SIZE
#define PORT_ARENA_SIZE (24u << 20)
#endif

static u8 sArena[PORT_ARENA_SIZE] __attribute__((aligned(16)));
static u32 sArenaUsed;

void *port_arena_alloc(unsigned long long size, unsigned long long align)
{
    u32 start = (sArenaUsed + (u32)align - 1) & ~((u32)align - 1);

    if (start + size > PORT_ARENA_SIZE)
    {
        port_fatal("arena exhausted (%u bytes more needed)", (u32)size);
        return NULL;
    }
    sArenaUsed = start + (u32)size;
    port_memset(&sArena[start], 0, size);
    return &sArena[start];
}

void port_arena_reset(void)
{
    sArenaUsed = 0;
}

unsigned long long port_arena_used(void)
{
    return sArenaUsed;
}

void port_memcpy(void *dst, const void *src, unsigned long long size)
{
    __builtin_memmove(dst, src, (u32)size);
}

void port_memset(void *dst, int value, unsigned long long size)
{
    __builtin_memset(dst, value, (u32)size);
}

/* ---- overlays (as in runtime/host/port_host.c, with the copies in the arena) --------------- */
static char **sOverlayData;
static char **sOverlayBss;

static char *overlay_copy(const char *start, const char *end)
{
    char *copy = port_arena_alloc((unsigned long long)(end - start) + 1, 16);

    port_memcpy(copy, start, (unsigned long long)(end - start));
    return copy;
}

static void overlay_restore(unsigned int index)
{
    const PortOverlay *ovl = &gPortOverlays[index];

    port_memcpy(ovl->data_start, sOverlayData[index], (unsigned long long)(ovl->data_end - ovl->data_start));
    port_memcpy(ovl->bss_start, sOverlayBss[index], (unsigned long long)(ovl->bss_end - ovl->bss_start));
}

void port_overlays_reset_all(void)
{
    unsigned int i;

    if (sOverlayData == NULL)
    {
        sOverlayData = port_arena_alloc(sizeof(*sOverlayData) * (gPortOverlaysNum + 1), 4);
        sOverlayBss = port_arena_alloc(sizeof(*sOverlayBss) * (gPortOverlaysNum + 1), 4);
        for (i = 0; i < gPortOverlaysNum; i++)
        {
            sOverlayData[i] = overlay_copy(gPortOverlays[i].data_start, gPortOverlays[i].data_end);
            sOverlayBss[i] = overlay_copy(gPortOverlays[i].bss_start, gPortOverlays[i].bss_end);
        }
        return;
    }
    for (i = 0; i < gPortOverlaysNum; i++)
    {
        overlay_restore(i);
    }
}

void port_overlay_load(unsigned long long rom_start)
{
    unsigned int i;

    for (i = 0; i < gPortOverlaysNum; i++)
    {
        if ((u32)gPortOverlays[i].rom_start == (u32)rom_start)
        {
            if (sOverlayData != NULL)
            {
                overlay_restore(i);
            }
            return;
        }
    }
    port_log("overlay at ROM 0x%X is not in the overlay table", (u32)rom_start);
}

/* ---- files and ROM ----------------------------------------------------------------------------- */
unsigned int port_file_read(const char *path, void *data, unsigned int size)
{
    return n64w_file_read(path, data, size);
}

int port_file_write(const char *path, const void *data, unsigned int size)
{
    return n64w_file_write(path, data, size);
}

int port_rom_load(const char *path)
{
    return n64w_rom_loaded(); /* the host loads it before booting the module */
}

void port_rom_read(unsigned int offset, void *dst, unsigned int size)
{
    n64w_rom_read(offset, dst, size);
}

const unsigned char *port_rom_view(unsigned int offset, unsigned int size)
{
    static u8 *sView;
    static u32 sViewSize;

    if (size > sViewSize)
    {
        sViewSize = (size + 0xFFFF) & ~0xFFFFu;
        sView = port_arena_alloc(sViewSize, 16);
    }
    n64w_rom_read(offset, sView, size);
    return sView;
}

unsigned int port_rom_size(void)
{
    return n64w_rom_size();
}

/* ---- coroutines ---------------------------------------------------------------------------------
 * The host switches native stacks (n64w_host.c); each coroutine also needs its own part of
 * the module's stack (the wasm shadow stack), carved out of the arena here. */
#define N64W_SHADOW_STACK (128u << 10)

struct PortCoro
{
    u32 handle;
};

PortCoro *port_coro_create(void (*entry)(void *), void *arg, unsigned long long stack_size)
{
    PortCoro *coro = port_arena_alloc(sizeof(*coro), 4);
    u8 *stack = port_arena_alloc(N64W_SHADOW_STACK, 16);

    coro->handle = n64w_coro_create((u32)entry, (u32)arg, (u32)(stack + N64W_SHADOW_STACK));
    return coro;
}

/* Entry of every coroutine, called by the host on the coroutine's own native stack. */
__attribute__((export_name("n64w_coro_entry"))) void n64w_coro_entry(u32 entry, u32 arg)
{
    ((void (*)(void *))entry)((void *)arg);
}

void port_coro_destroy(PortCoro *coro)
{
    if (coro != NULL)
    {
        n64w_coro_destroy(coro->handle); /* (its shadow stack is not reused) */
    }
}

void port_coro_resume(PortCoro *coro)
{
    n64w_coro_resume(coro->handle);
}

void port_coro_yield(void)
{
    n64w_coro_yield();
}

int port_coro_finished(PortCoro *coro)
{
    return n64w_coro_finished(coro->handle);
}

/* ---- the embedding API the host calls (n64w_host.c wraps these) ------------------------------ */
__attribute__((export_name("n64w_set_pad"))) void n64w_set_pad(s32 port, s32 buttons, s32 stick_x, s32 stick_y, s32 connected)
{
    PortPad pad;

    pad.buttons = (unsigned short)buttons;
    pad.stick_x = (signed char)stick_x;
    pad.stick_y = (signed char)stick_y;
    pad.connected = (unsigned char)connected;
    n64_set_pad(port, &pad);
}

static char sSavePath[512];

/* The host writes the path into this buffer (bytes), then calls n64w_set_save_path. */
__attribute__((export_name("n64w_path_buffer"))) char *n64w_path_buffer(void)
{
    return sSavePath;
}

__attribute__((export_name("n64w_set_save_path"))) void n64w_set_save_path(void)
{
    n64_set_save_path(sSavePath);
}

__attribute__((export_name("n64w_boot"))) s32 n64w_boot(void)
{
    return n64_boot("");
}

__attribute__((export_name("n64w_run_frame"))) void n64w_run_frame(void)
{
    n64_run_frame();
}

__attribute__((export_name("n64w_is_running"))) s32 n64w_is_running(void)
{
    return n64_is_running();
}

static s32 sOutA, sOutB;

__attribute__((export_name("n64w_framebuffer"))) u32 n64w_framebuffer(void)
{
    int w, h;
    const unsigned char *fb = n64_framebuffer(&w, &h);

    sOutA = w;
    sOutB = h;
    return (u32)fb;
}

__attribute__((export_name("n64w_audio"))) u32 n64w_audio(void)
{
    int frames, rate;
    const short *pcm = n64_audio(&frames, &rate);

    sOutA = frames;
    sOutB = rate;
    return (u32)pcm;
}

__attribute__((export_name("n64w_out_a"))) s32 n64w_out_a(void)
{
    return sOutA;
}

__attribute__((export_name("n64w_out_b"))) s32 n64w_out_b(void)
{
    return sOutB;
}

__attribute__((export_name("n64w_draws_to_screen"))) s32 n64w_draws_to_screen(void)
{
    return n64_draws_to_screen();
}

/* ---- script bridge (port_bridge.h) for the host ------------------------------------------------
 * Strings and arrays go through n64w_scratch (bytes: names, text) and n64w_args (ints, which
 * the host reads and writes as big-endian words). */
static char sScratch[1024];
static int sArgs[PB_MAX_ARGS];
static double sValue;

char *n64w_scratch(void)
{
    return sScratch;
}

int *n64w_args(void)
{
    return sArgs;
}

double *n64w_value(void)
{
    return &sValue;
}

u32 n64w_arena_used(void)
{
    return (u32)port_arena_used();
}

s32 n64w_bridge_var_count(void)
{
    return n64_bridge_var_count();
}

s32 n64w_bridge_request_count(void)
{
    return n64_bridge_request_count();
}

/* -> the entry's PortBridgeVar / PortBridgeRequest (guest memory) */
u32 n64w_bridge_var(s32 index)
{
    return (u32)n64_bridge_var(index);
}

u32 n64w_bridge_request_info(s32 index)
{
    return (u32)n64_bridge_request_info(index);
}

/* name in the scratch buffer; text comes back there too, the number in n64w_value */
s32 n64w_bridge_get(s32 index)
{
    char name[128];
    s32 i;

    for (i = 0; i < (s32)sizeof(name) - 1 && sScratch[i] != '\0'; i++)
    {
        name[i] = sScratch[i];
    }
    name[i] = '\0';
    return n64_bridge_get(name, index, &sValue, sScratch, sizeof(sScratch));
}

s32 n64w_bridge_request(s32 nargs)
{
    return n64_bridge_request(sScratch, sArgs, nargs);
}

/* -> 1 with the result in n64w_out_a */
s32 n64w_bridge_result(s32 id)
{
    int result = 0;
    s32 got = n64_bridge_result(id, &result);

    sOutA = result;
    return got;
}

/* -> 1 with the name in the scratch buffer, the arguments in n64w_args and their count in n64w_out_a */
s32 n64w_bridge_poll_event(void)
{
    int nargs = 0;
    s32 got = n64_bridge_poll_event(sScratch, sizeof(sScratch), sArgs, PB_MAX_ARGS, &nargs);

    sOutA = nargs;
    return got;
}
