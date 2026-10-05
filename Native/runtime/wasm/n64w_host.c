/*
 * Host half of the wasm build: instantiates the wasm2c-translated game module, provides its
 * imports (n64w_imports.h) and offers the usual embedding API (port_host.h: n64_boot,
 * n64_run_frame, ...) on top of the module's exports, so hosts and the Polyphase addons use
 * a wasm game exactly like a natively compiled one.
 *
 * Compiled once per game with N64W_MODULE set to the module name given to wasm2c
 * (tools/wasm_to_c.py), with the generated <name>_guest.h on the include path.
 *
 * Native stacks for the game's threads come from the platform backend's coroutines
 * (port_host_<platform>.c); each coroutine also has its own part of the module's stack,
 * so the module's stack pointer global is switched along with the native stack.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port_host.h"
#include "port_bridge.h"
#include "port_host_plat.h"
#include "wasm-rt.h"
#include "n64w.h"

#define N64W_CAT2(a, b) a##b
#define N64W_CAT(a, b) N64W_CAT2(a, b)
#define N64W_STR2(a) #a
#define N64W_STR(a) N64W_STR2(a)
#include N64W_STR(N64W_CAT(N64W_MODULE, _guest.h))

typedef N64W_CAT(w2c_, N64W_MODULE) Instance;
#define GUEST(fn) N64W_CAT(N64W_CAT(N64W_CAT(w2c_, N64W_MODULE), _), fn)
#define STACK_POINTER w2c_0x5F_stack_pointer

struct w2c_env
{
    int unused;
};

static Instance sInstance;
static struct w2c_env sEnv;
static int sInstantiated;
static int sRomLoaded;
static unsigned char *sMem;

static void *guest_ptr(uint32_t addr)
{
    return sMem + N64W_OFFSET(addr);
}

/* ---- traps ---------------------------------------------------------------------------------- */
void host_log(const char *fmt, ...)
{
    char buf[512];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    port_log("%s", buf);
}

void host_crashed(void)
{
    port_plat_abort();
}

void n64w_report_trap(const char *what)
{
    port_log("game module trapped: %s", what);
    if (gPortContainFaults)
    {
        port_plat_coro_abandon(); /* back to whoever resumed the game thread */
    }
    port_plat_abort(); /* (a crash handler prints the guest call stack) */
}

/* ---- imports ---------------------------------------------------------------------------------- */
void w2c_env_n64w_log(struct w2c_env *env, uint32_t line)
{
    port_log("%s", (const char *)guest_ptr(line) + 6); /* (the guest adds "[n64] ") */
}

void w2c_env_n64w_fatal(struct w2c_env *env, uint32_t line)
{
    port_fatal("%s", (const char *)guest_ptr(line) + 13); /* (after "[n64] FATAL: ") */
}

uint32_t w2c_env_n64w_faulted(struct w2c_env *env)
{
    return (uint32_t)port_faulted();
}

uint32_t w2c_env_n64w_env_int(struct w2c_env *env, uint32_t name)
{
    return (uint32_t)port_env_int((const char *)guest_ptr(name));
}

uint32_t w2c_env_n64w_file_read(struct w2c_env *env, uint32_t path, uint32_t data, uint32_t size)
{
    return port_file_read((const char *)guest_ptr(path), guest_ptr(data), size);
}

uint32_t w2c_env_n64w_file_write(struct w2c_env *env, uint32_t path, uint32_t data, uint32_t size)
{
    return (uint32_t)port_file_write((const char *)guest_ptr(path), guest_ptr(data), size);
}

uint32_t w2c_env_n64w_rom_loaded(struct w2c_env *env)
{
    return (uint32_t)sRomLoaded;
}

void w2c_env_n64w_rom_read(struct w2c_env *env, uint32_t offset, uint32_t dst, uint32_t size)
{
    port_rom_read(offset, guest_ptr(dst), size);
}

uint32_t w2c_env_n64w_rom_size(struct w2c_env *env)
{
    return port_rom_size();
}

/* ---- coroutines --------------------------------------------------------------------------------- */
#define N64W_CORO_MAX 64

typedef struct GuestCoro
{
    PortCoro *native;
    uint32_t entry, arg;
    uint32_t sp; /* module stack pointer while not running */
} GuestCoro;

static GuestCoro sCoros[N64W_CORO_MAX + 1]; /* handle 0 is unused */
static GuestCoro *sRunning;

static void coro_main(void *param)
{
    GuestCoro *coro = param;

    sInstance.STACK_POINTER = coro->sp;
    GUEST(n64w_coro_entry)(&sInstance, coro->entry, coro->arg);
}

uint32_t w2c_env_n64w_coro_create(struct w2c_env *env, uint32_t entry, uint32_t arg, uint32_t stack_top)
{
    uint32_t i;

    for (i = 1; i <= N64W_CORO_MAX; i++)
    {
        if (sCoros[i].native == NULL)
        {
            sCoros[i].entry = entry;
            sCoros[i].arg = arg;
            sCoros[i].sp = stack_top;
            sCoros[i].native = port_coro_create(coro_main, &sCoros[i], 1024 * 1024);
            return i;
        }
    }
    port_fatal("more than %d guest coroutines", N64W_CORO_MAX);
    return 0;
}

void w2c_env_n64w_coro_destroy(struct w2c_env *env, uint32_t handle)
{
    if (handle != 0 && handle <= N64W_CORO_MAX && sCoros[handle].native != NULL)
    {
        port_coro_destroy(sCoros[handle].native);
        sCoros[handle].native = NULL;
    }
}

void w2c_env_n64w_coro_resume(struct w2c_env *env, uint32_t handle)
{
    GuestCoro *coro = &sCoros[handle];
    GuestCoro *previous = sRunning;
    uint32_t sp = sInstance.STACK_POINTER;

    sRunning = coro;
    port_coro_resume(coro->native);
    sRunning = previous;
    sInstance.STACK_POINTER = sp;
}

void w2c_env_n64w_coro_yield(struct w2c_env *env)
{
    GuestCoro *coro = sRunning;

    coro->sp = sInstance.STACK_POINTER;
    port_coro_yield();
    sInstance.STACK_POINTER = coro->sp;
}

uint32_t w2c_env_n64w_coro_finished(struct w2c_env *env, uint32_t handle)
{
    return (uint32_t)port_coro_finished(sCoros[handle].native);
}

/* ---- embedding API --------------------------------------------------------------------------- */
static void instantiate(void)
{
    if (sInstantiated)
    {
        return;
    }
    wasm_rt_init();
    N64W_CAT(N64W_CAT(wasm2c_, N64W_MODULE), _instantiate)(&sInstance, &sEnv);
    sMem = sInstance.w2c_memory.data;
    GUEST(0x5F_wasm_call_ctors)(&sInstance); /* byte-swaps the initialised data (be_fixup.c) */
    sInstantiated = 1;
}

static char sPendingSavePath[512];

void n64_set_save_path(const char *path)
{
    snprintf(sPendingSavePath, sizeof(sPendingSavePath), "%s", path ? path : "");
    if (sInstantiated)
    {
        char *dst = guest_ptr(GUEST(n64w_path_buffer)(&sInstance));

        memcpy(dst, sPendingSavePath, strlen(sPendingSavePath) + 1);
        GUEST(n64w_set_save_path)(&sInstance);
    }
}

int n64_boot(const char *rom_path)
{
    if (!port_rom_load(rom_path))
    {
        return 0;
    }
    sRomLoaded = 1;
    instantiate();
    n64_set_save_path(sPendingSavePath);
    return (int)GUEST(n64w_boot)(&sInstance);
}

int n64_is_running(void)
{
    return sInstantiated && (int)GUEST(n64w_is_running)(&sInstance);
}

void n64_set_pad(int port, const PortPad *pad)
{
    if (sInstantiated)
    {
        GUEST(n64w_set_pad)(&sInstance, (uint32_t)port, pad->buttons, (uint32_t)(int)pad->stick_x,
                            (uint32_t)(int)pad->stick_y, pad->connected);
    }
}

void n64_run_frame(void)
{
    if (sInstantiated)
    {
        GUEST(n64w_run_frame)(&sInstance);
    }
}

void n64_shutdown(void)
{
}

const unsigned char *n64_framebuffer(int *width, int *height)
{
    uint32_t fb;

    *width = *height = 0;
    if (!sInstantiated)
    {
        return NULL;
    }
    fb = GUEST(n64w_framebuffer)(&sInstance);
    *width = (int)GUEST(n64w_out_a)(&sInstance);
    *height = (int)GUEST(n64w_out_b)(&sInstance);
    return fb ? guest_ptr(fb) : NULL; /* RGBA8 bytes: no byte order to fix */
}

int n64_draws_to_screen(void)
{
    return 0;
}

const short *n64_audio(int *frames, int *sample_rate)
{
    static short *sSamples;
    static int sCapacity;
    const unsigned char *src;
    uint32_t addr;
    int i, n;

    *frames = 0;
    *sample_rate = 32000;
    if (!sInstantiated)
    {
        return NULL;
    }
    addr = GUEST(n64w_audio)(&sInstance);
    n = (int)GUEST(n64w_out_a)(&sInstance);
    *sample_rate = (int)GUEST(n64w_out_b)(&sInstance);
    if (addr == 0 || n <= 0)
    {
        return NULL;
    }
    if (n * 2 > sCapacity)
    {
        sCapacity = n * 2;
        sSamples = realloc(sSamples, sizeof(short) * (size_t)sCapacity);
    }
    /* the module's samples are big-endian, like everything in its memory */
    src = guest_ptr(addr);
    for (i = 0; i < n * 2; i++)
    {
        sSamples[i] = (short)((src[i * 2] << 8) | src[i * 2 + 1]);
    }
    *frames = n;
    return sSamples;
}

unsigned long long port_arena_used(void)
{
    return sInstantiated ? GUEST(n64w_arena_used)(&sInstance) : 0;
}

/* ---- script bridge ------------------------------------------------------------------------------
 * The tables live in the module (big-endian, guest pointers); copies with host strings are
 * made on first use. Tables are published once, before the first frame. */
static uint32_t be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void put_be32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static const char *guest_str(uint32_t addr)
{
    return addr ? (const char *)guest_ptr(addr) : "";
}

#define N64W_BRIDGE_MAX 256
static PortBridgeVar sVarCopies[N64W_BRIDGE_MAX];
static PortBridgeRequest sReqCopies[N64W_BRIDGE_MAX];

int n64_bridge_var_count(void)
{
    return sInstantiated ? (int)GUEST(n64w_bridge_var_count)(&sInstance) : 0;
}

const PortBridgeVar *n64_bridge_var(int index)
{
    const unsigned char *v;
    PortBridgeVar *copy;
    uint32_t addr;

    if (!sInstantiated || index < 0 || index >= N64W_BRIDGE_MAX)
    {
        return NULL;
    }
    addr = GUEST(n64w_bridge_var)(&sInstance, (uint32_t)index);
    if (addr == 0)
    {
        return NULL;
    }
    v = guest_ptr(addr); /* { name, addr, type, count, stride, help } as big-endian words */
    copy = &sVarCopies[index];
    copy->name = guest_str(be32(v));
    copy->addr = NULL; /* (in the module's memory: read through n64_bridge_get) */
    copy->type = (int)be32(v + 8);
    copy->count = (int)be32(v + 12);
    copy->stride = (int)be32(v + 16);
    copy->help = guest_str(be32(v + 20));
    return copy;
}

int n64_bridge_request_count(void)
{
    return sInstantiated ? (int)GUEST(n64w_bridge_request_count)(&sInstance) : 0;
}

const PortBridgeRequest *n64_bridge_request_info(int index)
{
    const unsigned char *r;
    PortBridgeRequest *copy;
    uint32_t addr;

    if (!sInstantiated || index < 0 || index >= N64W_BRIDGE_MAX)
    {
        return NULL;
    }
    addr = GUEST(n64w_bridge_request_info)(&sInstance, (uint32_t)index);
    if (addr == 0)
    {
        return NULL;
    }
    r = guest_ptr(addr); /* { name, fn, help } */
    copy = &sReqCopies[index];
    copy->name = guest_str(be32(r));
    copy->fn = NULL;
    copy->help = guest_str(be32(r + 8));
    return copy;
}

static char *scratch(void)
{
    return guest_ptr(GUEST(n64w_scratch)(&sInstance));
}

int n64_bridge_get(const char *name, int index, double *value, char *text, unsigned text_cap)
{
    int kind;

    if (!sInstantiated)
    {
        return 0;
    }
    snprintf(scratch(), 1024, "%s", name);
    kind = (int)GUEST(n64w_bridge_get)(&sInstance, (uint32_t)index);
    if (kind == 1)
    {
        const unsigned char *d = guest_ptr(GUEST(n64w_value)(&sInstance));
        uint64_t bits = ((uint64_t)be32(d) << 32) | be32(d + 4);

        memcpy(value, &bits, sizeof(*value));
    }
    else if (kind == 2 && text != NULL && text_cap > 0)
    {
        snprintf(text, text_cap, "%s", scratch());
    }
    return kind;
}

int n64_bridge_request(const char *name, const int *args, int nargs)
{
    unsigned char *a;
    int i;

    if (!sInstantiated)
    {
        return 0;
    }
    if (nargs > PB_MAX_ARGS)
    {
        nargs = PB_MAX_ARGS;
    }
    snprintf(scratch(), 1024, "%s", name);
    a = guest_ptr(GUEST(n64w_args)(&sInstance));
    for (i = 0; i < nargs; i++)
    {
        put_be32(a + i * 4, (uint32_t)args[i]);
    }
    return (int)GUEST(n64w_bridge_request)(&sInstance, (uint32_t)nargs);
}

int n64_bridge_result(int id, int *result)
{
    if (!sInstantiated || !GUEST(n64w_bridge_result)(&sInstance, (uint32_t)id))
    {
        return 0;
    }
    *result = (int)GUEST(n64w_out_a)(&sInstance);
    return 1;
}

int n64_bridge_poll_event(char *name, unsigned name_cap, int *args, int max_args, int *nargs)
{
    const unsigned char *a;
    int i, n;

    if (!sInstantiated || !GUEST(n64w_bridge_poll_event)(&sInstance))
    {
        return 0;
    }
    snprintf(name, name_cap, "%s", scratch());
    n = (int)GUEST(n64w_out_a)(&sInstance);
    if (n > max_args)
    {
        n = max_args;
    }
    a = guest_ptr(GUEST(n64w_args)(&sInstance));
    for (i = 0; i < n; i++)
    {
        args[i] = (int)be32(a + i * 4);
    }
    *nargs = n;
    return 1;
}
