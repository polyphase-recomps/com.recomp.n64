/*
 * Recomp mode: the runtime for game code recompiled from the ROM by N64Recomp.
 *
 * N64Recomp turns every MIPS function into a C function `void f(uint8_t *rdram, recomp_context
 * *ctx)` that keeps the CPU registers in ctx and reaches memory through recomp.h's MEM_*
 * macros. On a little-endian host those store RDRAM as native 32-bit words (byte addresses ^3,
 * halfwords ^2): this file's accessors follow the same layout, so runtime code and recompiled
 * code see the same memory.
 *
 * The parts of libultra that touch hardware (threads, message queues, PI / SI / VI / AI, RSP
 * tasks, timers) are not recompiled: N64Recomp calls `<name>_recomp` instead, and those are
 * implemented here on the game's own structures in RDRAM (recomp_os.c, recomp_io.c).
 */
#ifndef RECOMP_RT_H
#define RECOMP_RT_H

#include "recomp.h" /* N64Recomp's generated-code contract (MIT, ThirdParty/N64Recomp/include) */
#include "recomp_ultra.h" /* what the runtime implements for the generated code */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- memory ------------------------------------------------------------------------------
 * `rdram` points at KSEG0 0x80000000. The window behind it covers 0x80000000-0xBFFFFFFF:
 * RDRAM, its uncached KSEG1 mirror at +0x20000000, and the RCP / PI register pages (their
 * writes land in scratch memory). */
#define RECOMP_RDRAM_SIZE (8u * 1024 * 1024)
#define RECOMP_WINDOW_SIZE 0x40000000u

extern uint8_t *gRecompRdram;

int recomp_mem_init(void); /* 0 on failure */
void recomp_mem_clear(void);

/* KSEG0 / KSEG1 / physical address -> offset in the window */
static inline uint32_t recomp_offset(uint32_t addr)
{
    return (addr >= 0x80000000u) ? addr - 0x80000000u : addr; /* physical RDRAM = KSEG0 offset */
}

static inline uint32_t rr_u32(uint32_t addr) { return *(uint32_t *)(gRecompRdram + recomp_offset(addr)); }
static inline uint16_t rr_u16(uint32_t addr) { return *(uint16_t *)(gRecompRdram + (recomp_offset(addr) ^ 2)); }
static inline uint8_t rr_u8(uint32_t addr) { return *(gRecompRdram + (recomp_offset(addr) ^ 3)); }
static inline void rw_u32(uint32_t addr, uint32_t v) { *(uint32_t *)(gRecompRdram + recomp_offset(addr)) = v; }
static inline void rw_u16(uint32_t addr, uint16_t v) { *(uint16_t *)(gRecompRdram + (recomp_offset(addr) ^ 2)) = v; }
static inline void rw_u8(uint32_t addr, uint8_t v) { *(gRecompRdram + (recomp_offset(addr) ^ 3)) = v; }

/* Big-endian bytes (ROM, save data) to / from RDRAM, any alignment. */
void recomp_mem_write_be(uint32_t addr, const uint8_t *src, uint32_t size);
void recomp_mem_read_be(uint32_t addr, uint8_t *dst, uint32_t size);

/* ---- calls ----------------------------------------------------------------------------------
 * o32: arguments in a0-a3 (r4-r7), then on the stack from sp + 16; results in v0 / v1. */
#define RA0(ctx) ((uint32_t)(ctx)->r4)
#define RA1(ctx) ((uint32_t)(ctx)->r5)
#define RA2(ctx) ((uint32_t)(ctx)->r6)
#define RA3(ctx) ((uint32_t)(ctx)->r7)
#define RSTACK(ctx, i) rr_u32((uint32_t)(ctx)->r29 + 16 + 4 * (i)) /* 5th argument and on */
#define RRET(ctx, v) ((ctx)->r2 = (gpr)(int32_t)(uint32_t)(v))
/* v is evaluated once: it may have side effects (reading the clock advances it) */
#define RRET64(ctx, v) do { uint64_t rret64_ = (uint64_t)(v); \
                            (ctx)->r2 = (gpr)(int32_t)(uint32_t)(rret64_ >> 32); \
                            (ctx)->r3 = (gpr)(int32_t)(uint32_t)rret64_; } while (0)

/* N64Recomp's generated lookup.cpp (compiled as C) */
gpr get_entrypoint_address(void);

/* ---- sections (recomp_sections.cpp, generated per game) ------------------------------------ */
typedef struct RecompSection RecompSection;
const void *recomp_section_table(size_t *count); /* SectionTableEntry[] */

void recomp_sections_init(void);
/* A PI DMA copied ROM [rom, rom + size) to RAM at vram: code sections in it become the live ones. */
void recomp_sections_on_dma(uint32_t rom, uint32_t vram, uint32_t size);

/* ---- threads (recomp_os.c) --------------------------------------------------------------- */
void recomp_os_reset(void);
void recomp_os_boot(void);             /* starts the boot code (entrypoint) */
void recomp_os_run(void);              /* run threads until all are blocked */
void recomp_os_post_event(int event);  /* OS_EVENT_* */
void recomp_os_post_vi_retrace(void);
void recomp_os_send(uint32_t mq, uint32_t msg); /* OS_MESG_NOBLOCK send from runtime code */

/* ---- statistics (N64_RECOMP_STATS=1: call counts of the OS functions, printed at shutdown) --- */
void recomp_stat(const char *name);
void recomp_stats_print(void);
#define RECOMP_STAT() recomp_stat(__func__)

/* ---- devices (recomp_io.c) --------------------------------------------------------------- */
extern unsigned int gPortFrameCount;

void recomp_fatal(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_RT_H */
