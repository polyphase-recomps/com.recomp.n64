/*
 * Recomp mode core: the memory window, code sections (overlays) and function lookup, the hooks
 * N64Recomp's output calls (break, jump table errors, cop0), and the 64-bit arithmetic helpers
 * of the game's C library. See recomp_rt.h.
 */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* memfd_create */
#endif
#include "recomp_rt.h"
#include "librecomp/sections.h"

#include <port_host.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif RECOMP_WINDOW
#include <sys/mman.h>
#include <unistd.h>
#endif

uint8_t *gRecompRdram;

void recomp_fatal(const char *fmt, ...)
{
    char text[512];
    va_list args;

    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    port_fatal("recomp: %s", text);
}

/* ---- statistics --------------------------------------------------------------------------- */
#define MAX_STATS 128

static struct
{
    const char *name;
    unsigned long long count;
} sStats[MAX_STATS];
static int sStatsNum, sStatsOn = -1;

void recomp_stat(const char *name)
{
    int i;

    if (sStatsOn < 0)
    {
        sStatsOn = port_env_int("N64_RECOMP_STATS");
    }
    if (!sStatsOn)
    {
        return;
    }
    for (i = 0; i < sStatsNum; i++)
    {
        if (sStats[i].name == name)
        {
            sStats[i].count++;
            return;
        }
    }
    if (sStatsNum < MAX_STATS)
    {
        sStats[sStatsNum].name = name;
        sStats[sStatsNum++].count = 1;
    }
}

void recomp_stats_print(void)
{
    int i;

    for (i = 0; i < sStatsNum; i++)
    {
        port_log("recomp stats: %-32s %llu", sStats[i].name, sStats[i].count);
    }
}

/* ---- memory ----------------------------------------------------------------------------- */
#if RECOMP_WINDOW
/* Offsets in the window: RDRAM, its KSEG1 mirror, and the RCP register pages (0xA4000000-). */
#define KSEG1_OFFSET 0x20000000u
#define RCP_OFFSET 0x24000000u
#define RCP_SIZE 0x00900000u

#if defined(_WIN32)
int recomp_mem_init(void)
{
    if (gRecompRdram != NULL)
    {
        return 1;
    }
    /* Reserve the window, then make RDRAM and the RCP pages usable. The KSEG1 mirror is a
     * second view of the same pages (a file mapping), so writes through either are the same. */
    uint8_t *base = VirtualAlloc(NULL, RECOMP_WINDOW_SIZE, MEM_RESERVE, PAGE_NOACCESS);
    if (base == NULL)
    {
        return 0;
    }
    VirtualFree(base, 0, MEM_RELEASE);
    HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, RECOMP_RDRAM_SIZE, NULL);
    if (map == NULL)
    {
        return 0;
    }
    /* The released range is free again: map the two views and the RCP pages into it (another
     * thread could take it in between; this runs before the game starts). */
    if (MapViewOfFileEx(map, FILE_MAP_ALL_ACCESS, 0, 0, RECOMP_RDRAM_SIZE, base) != base ||
        MapViewOfFileEx(map, FILE_MAP_ALL_ACCESS, 0, 0, RECOMP_RDRAM_SIZE, base + KSEG1_OFFSET) != base + KSEG1_OFFSET ||
        VirtualAlloc(base + RCP_OFFSET, RCP_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != base + RCP_OFFSET)
    {
        return 0;
    }
    gRecompRdram = base;
    return 1;
}
#else
int recomp_mem_init(void)
{
    if (gRecompRdram != NULL)
    {
        return 1;
    }
    uint8_t *base = mmap(NULL, RECOMP_WINDOW_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (base == MAP_FAILED)
    {
        return 0;
    }
    int fd = memfd_create("n64-rdram", 0);
    if (fd < 0 || ftruncate(fd, RECOMP_RDRAM_SIZE) != 0)
    {
        return 0;
    }
    if (mmap(base, RECOMP_RDRAM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED ||
        mmap(base + KSEG1_OFFSET, RECOMP_RDRAM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED ||
        mmap(base + RCP_OFFSET, RCP_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED)
    {
        return 0;
    }
    close(fd);
    gRecompRdram = base;
    return 1;
}
#endif

void recomp_mem_clear(void)
{
    memset(gRecompRdram, 0, RECOMP_RDRAM_SIZE);
    memset(gRecompRdram + RCP_OFFSET, 0, RCP_SIZE);
}

#else /* !RECOMP_WINDOW: masked addressing (32-bit hosts) */

/* Everything outside RDRAM (RCP / PI / SI registers, mostly) lands in scratch slots: 1 KB per
 * 256 KB of physical space, so the register blocks (SP 0x0404, DP 0x0410, MI 0x0430, VI 0x0440,
 * AI 0x0450, PI 0x0460, RI 0x0470, SI 0x0480) each have their own. */
#define IO_SLOT_SHIFT 18
#define IO_SLOTS 64
#define IO_SLOT_SIZE 0x400u

static uint8_t sIo[IO_SLOTS * IO_SLOT_SIZE] __attribute__((aligned(8)));
static uint8_t *sRdramBlock;

uint8_t *recomp_io_ptr(uint32_t phys)
{
    return sIo + ((phys >> IO_SLOT_SHIFT) & (IO_SLOTS - 1)) * IO_SLOT_SIZE + (phys & (IO_SLOT_SIZE - 1));
}

int recomp_mem_init(void)
{
    if (gRecompRdram != NULL)
    {
        return 1;
    }
    sRdramBlock = malloc(RECOMP_RDRAM_SIZE + 64);
    if (sRdramBlock == NULL)
    {
        return 0;
    }
    gRecompRdram = (uint8_t *)(((uintptr_t)sRdramBlock + 63) & ~(uintptr_t)63);
    return 1;
}

void recomp_mem_clear(void)
{
    memset(gRecompRdram, 0, RECOMP_RDRAM_SIZE);
    memset(sIo, 0, sizeof(sIo));
}
#endif

void recomp_mem_write_be(uint32_t addr, const uint8_t *src, uint32_t size)
{
    uint32_t i = 0;

    /* whole words: a big-endian word is one native word */
    while (i < size && ((addr + i) & 3) != 0)
    {
        rw_u8(addr + i, src[i]);
        i++;
    }
    for (; i + 4 <= size; i += 4)
    {
        rw_u32(addr + i, ((uint32_t)src[i] << 24) | ((uint32_t)src[i + 1] << 16) | ((uint32_t)src[i + 2] << 8) | src[i + 3]);
    }
    for (; i < size; i++)
    {
        rw_u8(addr + i, src[i]);
    }
}

void recomp_mem_read_be(uint32_t addr, uint8_t *dst, uint32_t size)
{
    uint32_t i;

    for (i = 0; i < size; i++)
    {
        dst[i] = rr_u8(addr + i);
    }
}

/* ---- sections ------------------------------------------------------------------------------
 * Overlays share addresses: a call by address goes to the function of the section whose code
 * was last DMA'd there. */
#define MAX_SECTIONS 512
#define LOOKUP_CACHE 4096

static const SectionTableEntry *sSections;
static size_t sSectionsNum;
static uint8_t sLoaded[MAX_SECTIONS];
static struct
{
    uint32_t vram;
    recomp_func_t *func;
} sCache[LOOKUP_CACHE];

static int32_t sSectionAddresses[MAX_SECTIONS];
int32_t *section_addresses = sSectionAddresses;

void recomp_sections_init(void)
{
    size_t i;

    sSections = recomp_section_table(&sSectionsNum);
    if (sSectionsNum > MAX_SECTIONS)
    {
        recomp_fatal("%u sections, more than %u", (unsigned)sSectionsNum, MAX_SECTIONS);
    }
    memset(sLoaded, 0, sizeof(sLoaded));
    memset(sCache, 0, sizeof(sCache));
    /* by the recompiler's section index (the table leaves out sections without code) */
    for (i = 0; i < sSectionsNum; i++)
    {
        if (sSections[i].index >= MAX_SECTIONS)
        {
            recomp_fatal("section index %u, more than %u", (unsigned)sSections[i].index, MAX_SECTIONS);
        }
        sSectionAddresses[sSections[i].index] = (int32_t)sSections[i].ram_addr;
    }
}

void recomp_sections_on_dma(uint32_t rom, uint32_t vram, uint32_t size)
{
    size_t i, j;
    int changed = 0;

    for (i = 0; i < sSectionsNum; i++)
    {
        const SectionTableEntry *s = &sSections[i];

        /* the section's code start came along, and landed where the section runs */
        if (s->rom_addr < rom || s->rom_addr >= rom + size || vram + (s->rom_addr - rom) != s->ram_addr)
        {
            continue;
        }
        if (!sLoaded[i])
        {
            /* whatever was loaded over the same addresses is gone */
            for (j = 0; j < sSectionsNum; j++)
            {
                const SectionTableEntry *o = &sSections[j];

                if (j != i && sLoaded[j] && o->ram_addr < s->ram_addr + s->size && s->ram_addr < o->ram_addr + o->size)
                {
                    sLoaded[j] = 0;
                }
            }
            sLoaded[i] = 1;
            changed = 1;
            if (port_env_int("N64_RECOMP_TRACE"))
            {
                port_log("recomp: frame %u: section %u loaded at 0x%08X (rom 0x%X)", gPortFrameCount, (unsigned)i, s->ram_addr, s->rom_addr);
            }
        }
    }
    if (changed)
    {
        memset(sCache, 0, sizeof(sCache));
    }
}

static recomp_func_t *find_function(uint32_t vram)
{
    size_t i;

    for (i = 0; i < sSectionsNum; i++)
    {
        const SectionTableEntry *s = &sSections[i];
        size_t lo = 0, hi;

        if (!sLoaded[i] || vram < s->ram_addr || vram >= s->ram_addr + s->size)
        {
            continue;
        }
        hi = s->num_funcs;
        while (lo < hi)
        {
            size_t mid = (lo + hi) / 2;
            uint32_t at = s->ram_addr + s->funcs[mid].offset;

            if (at == vram)
            {
                return s->funcs[mid].func;
            }
            if (at < vram) lo = mid + 1;
            else hi = mid;
        }
        recomp_fatal("call to 0x%08X: inside section %u but not a function start", vram, (unsigned)i);
    }
    return NULL;
}

recomp_func_t *get_function(int32_t vram_signed)
{
    uint32_t vram = (uint32_t)vram_signed;
    uint32_t slot = (vram >> 2) & (LOOKUP_CACHE - 1);
    recomp_func_t *func;

    if (sCache[slot].vram == vram && sCache[slot].func != NULL)
    {
        return sCache[slot].func;
    }
    func = find_function(vram);
    if (func == NULL)
    {
        recomp_fatal("call to 0x%08X: no loaded code there", vram);
    }
    sCache[slot].vram = vram;
    sCache[slot].func = func;
    return func;
}

/* ---- hooks of the generated code ----------------------------------------------------------- */
void switch_error(const char *func, uint32_t vram, uint32_t jtbl)
{
    recomp_fatal("jump table 0x%08X in %s (0x%08X) went out of range", jtbl, func, vram);
}

void do_break(uint32_t vram)
{
    recomp_fatal("break instruction at 0x%08X", vram);
}

void recomp_syscall_handler(uint8_t *rdram, recomp_context *ctx, int32_t instruction_vram)
{
    recomp_fatal("syscall at 0x%08X", (uint32_t)instruction_vram);
}

gpr cop0_status_read(recomp_context *ctx)
{
    return (gpr)(int32_t)ctx->status_reg;
}

void cop0_status_write(recomp_context *ctx, gpr value)
{
    ctx->status_reg = (uint32_t)value;
}

/* ---- 64-bit arithmetic (IDO's libc helpers; o32 passes a long long in a register pair,
 * high word first) ------------------------------------------------------------------------- */
static int64_t arg64(recomp_context *ctx, int first)
{
    gpr hi = first ? ctx->r4 : ctx->r6, lo = first ? ctx->r5 : ctx->r7;
    return (int64_t)(((uint64_t)(uint32_t)hi << 32) | (uint32_t)lo);
}

void __ll_div_recomp(uint8_t *rdram, recomp_context *ctx)
{
    int64_t a = arg64(ctx, 1), b = arg64(ctx, 0);
    RRET64(ctx, (b == 0) ? 0 : (a == INT64_MIN && b == -1) ? a : a / b);
}

void __ull_div_recomp(uint8_t *rdram, recomp_context *ctx)
{
    uint64_t a = (uint64_t)arg64(ctx, 1), b = (uint64_t)arg64(ctx, 0);
    RRET64(ctx, (b == 0) ? 0 : a / b);
}

void __ll_rem_recomp(uint8_t *rdram, recomp_context *ctx)
{
    int64_t a = arg64(ctx, 1), b = arg64(ctx, 0);
    RRET64(ctx, (b == 0 || (a == INT64_MIN && b == -1)) ? 0 : a % b);
}

void __ull_rem_recomp(uint8_t *rdram, recomp_context *ctx)
{
    uint64_t a = (uint64_t)arg64(ctx, 1), b = (uint64_t)arg64(ctx, 0);
    RRET64(ctx, (b == 0) ? a : a % b);
}

void __ll_mul_recomp(uint8_t *rdram, recomp_context *ctx)
{
    RRET64(ctx, (uint64_t)arg64(ctx, 1) * (uint64_t)arg64(ctx, 0));
}

void __ll_lshift_recomp(uint8_t *rdram, recomp_context *ctx)
{
    RRET64(ctx, (uint64_t)arg64(ctx, 1) << (arg64(ctx, 0) & 63));
}

void __ull_rshift_recomp(uint8_t *rdram, recomp_context *ctx)
{
    RRET64(ctx, (uint64_t)arg64(ctx, 1) >> (arg64(ctx, 0) & 63));
}

void __ll_to_f_recomp(uint8_t *rdram, recomp_context *ctx)
{
    ctx->f0.fl = (float)arg64(ctx, 1);
}

void __ull_to_f_recomp(uint8_t *rdram, recomp_context *ctx)
{
    ctx->f0.fl = (float)(uint64_t)arg64(ctx, 1);
}

void __f_to_ll_recomp(uint8_t *rdram, recomp_context *ctx)
{
    RRET64(ctx, (int64_t)ctx->f12.fl);
}

/* double results go in f0/f1, which the 32-bit FPU mode keeps as one fpr */
void __ull_to_d_recomp(uint8_t *rdram, recomp_context *ctx)
{
    ctx->f0.d = (double)(uint64_t)arg64(ctx, 1);
}

/* ---- FPU control ----------------------------------------------------------------------------- */
void __osSetFpcCsr_recomp(uint8_t *rdram, recomp_context *ctx)
{
    uint32_t old = get_cop1_cs();

    set_cop1_cs(RA0(ctx));
    RRET(ctx, old);
}
