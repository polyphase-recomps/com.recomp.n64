/*
 * libultra device layer replacement: PI (cartridge ROM / SRAM), VI, AI, SP
 * tasks, controllers, plus the handful of kernel odds and ends the game links
 * against. Nothing here touches hardware; it talks to the host through
 * port_host.h.
 */
#include <port_types.h>
#include <PR/os.h>
#include <PR/rcp.h>
#include <PR/sptask.h>
#include <PR/mbi.h>
#include <port_host.h>
#include "port_guest.h"

u32 gPortFrameCount;

/* ---- RCP register reads -------------------------------------------------- */
u32 port_io_read(u32 addr)
{
    switch (addr)
    {
    case SP_IMEM_START:
        return 6103; /* what the boot code leaves behind; checked by syMainSetImemStatus */
    case SP_DMEM_START:
        return 0xFFFFFFFF;
    case AI_LEN_REG:
        return port_audio_ai_length();
    default:
        return 0;
    }
}

void port_io_write(u32 addr, u32 data)
{
}

/* ---- PI: cartridge ROM and SRAM -------------------------------------------- */
#define PORT_SRAM_SIZE 0x8000

static OSPiHandle sCartHandle;
static u8 sSram[PORT_SRAM_SIZE];
static char sSavePath[512];
static sb32 sSramDirty;

void n64_set_save_path(const char *path)
{
    u32 i = 0;

    if (path != NULL)
    {
        for (; path[i] != '\0' && i < sizeof(sSavePath) - 1; i++)
        {
            sSavePath[i] = path[i];
        }
    }
    sSavePath[i] = '\0';
}

/* Called at boot: the save RAM starts out as the saved image, or blank. */
void port_io_load_save(void)
{
    port_memset(sSram, 0, sizeof(sSram));
    sSramDirty = FALSE;
    if (sSavePath[0] != '\0' && port_file_read(sSavePath, sSram, sizeof(sSram)) != 0)
    {
        port_log("save: loaded '%s'", sSavePath);
    }
}

/* Called once a frame: the game writes its save in several pieces, so flush after the fact. */
static void port_io_flush_save(void)
{
    if (sSramDirty && sSavePath[0] != '\0')
    {
        if (!port_file_write(sSavePath, sSram, sizeof(sSram)))
        {
            port_log("save: cannot write '%s'", sSavePath);
        }
    }
    sSramDirty = FALSE;
}

OSPiHandle *osCartRomInit(void)
{
    sCartHandle.type = DEVICE_TYPE_CART;
    sCartHandle.baseAddress = PHYS_TO_K1(PI_DOM1_ADDR2);
    sCartHandle.domain = PI_DOMAIN1;
    return &sCartHandle;
}

s32 osEPiLinkHandle(OSPiHandle *handle)
{
    return 0;
}

void osCreatePiManager(OSPri pri, OSMesgQueue *cmd_queue, OSMesg *cmd_buf, s32 cmd_count)
{
}

u8 *port_sram(void)
{
    return sSram;
}

s32 osEPiStartDma(OSPiHandle *handle, OSIoMesg *mb, s32 direction)
{
    gPortProgress[2]++;
    u32 offset = mb->devAddr & 0x0FFFFFFF;

    if (handle->type == DEVICE_TYPE_SRAM)
    {
        offset &= (PORT_SRAM_SIZE - 1);
        if (offset + mb->size > PORT_SRAM_SIZE)
        {
            port_fatal("SRAM access out of range (0x%X + 0x%X)", offset, mb->size);
        }
        if (direction == OS_READ)
        {
            port_memcpy(mb->dramAddr, sSram + offset, mb->size);
        }
        else
        {
            port_memcpy(sSram + offset, mb->dramAddr, mb->size);
            sSramDirty = TRUE;
        }
    }
    else if (direction == OS_READ)
    {
        u32 size = mb->size;

        if (offset >= port_rom_size())
        {
            port_fatal("ROM read out of range (0x%X)", offset);
        }
        if (offset + size > port_rom_size())
        {
            size = port_rom_size() - offset;
        }
        port_rom_read(offset, mb->dramAddr, size);
    }
    /* Completion is immediate; the caller picks the message up on its next osRecvMesg. */
    osSendMesg(mb->hdr.retQueue, (OSMesg)mb, OS_MESG_NOBLOCK);
    return 0;
}

/* Same as osEPiStartDma on the cartridge (the PI manager's own handle). */
s32 osPiStartDma(OSIoMesg *mb, s32 priority, s32 direction, u32 devAddr, void *dramAddr, u32 size, OSMesgQueue *mq)
{
    mb->hdr.pri = priority;
    mb->hdr.retQueue = mq;
    mb->devAddr = devAddr;
    mb->dramAddr = dramAddr;
    mb->size = size;
    return osEPiStartDma(osCartRomInit(), mb, direction);
}

/* 64DD drive ROM: there is no drive; reads through this handle go to the cartridge ROM. */
OSPiHandle *osDriveRomInit(void)
{
    static OSPiHandle sDriveHandle;

    sDriveHandle.type = DEVICE_TYPE_CART;
    return &sDriveHandle;
}

/* ---- EEPROM (kept in the save image, after nothing else uses it: games have one kind) ---- */
#ifndef PORT_EEPROM_TYPE
#define PORT_EEPROM_TYPE EEPROM_TYPE_4K
#endif

s32 osEepromProbe(OSMesgQueue *mq)
{
    return PORT_EEPROM_TYPE;
}

s32 osEepromLongRead(OSMesgQueue *mq, u8 address, u8 *buffer, int nbytes)
{
    u32 offset = (u32)address * EEPROM_BLOCK_SIZE;

    if (offset + (u32)nbytes > EEP16K_MAXBLOCKS * EEPROM_BLOCK_SIZE)
    {
        return -1;
    }
    port_memcpy(buffer, sSram + offset, nbytes);
    return 0;
}

s32 osEepromLongWrite(OSMesgQueue *mq, u8 address, u8 *buffer, int nbytes)
{
    u32 offset = (u32)address * EEPROM_BLOCK_SIZE;

    if (offset + (u32)nbytes > EEP16K_MAXBLOCKS * EEPROM_BLOCK_SIZE)
    {
        return -1;
    }
    port_memcpy(sSram + offset, buffer, nbytes);
    sSramDirty = TRUE;
    return 0;
}

s32 osEepromRead(OSMesgQueue *mq, u8 address, u8 *buffer)
{
    return osEepromLongRead(mq, address, buffer, EEPROM_BLOCK_SIZE);
}

s32 osEepromWrite(OSMesgQueue *mq, u8 address, u8 *buffer)
{
    return osEepromLongWrite(mq, address, buffer, EEPROM_BLOCK_SIZE);
}

/* ---- Controller Pak: none inserted ------------------------------------------------- */
s32 osPfsInitPak(OSMesgQueue *mq, OSPfs *pfs, int channel) { return PFS_ERR_NOPACK; }
s32 osPfsNumFiles(OSPfs *pfs, s32 *max_files, s32 *files_used) { return PFS_ERR_NOPACK; }
s32 osPfsFreeBlocks(OSPfs *pfs, s32 *bytes_not_used) { return PFS_ERR_NOPACK; }
s32 osPfsFileState(OSPfs *pfs, s32 file_no, OSPfsState *state) { return PFS_ERR_NOPACK; }
s32 osPfsFindFile(OSPfs *pfs, u16 company_code, u32 game_code, u8 *game_name, u8 *ext_name, s32 *file_no)
{
    return PFS_ERR_NOPACK;
}
s32 osPfsAllocateFile(OSPfs *pfs, u16 company_code, u32 game_code, u8 *game_name, u8 *ext_name, int file_size,
                      s32 *file_no)
{
    return PFS_ERR_NOPACK;
}
s32 osPfsDeleteFile(OSPfs *pfs, u16 company_code, u32 game_code, u8 *game_name, u8 *ext_name)
{
    return PFS_ERR_NOPACK;
}
s32 osPfsReadWriteFile(OSPfs *pfs, s32 file_no, u8 flag, int offset, int size_in_bytes, u8 *data_buffer)
{
    return PFS_ERR_NOPACK;
}

/* ---- VI ------------------------------------------------------------------------- */
/* The modes only matter to the VI hardware; osViSetMode takes them and ignores them. */
OSViMode osViModeTable[56];

void osViSetSpecialFeatures(u32 func)
{
}

static void *sViCurrentFb;
static void *sViNextFb;

void osCreateViManager(OSPri pri)
{
}

void osViSetMode(OSViMode *mode)
{
}

void osViSetYScale(f32 scale)
{
}

void osViBlack(u8 active)
{
}

void osViSwapBuffer(void *fb)
{
    sViNextFb = fb;
    port_gfx_set_framebuffer(fb);
}

void *osViGetCurrentFramebuffer(void)
{
    return sViCurrentFb;
}

void *osViGetNextFramebuffer(void)
{
    return sViNextFb;
}

/* Called by the host once per video frame before the scheduler runs. */
void port_io_frame_begin(void)
{
    gPortFrameCount++;
    port_io_flush_save();
    sViCurrentFb = sViNextFb;
}

/* ---- timing ------------------------------------------------------------------------
 * Derived from the frame counter so runs are deterministic. The game also uses the low
 * bits of the clock as entropy (and loops until it gets a different value), so every
 * read advances the clock a little, as real time would. */
#define PORT_COUNTS_PER_FRAME (46875000 / 60)
#define PORT_COUNTS_PER_READ 1543

static u64 sClockReads;
static s32 sClockTraceFrames = -1; /* N64_CLOCK_TRACE=N: log every clock read of the first N frames */

static u64 port_clock(const void *caller)
{
    if (sClockTraceFrames < 0)
    {
        sClockTraceFrames = port_env_int("N64_CLOCK_TRACE");
    }
    if ((s32)gPortFrameCount < sClockTraceFrames)
    {
        port_log("clock: frame %u read %llu from %p", gPortFrameCount, (unsigned long long)sClockReads, caller);
    }
    return (u64)gPortFrameCount * PORT_COUNTS_PER_FRAME + (sClockReads++ * PORT_COUNTS_PER_READ);
}

#if defined(__wasm__)
#define PORT_CALLER NULL
#else
#define PORT_CALLER __builtin_return_address(0)
#endif

u32 osGetCount(void)
{
    return (u32)port_clock(PORT_CALLER);
}

OSTime osGetTime(void)
{
    return port_clock(PORT_CALLER);
}

/* ---- SP tasks ---------------------------------------------------------------------- */
/* Microcode images: only their addresses are used, to tell tasks apart (port_gfx_run_task). */
long long int gspF3DEX2_fifoTextStart[1], gspF3DEX2_fifoDataStart[1];
long long int gspS2DEX_fifoTextStart[1], gspS2DEX_fifoDataStart[1];
long long int n_aspMainTextStart[1], n_aspMainDataStart[1];
long long int aspMainTextStart[1], aspMainDataStart[1];
long long int rspbootTextStart[1], rspbootTextEnd[1];

void osSpTaskLoad(OSTask *task)
{
}

void osSpTaskStartGo(OSTask *task)
{
    /* Profiling switch N64_PROF_SKIP (environment, bit mask): 1 skips graphics tasks, 2 audio
     * tasks, 4 triangle rasterizing, 8 fill rectangles (port_gfx.c). What remains shows where
     * the time goes on a slow target. */
    static int sSkip = -1;

    if (sSkip < 0)
    {
        sSkip = port_env_int("N64_PROF_SKIP");
    }
    /* Tasks complete synchronously; their interrupts are delivered as queued messages. */
    if (task->t.type == M_AUDTASK)
    {
        if (!(sSkip & 2))
        {
            port_audio_run_task(task);
        }
        port_os_post_event(OS_EVENT_SP);
    }
    else
    {
        if (!(sSkip & 1))
        {
            port_gfx_run_task(task);
        }
        port_os_post_event(OS_EVENT_SP);
        port_os_post_event(OS_EVENT_DP);
    }
}

void osSpTaskYield(void)
{
}

OSYieldResult osSpTaskYielded(OSTask *task)
{
    return 0;
}

s32 osDpSetNextBuffer(void *buf, u64 size)
{
    return 0;
}

/* ---- AI ---------------------------------------------------------------------------- */
s32 osAiSetFrequency(u32 frequency)
{
    /* The DAC rate the hardware ends up with from the game's request (NTSC video clock). */
    u32 dacRate = (u32)(48681812.0F / frequency + 0.5F);
    u32 rate = (dacRate != 0) ? 48681812 / dacRate : frequency;

    port_audio_set_rate(rate);
    return (s32)rate;
}

u32 osAiGetLength(void)
{
    return port_audio_ai_length();
}

s32 osAiSetNextBuffer(void *buf, u32 size)
{
    port_audio_submit(buf, size);
    return 0;
}

/* ---- controllers ---------------------------------------------------------------------- */
static PortPad sPads[MAXCONTROLLERS];

void n64_set_pad(int port, const PortPad *pad)
{
    if (port >= 0 && port < MAXCONTROLLERS)
    {
        sPads[port] = *pad;
    }
}

static void port_cont_status(OSContStatus *status)
{
    s32 i;

    for (i = 0; i < MAXCONTROLLERS; i++)
    {
        status[i].type = CONT_TYPE_NORMAL;
        status[i].status = 0;
        status[i].errno = sPads[i].connected ? 0 : CONT_NO_RESPONSE_ERROR;
    }
}

s32 osContInit(OSMesgQueue *mq, u8 *bitpattern, OSContStatus *status)
{
    s32 i;

    *bitpattern = 0;
    for (i = 0; i < MAXCONTROLLERS; i++)
    {
        if (sPads[i].connected)
        {
            *bitpattern |= 1 << i;
        }
    }
    port_cont_status(status);
    return 0;
}

s32 osContStartQuery(OSMesgQueue *mq)
{
    osSendMesg(mq, NULL, OS_MESG_NOBLOCK);
    return 0;
}

void osContGetQuery(OSContStatus *status)
{
    port_cont_status(status);
}

s32 osContStartReadData(OSMesgQueue *mq)
{
    osSendMesg(mq, NULL, OS_MESG_NOBLOCK);
    return 0;
}

void osContGetReadData(OSContPad *pads)
{
    s32 i;

    for (i = 0; i < MAXCONTROLLERS; i++)
    {
        pads[i].button = sPads[i].buttons;
        pads[i].stick_x = sPads[i].stick_x;
        pads[i].stick_y = sPads[i].stick_y;
        pads[i].errno = sPads[i].connected ? 0 : CONT_NO_RESPONSE_ERROR;
    }
}

/* No Rumble Pak: report "no pak" so the game skips motor commands. */
s32 osMotorInit(OSMesgQueue *mq, OSPfs *pfs, int channel)
{
    return PFS_ERR_NOPACK;
}

s32 __osMotorAccess(OSPfs *pfs, s32 start)
{
    return PFS_ERR_NOPACK;
}

/* ---- libm ------------------------------------------------------------------------------- */
extern float sinf(float);
extern float cosf(float);

/* For games that do not link libultra's own (weak: those that do keep theirs). */
__attribute__((weak)) f32 __sinf(f32 x)
{
    return sinf(x);
}

__attribute__((weak)) f32 __cosf(f32 x)
{
    return cosf(x);
}

/* ---- kernel odds and ends ------------------------------------------------------------- */
f32 __libm_qnan_f = 0.0F / 0.0F;

void bzero(void *ptr, int size)
{
    port_memset(ptr, 0, size);
}

void bcopy(const void *src, void *dst, int size)
{
    port_memcpy(dst, src, size);
}

void osInvalDCache(void *addr, s32 size)
{
}

void osInvalICache(void *addr, s32 size)
{
}

void osWritebackDCache(void *addr, s32 size)
{
}

void osWritebackDCacheAll(void)
{
}

u32 osVirtualToPhysical(void *addr)
{
    /* Audio commands carry 24-bit addresses: for audio heap memory that is a heap offset. */
    if (port_audio_is_heap(addr))
    {
        return port_audio_addr(addr);
    }
    return (u32)(uintptr_t)addr;
}

void __osSetWatchLo(u32 value)
{
}

s32 osAfterPreNMI(void)
{
    return 0;
}
