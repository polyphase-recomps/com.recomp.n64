/*
 * How RDRAM is laid out in host memory in recomp mode, shared by the generated code's memory
 * macros (recomp.h) and the runtime's accessors (recomp_rt.h), so both always agree.
 *
 * Byte order:
 *   little-endian hosts  RDRAM holds native 32-bit words: byte addresses ^3, halfwords ^2
 *                        (N64Recomp's own layout, RECOMP_SWAPPED)
 *   big-endian hosts     RDRAM is the N64's memory image as it is (GameCube, Wii)
 * Addressing:
 *   64-bit hosts         a 1 GB window from 0x80000000: RDRAM, its KSEG1 mirror and the RCP
 *                        registers are reached by plain addition (RECOMP_WINDOW)
 *   32-bit hosts         an address is masked to its physical one: RDRAM, or a small register
 *                        area for the rest (recomp_io_ptr); costs a compare per access
 */
#ifndef RECOMP_LAYOUT_H
#define RECOMP_LAYOUT_H

#include <stdint.h>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define RECOMP_HOST_BE 1
#define RECOMP_SWAPPED 0
#else
#define RECOMP_HOST_BE 0
#define RECOMP_SWAPPED 1
#endif

#if UINTPTR_MAX > 0xFFFFFFFFu && !defined(RECOMP_MASKED)
#define RECOMP_WINDOW 1
#else
#define RECOMP_WINDOW 0
#endif

/* RDRAM the runtime allocates (8 MB: with the Expansion Pak; a game that needs only 4 MB can
 * build with RECOMP_RDRAM_MB=4 on hosts short on memory) */
#ifndef RECOMP_RDRAM_MB
#define RECOMP_RDRAM_MB 8
#endif
#define RECOMP_RDRAM_SIZE ((uint32_t)RECOMP_RDRAM_MB << 20)

/* byte / halfword address adjustment of the layout */
#define RECOMP_XOR8 (RECOMP_SWAPPED ? 3u : 0u)
#define RECOMP_XOR16 (RECOMP_SWAPPED ? 2u : 0u)

#ifdef __cplusplus
extern "C" {
#endif

extern uint8_t *gRecompRdram;

#if RECOMP_WINDOW
/* KSEG0 / KSEG1 / physical address -> host memory: `rdram` is KSEG0 0x80000000 */
static inline uint8_t *recomp_ptr(uint8_t *rdram, uint32_t addr)
{
    return rdram + ((addr >= 0x80000000u) ? addr - 0x80000000u : addr);
}
#else
/* Anything outside RDRAM: the RCP / PI / SI registers and the rest of the physical space. */
uint8_t *recomp_io_ptr(uint32_t phys);

static inline uint8_t *recomp_ptr(uint8_t *rdram, uint32_t addr)
{
    const uint32_t phys = addr & 0x1FFFFFFFu;

    return (phys < RECOMP_RDRAM_SIZE) ? rdram + phys : recomp_io_ptr(phys);
}
#endif

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_LAYOUT_H */
