/*
 * Guest memory of the wasm build.
 *
 * The guest module is linked with its data at 0x80000400, where an N64 game's own code and
 * data start, so its pointers look like N64 (KSEG0) addresses. N64W_OFFSET folds every
 * address onto one buffer the way the N64 mirrors its RAM (KSEG0, KSEG1 and physical
 * addresses of the same byte meet), which also keeps every access inside the buffer:
 * no bounds checks are needed. NULL and other low pointers read the bottom of the buffer
 * (below the module's data), as on the console.
 *
 * The buffer holds big-endian data: the guest code is compiled to swap on every access
 * (tools/wasm_be_cc.py), so wasm loads and stores themselves are plain host order.
 */
#ifndef N64W_H
#define N64W_H

#include <stdint.h>

#define N64W_WINDOW_MASK 0x03FFFFFFu /* 64 MB */
#define N64W_OFFSET(a) ((uint32_t)(a) & N64W_WINDOW_MASK)
/* room for an access of up to 16 bytes at the last address */
#define N64W_MEM_BYTES (N64W_WINDOW_MASK + 1u + 16u)

#endif /* N64W_H */
