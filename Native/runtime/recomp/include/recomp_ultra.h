/*
 * Everything the recomp runtime implements in place of the game's own code: libultra (threads,
 * messages, PI/SI/VI/AI/SP, timers, caches) and the compiler's 64-bit helpers. N64Recomp emits
 * calls to these as <name>_recomp. Its funcs.h only declares some of them, and C99 compilers
 * (clang) reject calls to undeclared functions, so the generated code gets them all from here
 * (through recomp_hooks.h); the runtime includes it too, so both agree on every prototype.
 *
 * A game that calls a reimplemented function missing here fails to link: add it to the runtime
 * (runtime/recomp/recomp_*.c) and to this list.
 */
#ifndef RECOMP_ULTRA_H
#define RECOMP_ULTRA_H

#include "recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Every function, as X(name) (also used to build the live mode's name -> function table) */
#define RECOMP_ULTRA_FUNCS(X) \
    /* threads, messages, events, interrupts (recomp_os.c) */ \
    X(osCreateThread) \
    X(osStartThread) \
    X(osStopThread) \
    X(osDestroyThread) \
    X(osSetThreadPri) \
    X(osGetThreadPri) \
    X(osCreateMesgQueue) \
    X(osSendMesg) \
    X(osJamMesg) \
    X(osRecvMesg) \
    X(osSetEventMesg) \
    X(osViSetEvent) \
    X(osSetIntMask) \
    X(__osDisableInt) \
    X(__osRestoreInt) \
    /* boot, PI, SI, VI, time, RSP/RDP, AI, caches (recomp_io.c) */ \
    X(osInitialize) \
    X(osCartRomInit) \
    X(osCreatePiManager) \
    X(osEPiStartDma) \
    X(osContInit) \
    X(osContStartQuery) \
    X(osContGetQuery) \
    X(osContStartReadData) \
    X(osContGetReadData) \
    X(__osContAddressCrc) \
    X(osMotorInit) \
    X(__osMotorAccess) \
    X(osCreateViManager) \
    X(osViSetMode) \
    X(osViSetYScale) \
    X(osViBlack) \
    X(osViSwapBuffer) \
    X(osViGetCurrentFramebuffer) \
    X(osViGetNextFramebuffer) \
    X(osGetCount) \
    X(osGetTime) \
    X(osSetTime) \
    X(osSetTimer) \
    X(osStopTimer) \
    X(osSpTaskLoad) \
    X(osSpTaskStartGo) \
    X(osSpTaskYield) \
    X(osSpTaskYielded) \
    X(__osSpSetPc) \
    X(osDpSetNextBuffer) \
    X(osAiSetFrequency) \
    X(osAiSetNextBuffer) \
    X(osAiGetLength) \
    X(osAiGetStatus) \
    X(osInvalDCache) \
    X(osInvalICache) \
    X(osWritebackDCache) \
    X(osWritebackDCacheAll) \
    X(__osSetWatchLo) \
    X(osVirtualToPhysical) \
    /* compiler helpers and FPU control (recomp_rt.c) */ \
    X(__ll_div) \
    X(__ull_div) \
    X(__ll_rem) \
    X(__ull_rem) \
    X(__ll_mul) \
    X(__ll_lshift) \
    X(__ull_rshift) \
    X(__ll_to_f) \
    X(__ull_to_f) \
    X(__f_to_ll) \
    X(__ull_to_d) \
    X(__osSetFpcCsr)

#define RECOMP_ULTRA_DECLARE(name) void name##_recomp(uint8_t *rdram, recomp_context *ctx);
RECOMP_ULTRA_FUNCS(RECOMP_ULTRA_DECLARE)
#undef RECOMP_ULTRA_DECLARE

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_ULTRA_H */
