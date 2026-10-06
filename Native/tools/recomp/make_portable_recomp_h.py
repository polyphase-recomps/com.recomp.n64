"""Derives com.recomp.n64's portable recomp.h (runtime/recomp/include/portable) from the vendored
N64Recomp's include/recomp.h, for hosts that are not 64-bit little-endian. See the comment it
writes at the top of that file. Run it again after updating ThirdParty/N64Recomp.

    python make_portable_recomp_h.py
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
NATIVE = os.path.normpath(os.path.join(HERE, '..', '..'))
SRC = os.path.join(NATIVE, '..', 'ThirdParty', 'N64Recomp', 'include', 'recomp.h')
DST = os.path.join(NATIVE, 'runtime', 'recomp', 'include', 'portable', 'recomp.h')
BS = '\\'

up = open(SRC, encoding='utf-8').read()


def rep(old, new):
    global up
    assert up.count(old) == 1, old[:60]
    up = up.replace(old, new)


rep('''#else
#error "128-bit integer type not found"
#endif''', '''#else

/* 32-bit hosts: the 128-bit product from 32-bit halves */
static inline void DMULTU(uint64_t a, uint64_t b, uint64_t* lo64, uint64_t* hi64) {
    uint64_t a_lo = (uint32_t)a, a_hi = a >> 32, b_lo = (uint32_t)b, b_hi = b >> 32;
    uint64_t lo_lo = a_lo * b_lo, hi_lo = a_hi * b_lo, lo_hi = a_lo * b_hi, hi_hi = a_hi * b_hi;
    uint64_t cross = (lo_lo >> 32) + (uint32_t)hi_lo + lo_hi;

    *hi64 = hi_hi + (hi_lo >> 32) + (cross >> 32);
    *lo64 = (cross << 32) | (uint32_t)lo_lo;
}

static inline void DMULT(int64_t a, int64_t b, int64_t* lo64, int64_t* hi64) {
    uint64_t lo, hi;

    DMULTU((uint64_t)a, (uint64_t)b, &lo, &hi);
    /* signed: subtract the other operand from the high half for each negative one */
    if (a < 0) hi -= (uint64_t)b;
    if (b < 0) hi -= (uint64_t)a;
    *lo64 = (int64_t)lo;
    *hi64 = (int64_t)hi;
}

#endif''')

start = up.index('#define MEM_W(offset, reg) ' + BS)
end = up.index('static inline uint64_t load_doubleword')
mem = f'''/* (recomp_layout.h: the byte order and addressing of RDRAM on this host) */
#define RECOMP_MEM(offset, reg, adjust) {BS}
    recomp_ptr(rdram, (uint32_t)((reg) + (offset)) ^ (adjust))

#define MEM_W(offset, reg) {BS}
    (*(int32_t*)RECOMP_MEM(offset, reg, 0))

#define MEM_H(offset, reg) {BS}
    (*(int16_t*)RECOMP_MEM(offset, reg, RECOMP_XOR16))

#define MEM_B(offset, reg) {BS}
    (*(int8_t*)RECOMP_MEM(offset, reg, RECOMP_XOR8))

#define MEM_HU(offset, reg) {BS}
    (*(uint16_t*)RECOMP_MEM(offset, reg, RECOMP_XOR16))

#define MEM_BU(offset, reg) {BS}
    (*(uint8_t*)RECOMP_MEM(offset, reg, RECOMP_XOR8))

#define SD(val, offset, reg) {{ {BS}
    *(uint32_t*)RECOMP_MEM((offset) + 4, reg, 0) = (uint32_t)((gpr)(val) >> 0); {BS}
    *(uint32_t*)RECOMP_MEM((offset) + 0, reg, 0) = (uint32_t)((gpr)(val) >> 32); {BS}
}}

'''
up = up[:start] + mem + up[end:]

rep('''typedef union {
    double d;
    struct {
        float fl;
        float fh;
    };
    struct {
        uint32_t u32l;
        uint32_t u32h;
    };
    uint64_t u64;
} fpr;''', '''typedef union {
    double d;
#if RECOMP_HOST_BE
    /* the low word of a double comes second in memory on a big-endian host */
    struct {
        float fh;
        float fl;
    };
    struct {
        uint32_t u32h;
        uint32_t u32l;
    };
#else
    struct {
        float fl;
        float fh;
    };
    struct {
        uint32_t u32l;
        uint32_t u32h;
    };
#endif
    uint64_t u64;
} fpr;''')

rep('''#ifndef __RECOMP_H__
#define __RECOMP_H__
''', '''/*
 * N64Recomp's generated-code contract (include/recomp.h of N64Recomp, MIT License,
 * Copyright (c) 2024 Wiseguy; see ThirdParty/N64Recomp/LICENSE), made portable for com.recomp.n64:
 * hosts that are not 64-bit little-endian (GameCube, Wii, 3DS, ...).
 *
 * On a 64-bit little-endian host this defers to N64Recomp's own header unchanged. Elsewhere it is
 * that header (snapshot ffb39cd) with:
 *   - memory macros going through recomp_layout.h (RDRAM in the host's byte order where that is
 *     the N64's, masked addressing where there is no room for a 1 GB window)
 *   - float register halves named by meaning (`fl` is the low word of `d`) on big-endian hosts
 *   - 64x64 -> 128-bit products without __int128
 * Made by com.recomp.n64's Native/tools/recomp/make_portable_recomp_h.py; run it again when the
 * vendored N64Recomp is updated.
 */
#include "recomp_layout.h"

#if RECOMP_WINDOW && RECOMP_SWAPPED
#include_next <recomp.h>
#else

#ifndef __RECOMP_H__
#define __RECOMP_H__
''')
up = up.rstrip('\n')
assert up.endswith('#endif')
up += '\n\n#endif /* !(RECOMP_WINDOW && RECOMP_SWAPPED) */\n'
open(DST, 'w', encoding='utf-8', newline='\n').write(up)
print('wrote', DST)
