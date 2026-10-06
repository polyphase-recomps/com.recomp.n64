/*
 * Runtime functions a game's N64Recomp config can call from [[patches.hook]] text (C inserted
 * before an instruction of a recompiled function). Force-included into the generated code.
 *
 *   [[patches.hook]]
 *   func = "scManagerRunLoop"
 *   before_vram = 0x800A1BB0
 *   text = "recomp_wait_frame(rdram, ctx);"
 */
#ifndef RECOMP_HOOKS_H
#define RECOMP_HOOKS_H

#include "recomp.h"
#include "recomp_ultra.h" /* the runtime's libultra: N64Recomp's funcs.h misses some */

#ifdef __cplusplus
extern "C" {
#endif

/* Park the calling thread until the next video frame. For busy-wait loops on a flag another
 * thread clears: on the N64 that thread preempts the loop, here threads only switch when one
 * blocks. */
void recomp_wait_frame(uint8_t *rdram, recomp_context *ctx);

/* Preemption points (tools/recomp/add_loop_checks.py puts one on every loop back edge): a
 * thread that loops this long without blocking is spinning on something another thread will
 * do, so it waits for the next frame. The first time each loop does, it is logged. */
extern int32_t gRecompLoopBudget;
void recomp_loop_preempt(uint8_t *rdram, recomp_context *ctx, uint32_t loop_vram);
#define RECOMP_LOOP_CHECK(vram)                                                                       \
    do                                                                                                \
    {                                                                                                 \
        if (--gRecompLoopBudget < 0) recomp_loop_preempt(rdram, ctx, (vram));                          \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_HOOKS_H */
