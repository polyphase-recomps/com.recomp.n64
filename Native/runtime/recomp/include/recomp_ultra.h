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

#define RECOMP_ULTRA(name) void name##_recomp(uint8_t *rdram, recomp_context *ctx)

/* threads, messages, events, interrupts (recomp_os.c) */
RECOMP_ULTRA(osCreateThread);
RECOMP_ULTRA(osStartThread);
RECOMP_ULTRA(osStopThread);
RECOMP_ULTRA(osDestroyThread);
RECOMP_ULTRA(osSetThreadPri);
RECOMP_ULTRA(osGetThreadPri);
RECOMP_ULTRA(osCreateMesgQueue);
RECOMP_ULTRA(osSendMesg);
RECOMP_ULTRA(osJamMesg);
RECOMP_ULTRA(osRecvMesg);
RECOMP_ULTRA(osSetEventMesg);
RECOMP_ULTRA(osViSetEvent);
RECOMP_ULTRA(osSetIntMask);
RECOMP_ULTRA(__osDisableInt);
RECOMP_ULTRA(__osRestoreInt);

/* boot, PI, SI, VI, time, RSP/RDP, AI, caches (recomp_io.c) */
RECOMP_ULTRA(osInitialize);
RECOMP_ULTRA(osCartRomInit);
RECOMP_ULTRA(osCreatePiManager);
RECOMP_ULTRA(osEPiStartDma);
RECOMP_ULTRA(osContInit);
RECOMP_ULTRA(osContStartQuery);
RECOMP_ULTRA(osContGetQuery);
RECOMP_ULTRA(osContStartReadData);
RECOMP_ULTRA(osContGetReadData);
RECOMP_ULTRA(__osContAddressCrc);
RECOMP_ULTRA(osMotorInit);
RECOMP_ULTRA(__osMotorAccess);
RECOMP_ULTRA(osCreateViManager);
RECOMP_ULTRA(osViSetMode);
RECOMP_ULTRA(osViSetYScale);
RECOMP_ULTRA(osViBlack);
RECOMP_ULTRA(osViSwapBuffer);
RECOMP_ULTRA(osViGetCurrentFramebuffer);
RECOMP_ULTRA(osViGetNextFramebuffer);
RECOMP_ULTRA(osGetCount);
RECOMP_ULTRA(osGetTime);
RECOMP_ULTRA(osSetTime);
RECOMP_ULTRA(osSetTimer);
RECOMP_ULTRA(osStopTimer);
RECOMP_ULTRA(osSpTaskLoad);
RECOMP_ULTRA(osSpTaskStartGo);
RECOMP_ULTRA(osSpTaskYield);
RECOMP_ULTRA(osSpTaskYielded);
RECOMP_ULTRA(__osSpSetPc);
RECOMP_ULTRA(osDpSetNextBuffer);
RECOMP_ULTRA(osAiSetFrequency);
RECOMP_ULTRA(osAiSetNextBuffer);
RECOMP_ULTRA(osAiGetLength);
RECOMP_ULTRA(osAiGetStatus);
RECOMP_ULTRA(osInvalDCache);
RECOMP_ULTRA(osInvalICache);
RECOMP_ULTRA(osWritebackDCache);
RECOMP_ULTRA(osWritebackDCacheAll);
RECOMP_ULTRA(__osSetWatchLo);
RECOMP_ULTRA(osVirtualToPhysical);

/* compiler helpers and FPU control (recomp_rt.c) */
RECOMP_ULTRA(__ll_div);
RECOMP_ULTRA(__ull_div);
RECOMP_ULTRA(__ll_rem);
RECOMP_ULTRA(__ull_rem);
RECOMP_ULTRA(__ll_mul);
RECOMP_ULTRA(__ll_lshift);
RECOMP_ULTRA(__ull_rshift);
RECOMP_ULTRA(__ll_to_f);
RECOMP_ULTRA(__ull_to_f);
RECOMP_ULTRA(__f_to_ll);
RECOMP_ULTRA(__ull_to_d);
RECOMP_ULTRA(__osSetFpcCsr);

#undef RECOMP_ULTRA

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_ULTRA_H */
