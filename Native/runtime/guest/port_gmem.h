/*
 * Guest memory as the RSP / RDP replacement (port_gfx.c) reads it.
 *
 * Normally that code runs inside the game, in the same address space and byte order: the
 * accessors are plain loads and guest addresses are pointers.
 *
 * PORT_RSP_HOST compiles it into the host of the big-endian wasm guest (runtime/wasm)
 * instead, where it runs as native code: addresses are guest addresses folded onto the
 * guest's memory buffer (n64w.h), and 16- and 32-bit values are stored big-endian.
 */
#ifndef PORT_GMEM_H
#define PORT_GMEM_H

#include <stdint.h>

#if defined(PORT_RSP_HOST) && defined(PORT_RSP_RECOMP)
/* Recomp mode (runtime/recomp): RDRAM as N64Recomp's code keeps it on a little-endian host,
 * native 32-bit words: halfword addresses ^2, byte addresses ^3. RDRAM pointers are word
 * aligned (the window is page aligned), so the XOR works on host pointers. */
extern uint8_t *gRecompRdram;

#define GM_PTR(addr) ((addr) == 0 ? NULL : (void *)(gRecompRdram + ((uint32_t)(addr) & 0x1FFFFFFFu)))
#define GM_U32(p) (*(const uint32_t *)(const void *)(p))
#define GM_U16(p) (*(const uint16_t *)(const void *)((uintptr_t)(p) ^ 2))
#define GM_U8(p) (*(const uint8_t *)(const void *)((uintptr_t)(p) ^ 3))
#define GM_BIG_ENDIAN_DATA 1
#define GM_SWAPPED 1
#elif defined(PORT_RSP_HOST)
#include "n64w.h"

extern unsigned char *gPortGuestMem; /* the guest's memory buffer (n64w_host.c) */

/* host pointer to a guest address (0 stays NULL) */
#define GM_PTR(addr) ((addr) == 0 ? NULL : (void *)(gPortGuestMem + N64W_OFFSET((uint32_t)(addr))))
#define GM_U32(p) __builtin_bswap32(*(const uint32_t *)(const void *)(p))
#define GM_U16(p) __builtin_bswap16(*(const uint16_t *)(const void *)(p))
/* guest data in memory is big-endian through and through */
#define GM_BIG_ENDIAN_DATA 1
#else
#define GM_PTR(addr) ((void *)(uintptr_t)(addr))
#define GM_U32(p) (*(const uint32_t *)(const void *)(p))
#define GM_U16(p) (*(const uint16_t *)(const void *)(p))
#define GM_BIG_ENDIAN_DATA 0
#endif

#ifndef GM_U8
#define GM_U8(p) (*(const uint8_t *)(const void *)(p))
#endif
#ifndef GM_SWAPPED
#define GM_SWAPPED 0
#endif

#define GM_S16(p) ((int16_t)GM_U16(p))
#define GM_S8(p) ((int8_t)GM_U8(p))

#endif /* PORT_GMEM_H */
