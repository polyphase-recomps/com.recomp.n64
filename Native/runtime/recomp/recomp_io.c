/*
 * Recomp mode: devices (PI, SI, VI, AI, RSP / RDP tasks, timers, caches) for recompiled game
 * code, and the embedding API (n64_boot, n64_run_frame, ...) the editor addon and host runner
 * use - the same API as the decomp build, so either can drive a game.
 *
 * Semantics follow the decomp port's port_io.c: DMA and RSP tasks complete at once and post
 * their completion messages, and the clock is derived from the frame counter, so runs are
 * deterministic.
 */
#include "recomp_rt.h"

#include <port_bridge.h>
#include <port_host.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* port_gfx.c, compiled in host mode on this RDRAM (PORT_RSP_HOST + PORT_RSP_RECOMP) */
void port_gfx_run_guest_task(unsigned int task);
void port_gfx_set_framebuffer(void *fb);
/* port_audio.c / port_audio_abi1.c, the same way */
void port_audio_run_guest_task(unsigned int task);
void port_audio_set_abi1(int abi1);
void port_audio_submit_guest(unsigned int addr, unsigned int size);
void port_audio_set_rate(unsigned int rate);
unsigned int port_audio_ai_length(void);
void port_audio_frame_begin(void);

/* libultra (2.0) */
#define OS_EVENT_SP 4
#define OS_EVENT_SI 5
#define OS_EVENT_AI 6
#define OS_EVENT_VI 7
#define OS_EVENT_PI 8
#define OS_EVENT_DP 9
#define OS_READ 0
#define DEVICE_TYPE_CART 0
#define DEVICE_TYPE_SRAM 3
#define M_GFXTASK 1
#define M_AUDTASK 2
#define MAXCONTROLLERS 4
#define CONT_TYPE_NORMAL 0x0005
#define CONT_NO_RESPONSE_ERROR 0x8
#define PFS_ERR_NOPACK 1

/* Runtime data the game needs in RDRAM (the PI handle osCartRomInit returns) lives above the
 * 4 MB the game is told it has (osMemSize). */
#define RT_AREA 0x80780000u
#define RT_CART_HANDLE (RT_AREA + 0x000)

unsigned int gPortFrameCount;
extern int gPortVerbose;
extern unsigned int gPortGfxTraceFrame;
static int sRunning;

/* Decomp-mode tables port_host.c refers to; recomp mode loads overlays by DMA instead. */
const PortOverlay gPortOverlays[1];
const unsigned int gPortOverlaysNum = 0;

/* ---- saves: SRAM (32 KB), flushed once a frame when written ------------------------------- */
#define SRAM_SIZE 0x8000
static uint8_t sSram[SRAM_SIZE];
static int sSramDirty;
static char sSavePath[1024];

void n64_set_save_path(const char *path)
{
    snprintf(sSavePath, sizeof(sSavePath), "%s", path ? path : "");
}

static void save_load(void)
{
    memset(sSram, 0, sizeof(sSram));
    if (sSavePath[0] != 0)
    {
        port_file_read(sSavePath, sSram, sizeof(sSram));
    }
}

static void save_flush(void)
{
    if (sSramDirty && sSavePath[0] != 0)
    {
        port_file_write(sSavePath, sSram, sizeof(sSram));
    }
    sSramDirty = 0;
}

/* ---- PI ------------------------------------------------------------------------------------- */
void osCartRomInit_recomp(uint8_t *rdram, recomp_context *ctx)
{
    RECOMP_STAT();
    uint32_t h = RT_CART_HANDLE;

    rw_u32(h + 0, 0);
    rw_u8(h + 4, DEVICE_TYPE_CART);
    rw_u32(h + 12, 0xB0000000u); /* baseAddress */
    RRET(ctx, h);
}

void osCreatePiManager_recomp(uint8_t *rdram, recomp_context *ctx)
{
    RECOMP_STAT();
}

/* osEPiStartDma(OSPiHandle *handle, OSIoMesg *mb, s32 direction) */
void osEPiStartDma_recomp(uint8_t *rdram, recomp_context *ctx)
{
    RECOMP_STAT();
    uint32_t handle = RA0(ctx), mb = RA1(ctx);
    int direction = (int)RA2(ctx);
    uint32_t dram = rr_u32(mb + 8), dev = rr_u32(mb + 12), size = rr_u32(mb + 16);
    uint32_t offset = dev & 0x0FFFFFFF;

    if (rr_u8(handle + 4) == DEVICE_TYPE_SRAM)
    {
        offset &= SRAM_SIZE - 1;
        if (offset + size > SRAM_SIZE)
        {
            recomp_fatal("SRAM access out of range (0x%X + 0x%X)", offset, size);
        }
        if (direction == OS_READ)
        {
            recomp_mem_write_be(dram, sSram + offset, size);
        }
        else
        {
            recomp_mem_read_be(dram, sSram + offset, size);
            sSramDirty = 1;
        }
    }
    else if (direction == OS_READ)
    {
        if (offset >= port_rom_size())
        {
            recomp_fatal("ROM read out of range (0x%X)", offset);
        }
        if (offset + size > port_rom_size())
        {
            size = port_rom_size() - offset;
        }
        recomp_mem_write_be(dram, port_rom_view(offset, size), size);
        recomp_sections_on_dma(offset, dram, size);
    }
    recomp_os_send(rr_u32(mb + 4), mb); /* hdr.retQueue */
    RRET(ctx, 0);
}

/* ---- SI: controllers ------------------------------------------------------------------------ */
static PortPad sPads[MAXCONTROLLERS];

void n64_set_pad(int port, const PortPad *pad)
{
    if (port >= 0 && port < MAXCONTROLLERS)
    {
        sPads[port] = *pad;
    }
}

static void cont_status(uint32_t status)
{
    int i;

    for (i = 0; i < MAXCONTROLLERS; i++)
    {
        rw_u16(status + 4 * i, CONT_TYPE_NORMAL);
        rw_u8(status + 4 * i + 2, 0);
        rw_u8(status + 4 * i + 3, sPads[i].connected ? 0 : CONT_NO_RESPONSE_ERROR);
    }
}

/* osContInit(OSMesgQueue *mq, u8 *bitpattern, OSContStatus *status) */
void osContInit_recomp(uint8_t *rdram, recomp_context *ctx)
{
    RECOMP_STAT();
    uint8_t bits = 0;
    int i;

    for (i = 0; i < MAXCONTROLLERS; i++)
    {
        if (sPads[i].connected) bits |= (uint8_t)(1 << i);
    }
    rw_u8(RA1(ctx), bits);
    cont_status(RA2(ctx));
    RRET(ctx, 0);
}

void osContStartQuery_recomp(uint8_t *rdram, recomp_context *ctx)
{
    RECOMP_STAT();
    recomp_os_send(RA0(ctx), 0);
    RRET(ctx, 0);
}

void osContGetQuery_recomp(uint8_t *rdram, recomp_context *ctx)
{
    RECOMP_STAT();
    cont_status(RA0(ctx));
}

void osContStartReadData_recomp(uint8_t *rdram, recomp_context *ctx)
{
    RECOMP_STAT();
    recomp_os_send(RA0(ctx), 0);
    RRET(ctx, 0);
}

/* osContGetReadData(OSContPad *pads): 6 bytes each */
void osContGetReadData_recomp(uint8_t *rdram, recomp_context *ctx)
{
    RECOMP_STAT();
    uint32_t pads = RA0(ctx);
    int i;

    for (i = 0; i < MAXCONTROLLERS; i++)
    {
        uint32_t p = pads + 6 * i;

        rw_u16(p, sPads[i].buttons);
        rw_u8(p + 2, (uint8_t)sPads[i].stick_x);
        rw_u8(p + 3, (uint8_t)sPads[i].stick_y);
        rw_u8(p + 4, sPads[i].connected ? 0 : CONT_NO_RESPONSE_ERROR);
    }
}

/* Controller Pak address CRC (5 bits over the 11-bit block address) */
void __osContAddressCrc_recomp(uint8_t *rdram, recomp_context *ctx)
{
    RECOMP_STAT();
    uint32_t addr = RA0(ctx) & 0xFFFF, crc = 0, bit;
    int i;

    for (bit = 0x400; bit != 0; bit >>= 1)
    {
        crc <<= 1;
        if (addr & bit)
        {
            crc = (crc & 0x20) ? crc ^ 0x14 : crc + 1;
        }
        else if (crc & 0x20)
        {
            crc ^= 0x15;
        }
    }
    for (i = 0; i < 5; i++)
    {
        crc <<= 1;
        if (crc & 0x20) crc ^= 0x15;
    }
    RRET(ctx, crc & 0x1F);
}

/* No Rumble Pak */
void osMotorInit_recomp(uint8_t *rdram, recomp_context *ctx)
{
    RECOMP_STAT();
    RRET(ctx, PFS_ERR_NOPACK);
}

void __osMotorAccess_recomp(uint8_t *rdram, recomp_context *ctx)
{
    RECOMP_STAT();
    RRET(ctx, PFS_ERR_NOPACK);
}

/* ---- VI ------------------------------------------------------------------------------------- */
static uint32_t sViCurrentFb, sViNextFb;

void osCreateViManager_recomp(uint8_t *rdram, recomp_context *ctx) { RECOMP_STAT(); }
void osViSetMode_recomp(uint8_t *rdram, recomp_context *ctx) { RECOMP_STAT(); }
void osViSetYScale_recomp(uint8_t *rdram, recomp_context *ctx) { RECOMP_STAT(); }
void osViBlack_recomp(uint8_t *rdram, recomp_context *ctx) { RECOMP_STAT(); }

void osViSwapBuffer_recomp(uint8_t *rdram, recomp_context *ctx)
{
    RECOMP_STAT();
    sViNextFb = RA0(ctx);
    port_gfx_set_framebuffer(NULL); /* the frame drawn so far is the one to show */
}

void osViGetCurrentFramebuffer_recomp(uint8_t *rdram, recomp_context *ctx)
{
    RECOMP_STAT();
    RRET(ctx, sViCurrentFb);
}

void osViGetNextFramebuffer_recomp(uint8_t *rdram, recomp_context *ctx)
{
    RECOMP_STAT();
    RRET(ctx, sViNextFb);
}

/* ---- time ------------------------------------------------------------------------------------ */
#define COUNTS_PER_FRAME (46875000 / 60)
#define COUNTS_PER_READ 1543

static uint64_t sClockReads;
static int64_t sTimeBase;

static int sClockTraceFrames; /* N64_CLOCK_TRACE=N: log every clock read of the first N frames */

static uint64_t clock_now(const void *caller)
{
    if ((int)gPortFrameCount < sClockTraceFrames)
    {
        /* the caller as an offset from n64_boot, for addr2line on the host binary */
        port_log("clock: frame %u read %llu from n64_boot%+lld", gPortFrameCount, (unsigned long long)sClockReads,
                 (long long)((const char *)caller - (const char *)(uintptr_t)&n64_boot));
    }
    return (uint64_t)gPortFrameCount * COUNTS_PER_FRAME + (sClockReads++ * COUNTS_PER_READ);
}

void osGetCount_recomp(uint8_t *rdram, recomp_context *ctx)
{
    RECOMP_STAT();
    RRET(ctx, (uint32_t)clock_now(__builtin_return_address(0)));
}

void osGetTime_recomp(uint8_t *rdram, recomp_context *ctx)
{
    RECOMP_STAT();
    RRET64(ctx, clock_now(__builtin_return_address(0)) + (uint64_t)sTimeBase);
}

void osSetTime_recomp(uint8_t *rdram, recomp_context *ctx)
{
    RECOMP_STAT();
    uint64_t t = ((uint64_t)(uint32_t)ctx->r4 << 32) | (uint32_t)ctx->r5;

    sTimeBase = (int64_t)(t - clock_now(__builtin_return_address(0)));
}

/* Timers fire at frame boundaries against the frame clock (deterministic). */
#define MAX_TIMERS 32

static struct
{
    uint32_t timer; /* OSTimer in RDRAM, 0 = free slot */
    uint64_t due, interval;
    uint32_t mq, msg;
} sTimers[MAX_TIMERS];

/* osSetTimer(OSTimer *t, OSTime countdown, OSTime interval, OSMesgQueue *mq, OSMesg msg):
 * countdown in a2/a3 (a 64-bit argument starts at an even register), the rest on the stack */
void osSetTimer_recomp(uint8_t *rdram, recomp_context *ctx)
{
    RECOMP_STAT();
    uint32_t timer = RA0(ctx);
    uint64_t countdown = ((uint64_t)RA2(ctx) << 32) | RA3(ctx);
    uint64_t interval = ((uint64_t)RSTACK(ctx, 0) << 32) | RSTACK(ctx, 1);
    int i, slot = -1;

    for (i = 0; i < MAX_TIMERS; i++)
    {
        if (sTimers[i].timer == timer || (slot < 0 && sTimers[i].timer == 0))
        {
            slot = i;
            if (sTimers[i].timer == timer) break;
        }
    }
    if (slot < 0)
    {
        recomp_fatal("more than %d timers", MAX_TIMERS);
    }
    sTimers[slot].timer = timer;
    sTimers[slot].due = clock_now(__builtin_return_address(0)) + (countdown ? countdown : interval);
    sTimers[slot].interval = interval;
    sTimers[slot].mq = RSTACK(ctx, 2);
    sTimers[slot].msg = RSTACK(ctx, 3);
    rw_u32(timer + 8, (uint32_t)(interval >> 32));
    rw_u32(timer + 12, (uint32_t)interval);
    rw_u32(timer + 24, sTimers[slot].mq);
    rw_u32(timer + 28, sTimers[slot].msg);
    RRET(ctx, 0);
}

void osStopTimer_recomp(uint8_t *rdram, recomp_context *ctx)
{
    RECOMP_STAT();
    int i;

    for (i = 0; i < MAX_TIMERS; i++)
    {
        if (sTimers[i].timer == RA0(ctx))
        {
            sTimers[i].timer = 0;
            RRET(ctx, 0);
            return;
        }
    }
    RRET(ctx, -1);
}

static void timers_fire(void)
{
    uint64_t now = (uint64_t)gPortFrameCount * COUNTS_PER_FRAME;
    int i;

    for (i = 0; i < MAX_TIMERS; i++)
    {
        if (sTimers[i].timer != 0 && sTimers[i].due <= now)
        {
            recomp_os_send(sTimers[i].mq, sTimers[i].msg);
            if (sTimers[i].interval != 0)
            {
                sTimers[i].due += sTimers[i].interval;
            }
            else
            {
                sTimers[i].timer = 0;
            }
        }
    }
}

/* ---- RSP / RDP tasks --------------------------------------------------------------------------- */
void osSpTaskLoad_recomp(uint8_t *rdram, recomp_context *ctx) { RECOMP_STAT(); }

void osSpTaskStartGo_recomp(uint8_t *rdram, recomp_context *ctx)
{
    RECOMP_STAT();
    uint32_t task = RA0(ctx);

    /* tasks complete at once; their interrupts arrive as queued messages */
    if (rr_u32(task) == M_AUDTASK)
    {
        port_audio_run_guest_task(task);
        recomp_os_post_event(OS_EVENT_SP);
    }
    else
    {
        port_gfx_run_guest_task(task);
        recomp_os_post_event(OS_EVENT_SP);
        recomp_os_post_event(OS_EVENT_DP);
    }
}

void osSpTaskYield_recomp(uint8_t *rdram, recomp_context *ctx) { RECOMP_STAT(); }

void osSpTaskYielded_recomp(uint8_t *rdram, recomp_context *ctx)
{
    RECOMP_STAT();
    RRET(ctx, 0);
}

void __osSpSetPc_recomp(uint8_t *rdram, recomp_context *ctx)
{
    RECOMP_STAT();
    RRET(ctx, 0);
}

void osDpSetNextBuffer_recomp(uint8_t *rdram, recomp_context *ctx)
{
    RECOMP_STAT();
    RRET(ctx, 0);
}

/* ---- AI ------------------------------------------------------------------------------------------ */
static uint32_t sAudioRate = 32000;

void osAiSetFrequency_recomp(uint8_t *rdram, recomp_context *ctx)
{
    RECOMP_STAT();
    uint32_t frequency = RA0(ctx);
    uint32_t dac = (uint32_t)(48681812.0f / (float)frequency + 0.5f);

    sAudioRate = dac ? 48681812u / dac : frequency;
    port_audio_set_rate(sAudioRate);
    RRET(ctx, sAudioRate);
}

/* AI_LEN_REG, which games also read directly (IO_READ): bytes left in the playing buffer */
static void ai_registers_update(void)
{
    rw_u32(0xA4500004u, port_audio_ai_length());
}

void osAiSetNextBuffer_recomp(uint8_t *rdram, recomp_context *ctx)
{
    RECOMP_STAT();
    uint32_t buffer = RA0(ctx), size = RA1(ctx);

    port_audio_submit_guest(buffer & 0x1FFFFFFFu, size & 0x3FFF8u);
    ai_registers_update();
    RRET(ctx, 0);
}

void osAiGetLength_recomp(uint8_t *rdram, recomp_context *ctx)
{
    RECOMP_STAT();
    RRET(ctx, port_audio_ai_length());
}

void osAiGetStatus_recomp(uint8_t *rdram, recomp_context *ctx)
{
    RECOMP_STAT();
    RRET(ctx, 0);
}

/* ---- caches, TLB, debug ----------------------------------------------------------------------- */
void osInvalDCache_recomp(uint8_t *rdram, recomp_context *ctx) { RECOMP_STAT(); }
void osInvalICache_recomp(uint8_t *rdram, recomp_context *ctx) { RECOMP_STAT(); }
void osWritebackDCache_recomp(uint8_t *rdram, recomp_context *ctx) { RECOMP_STAT(); }
void osWritebackDCacheAll_recomp(uint8_t *rdram, recomp_context *ctx) { RECOMP_STAT(); }
void __osSetWatchLo_recomp(uint8_t *rdram, recomp_context *ctx) { RECOMP_STAT(); }

void osVirtualToPhysical_recomp(uint8_t *rdram, recomp_context *ctx)
{
    RECOMP_STAT();
    RRET(ctx, RA0(ctx) & 0x1FFFFFFF);
}

/* ---- script bridge: recompiled games publish no variables (yet) -------------------------------- */
int n64_bridge_var_count(void) { return 0; }
const PortBridgeVar *n64_bridge_var(int index) { return NULL; }
int n64_bridge_request_count(void) { return 0; }
const PortBridgeRequest *n64_bridge_request_info(int index) { return NULL; }
int n64_bridge_get(const char *name, int index, double *value, char *text, unsigned text_cap) { return 0; }
int n64_bridge_request(const char *name, const int *args, int nargs) { return 0; }
int n64_bridge_result(int id, int *result) { return 0; }
int n64_bridge_poll_event(char *name, unsigned name_cap, int *args, int max_args, int *nargs) { return 0; }

/* ---- embedding API ------------------------------------------------------------------------------ */
/* Recomp mode runs the game's code from the ROM it was recompiled from and DMAs its overlays
 * (code and data) out of it, so it needs the whole ROM: an asset pack (the decomp build's game
 * data, no code) is refused, so a player can fall back to the ROM it is pointed at. */
static int is_whole_rom(const char *path)
{
    unsigned char magic[4] = {0, 0, 0, 0};
    FILE *f = fopen(path, "rb");

    if (f != NULL)
    {
        fread(magic, 1, sizeof(magic), f);
        fclose(f);
    }
    if (magic[0] == 0x80 && magic[1] == 0x37 && magic[2] == 0x12 && magic[3] == 0x40)
    {
        return 1;
    }
    port_log("'%s' is not a big-endian (.z64) N64 ROM; recompiled games boot from the whole ROM", path);
    return 0;
}

int n64_boot(const char *rom_path)
{
#ifdef RECOMP_DEFAULT_ROM
    /* development builds: the ROM the game was recompiled from (N64RECOMP_ROM) */
    if (!is_whole_rom(rom_path) && is_whole_rom(RECOMP_DEFAULT_ROM))
    {
        port_log("booting the ROM the game was recompiled from: %s", RECOMP_DEFAULT_ROM);
        rom_path = RECOMP_DEFAULT_ROM;
    }
#endif
    if (!is_whole_rom(rom_path) || !port_rom_load(rom_path) || !recomp_mem_init())
    {
        return 0;
    }
    recomp_os_reset();
    recomp_mem_clear();
    gPortFrameCount = 0;
    sClockReads = 0;
    sTimeBase = 0;
    sViCurrentFb = sViNextFb = 0;
    memset(sTimers, 0, sizeof(sTimers));

    /* IPL3: the first megabyte after the header goes to 0x80000400, and the boot globals */
    recomp_mem_write_be(0x80000400u, port_rom_view(0x1000, 0x100000), 0x100000);
    rw_u32(0x80000300u, 1);           /* osTvType: NTSC */
    rw_u32(0x80000304u, 0);           /* osRomType: cartridge */
    rw_u32(0x80000308u, 0xB0000000u); /* osRomBase */
    rw_u32(0x8000030Cu, 0);           /* osResetType: cold */
    rw_u32(0x80000310u, 6102);        /* osCicId */
    rw_u32(0x80000314u, 0);           /* osVersion */
    rw_u32(0x80000318u, 0x400000u);   /* osMemSize: 4 MB */
    recomp_sections_init();
    recomp_sections_on_dma(0x1000, 0x80000400u, 0x100000);

    gPortVerbose = port_env_int("N64_VERBOSE"); /* bring-up aid: per-frame renderer statistics */
    /* the audio microcode: n_aspMain unless the game uses libultra's own aspMain (ABI 1) */
    port_audio_set_abi1(port_env_int("N64_AUDIO_ABI1"));
    sClockTraceFrames = port_env_int("N64_CLOCK_TRACE");
    gPortGfxTraceFrame = (unsigned int)port_env_int("SSB64_GFX_TRACE"); /* log one frame's display lists */
    save_load();
    /* controller 1 is plugged in at power-on, as in the decomp build (some games, SSB among
     * them, stop at a "no controller" screen until reset otherwise) */
    memset(sPads, 0, sizeof(sPads));
    sPads[0].connected = 1;
    recomp_os_boot();
    recomp_os_run();
    sRunning = 1;
    return 1;
}

int n64_is_running(void)
{
    return sRunning;
}

void n64_run_frame(void)
{
    if (!sRunning || port_faulted())
    {
        return;
    }
    gPortFrameCount++;
    save_flush();
    sViCurrentFb = sViNextFb;
    timers_fire();
    port_audio_frame_begin();
    ai_registers_update();
    recomp_os_post_vi_retrace();
    recomp_os_run();
}

void n64_shutdown(void)
{
    recomp_stats_print();
    save_flush();
    recomp_os_reset();
    sRunning = 0;
}


