/*
 * Recomp mode: libultra's threads, message queues and events for recompiled game code.
 *
 * The same model as the decomp port's port_os.c: every OSThread is a coroutine, there is no
 * time preemption, and a thread runs until it blocks, stops or wakes a higher-priority thread.
 * The difference is where things live: the game's OSThread and OSMesgQueue structures are in
 * RDRAM, read and written in place (the game reads queue counts itself), and every thread has
 * its own recomp_context (the CPU registers of recompiled code).
 */
#include "recomp_rt.h"
#include "recomp_hooks.h"

#include <port_host.h>

#include <string.h>

/* libultra (2.0) */
#define OS_STATE_STOPPED 1
#define OS_STATE_RUNNABLE 2
#define OS_STATE_WAITING 8
#define OS_PRIORITY_IDLE 0
#define OS_NUM_EVENTS 15
#define OS_MESG_BLOCK 1

/* OSThread fields */
#define TH_PRIORITY 4
#define TH_STATE 16
#define TH_ID 20
/* OSMesgQueue fields */
#define MQ_VALID 8
#define MQ_FIRST 12
#define MQ_COUNT 16
#define MQ_MSG 20

/* Loop iterations a thread may run per turn before it counts as spinning (add_loop_checks.py) */
#ifndef RECOMP_LOOP_BUDGET
#define RECOMP_LOOP_BUDGET (1 << 22)
#endif
int32_t gRecompLoopBudget = RECOMP_LOOP_BUDGET;

#define MAX_THREADS 256
#ifndef RECOMP_THREAD_STACK
#define RECOMP_THREAD_STACK (4 * 1024 * 1024) /* native stack: recompiled calls nest like the game's */
#endif

typedef struct RThread
{
    uint32_t os; /* OSThread address; 0 for the boot code */
    PortCoro *coro;
    uint32_t entry, arg, sp;
    uint32_t wait_queue; /* queue this thread is blocked on */
    int is_wait_send;
    int is_dead;
    int boot_priority; /* the boot code has no OSThread */
    recomp_context ctx;
} RThread;

static RThread sThreads[MAX_THREADS];
static int sThreadsNum;
static RThread *sCurrent;

static struct
{
    uint32_t mq, msg;
} sEvents[OS_NUM_EVENTS], sViEvent;

static int thread_priority(const RThread *t)
{
    return t->os ? (int32_t)rr_u32(t->os + TH_PRIORITY) : t->boot_priority;
}

static int thread_state(const RThread *t)
{
    return t->os ? rr_u16(t->os + TH_STATE) : (t->is_dead ? OS_STATE_STOPPED : OS_STATE_RUNNABLE);
}

static void set_state(RThread *t, int state)
{
    if (t->os)
    {
        rw_u16(t->os + TH_STATE, (uint16_t)state);
    }
}

static RThread *thread_find(uint32_t os)
{
    int i;

    for (i = 0; i < sThreadsNum; i++)
    {
        if (sThreads[i].os == os && !sThreads[i].is_dead)
        {
            return &sThreads[i];
        }
    }
    return NULL;
}

static RThread *thread_alloc(void)
{
    int i;

    for (i = 0; i < sThreadsNum; i++)
    {
        if (sThreads[i].is_dead && sThreads[i].coro == NULL)
        {
            return &sThreads[i];
        }
    }
    if (sThreadsNum == MAX_THREADS)
    {
        recomp_fatal("out of thread slots");
    }
    return &sThreads[sThreadsNum++];
}

static void context_init(recomp_context *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->f_odd = &ctx->f0.u32h; /* 32-bit FPU mode: odd registers are the high halves */
    ctx->mips3_float_mode = 0;
}

static void thread_entry(void *param)
{
    RThread *t = param;
    recomp_func_t *func = get_function((int32_t)t->entry);

    t->ctx.r4 = (gpr)(int32_t)t->arg;
    t->ctx.r29 = (gpr)(int32_t)t->sp;
    func(gRecompRdram, &t->ctx);
    /* Returning from the thread function ends the thread (osDestroyThread(NULL)). */
    set_state(t, OS_STATE_STOPPED);
    t->is_dead = 1;
}

static void reschedule(void)
{
    if (sCurrent != NULL)
    {
        port_coro_yield();
    }
}

static RThread *pick_runnable(void)
{
    RThread *best = NULL;
    int i;

    for (i = 0; i < sThreadsNum; i++)
    {
        RThread *t = &sThreads[i];

        if (t->is_dead || thread_state(t) != OS_STATE_RUNNABLE || thread_priority(t) <= OS_PRIORITY_IDLE)
        {
            continue;
        }
        if (best == NULL || thread_priority(t) > thread_priority(best))
        {
            best = t;
        }
    }
    return best;
}

void recomp_os_run(void)
{
    RThread *t;
    int i;

    if (sCurrent != NULL)
    {
        recomp_fatal("recomp_os_run called from inside a thread");
    }
    while (!port_faulted() && (t = pick_runnable()) != NULL)
    {
        sCurrent = t;
        gRecompLoopBudget = RECOMP_LOOP_BUDGET;
        port_coro_resume(t->coro);
        sCurrent = NULL;
    }
    if (port_faulted())
    {
        return;
    }
    for (i = 0; i < sThreadsNum; i++)
    {
        if (sThreads[i].is_dead && sThreads[i].coro != NULL)
        {
            port_coro_destroy(sThreads[i].coro);
            sThreads[i].coro = NULL;
        }
    }
}

void recomp_os_reset(void)
{
    int i;

    for (i = 0; i < sThreadsNum; i++)
    {
        if (sThreads[i].coro != NULL)
        {
            port_coro_destroy(sThreads[i].coro);
        }
    }
    memset(sThreads, 0, sizeof(sThreads));
    sThreadsNum = 0;
    sCurrent = NULL;
    memset(sEvents, 0, sizeof(sEvents));
    memset(&sViEvent, 0, sizeof(sViEvent));
}

/* The boot code (the ROM entrypoint, then the game's main) runs as a thread without an
 * OSThread, above every game thread, until it starts the first thread: on the N64 that call
 * never returns. */
void recomp_os_boot(void)
{
    RThread *t = thread_alloc();

    memset(t, 0, sizeof(*t));
    context_init(&t->ctx);
    t->entry = (uint32_t)get_entrypoint_address();
    t->sp = 0x80400000u - 16; /* the entrypoint sets its own */
    t->boot_priority = 0x7FFFFFFF;
    t->coro = port_coro_create(thread_entry, t, RECOMP_THREAD_STACK);
}

/* ---- threads ---------------------------------------------------------------------------- */
void osInitialize_recomp(uint8_t *rdram, recomp_context *ctx)
{
    RECOMP_STAT();
}

/* osCreateThread(OSThread *t, OSId id, void (*entry)(void *), void *arg, void *sp, OSPri pri) */
void osCreateThread_recomp(uint8_t *rdram, recomp_context *ctx)
{
    RECOMP_STAT();
    uint32_t os = RA0(ctx);
    RThread *t = thread_find(os);

    if (t != NULL)
    {
        t->is_dead = 1; /* re-created over a live OSThread: the old one is retired */
    }
    t = thread_alloc();
    memset(t, 0, sizeof(*t));
    context_init(&t->ctx);
    t->os = os;
    t->entry = RA2(ctx);
    t->arg = RA3(ctx);
    t->sp = RSTACK(ctx, 0) - 16; /* as __osCreateThread: room for the callee's argument save area */
    t->coro = port_coro_create(thread_entry, t, RECOMP_THREAD_STACK);

    rw_u32(os + 0, 0);
    rw_u32(os + TH_PRIORITY, RSTACK(ctx, 1));
    rw_u32(os + 8, 0);
    rw_u16(os + TH_STATE, OS_STATE_STOPPED);
    rw_u32(os + TH_ID, RA1(ctx));
}

void osStartThread_recomp(uint8_t *rdram, recomp_context *ctx)
{
    RECOMP_STAT();
    RThread *t = thread_find(RA0(ctx));

    if (t == NULL)
    {
        recomp_fatal("osStartThread: unknown thread 0x%08X", RA0(ctx));
    }
    if (thread_state(t) == OS_STATE_STOPPED)
    {
        set_state(t, t->wait_queue ? OS_STATE_WAITING : OS_STATE_RUNNABLE);
    }
    if (sCurrent != NULL && sCurrent->os == 0)
    {
        /* the boot code hands over to the threads for good */
        sCurrent->is_dead = 1;
        reschedule();
        recomp_fatal("boot code resumed");
    }
    if (sCurrent != NULL && thread_priority(t) > thread_priority(sCurrent))
    {
        reschedule();
    }
}

void osStopThread_recomp(uint8_t *rdram, recomp_context *ctx)
{
    RECOMP_STAT();
    uint32_t os = RA0(ctx);

    if (os == 0)
    {
        if (sCurrent == NULL)
        {
            recomp_fatal("osStopThread(NULL) outside a thread");
        }
        set_state(sCurrent, OS_STATE_STOPPED);
        reschedule();
        return;
    }
    rw_u16(os + TH_STATE, OS_STATE_STOPPED);
    if (sCurrent != NULL && sCurrent->os == os)
    {
        reschedule();
    }
}

void osDestroyThread_recomp(uint8_t *rdram, recomp_context *ctx)
{
    RECOMP_STAT();
    RThread *t = RA0(ctx) ? thread_find(RA0(ctx)) : sCurrent;

    if (t == NULL)
    {
        return;
    }
    set_state(t, OS_STATE_STOPPED);
    t->wait_queue = 0;
    t->is_dead = 1;
    if (t == sCurrent)
    {
        reschedule();
        recomp_fatal("destroyed thread was resumed");
    }
}

void osSetThreadPri_recomp(uint8_t *rdram, recomp_context *ctx)
{
    RECOMP_STAT();
    uint32_t os = RA0(ctx) ? RA0(ctx) : (sCurrent ? sCurrent->os : 0);

    if (os != 0)
    {
        rw_u32(os + TH_PRIORITY, RA1(ctx));
    }
    reschedule();
}

void osGetThreadPri_recomp(uint8_t *rdram, recomp_context *ctx)
{
    RECOMP_STAT();
    uint32_t os = RA0(ctx) ? RA0(ctx) : (sCurrent ? sCurrent->os : 0);

    RRET(ctx, os ? rr_u32(os + TH_PRIORITY) : 0);
}

/* `b .` loops (waiting for an interrupt that never comes back): park the thread. */
void pause_self(uint8_t *rdram)
{
    if (sCurrent == NULL)
    {
        recomp_fatal("pause_self outside a thread");
    }
    set_state(sCurrent, OS_STATE_STOPPED);
    for (;;)
    {
        port_coro_yield();
    }
}

/* ---- message queues ------------------------------------------------------------------------ */
/* osCreateMesgQueue(OSMesgQueue *mq, OSMesg *msg, s32 count) */
void osCreateMesgQueue_recomp(uint8_t *rdram, recomp_context *ctx)
{
    RECOMP_STAT();
    uint32_t mq = RA0(ctx);

    rw_u32(mq + 0, 0);
    rw_u32(mq + 4, 0);
    rw_u32(mq + MQ_VALID, 0);
    rw_u32(mq + MQ_FIRST, 0);
    rw_u32(mq + MQ_COUNT, RA2(ctx));
    rw_u32(mq + MQ_MSG, RA1(ctx));
}

static RThread *wake_waiter(uint32_t mq, int is_send)
{
    RThread *best = NULL;
    int i;

    for (i = 0; i < sThreadsNum; i++)
    {
        RThread *t = &sThreads[i];

        if (t->is_dead || t->wait_queue != mq || t->is_wait_send != is_send)
        {
            continue;
        }
        if (best == NULL || thread_priority(t) > thread_priority(best))
        {
            best = t;
        }
    }
    if (best != NULL)
    {
        best->wait_queue = 0;
        if (thread_state(best) == OS_STATE_WAITING)
        {
            set_state(best, OS_STATE_RUNNABLE);
        }
    }
    return best;
}

static void block_on(uint32_t mq, int is_send)
{
    if (sCurrent == NULL)
    {
        recomp_fatal("blocking message queue call from the host");
    }
    sCurrent->wait_queue = mq;
    sCurrent->is_wait_send = is_send;
    set_state(sCurrent, OS_STATE_WAITING);
    port_coro_yield();
}

static int put_mesg(uint32_t mq, uint32_t msg, int flag, int is_jam)
{
    RThread *woken;
    int32_t count = (int32_t)rr_u32(mq + MQ_COUNT);

    while ((int32_t)rr_u32(mq + MQ_VALID) >= count)
    {
        if (flag != OS_MESG_BLOCK)
        {
            return -1;
        }
        block_on(mq, 1);
    }
    int32_t first = (int32_t)rr_u32(mq + MQ_FIRST), valid = (int32_t)rr_u32(mq + MQ_VALID);
    uint32_t buf = rr_u32(mq + MQ_MSG);
    if (is_jam)
    {
        first = (first + count - 1) % count;
        rw_u32(mq + MQ_FIRST, (uint32_t)first);
        rw_u32(buf + 4 * first, msg);
    }
    else
    {
        rw_u32(buf + 4 * ((first + valid) % count), msg);
    }
    rw_u32(mq + MQ_VALID, (uint32_t)(valid + 1));

    woken = wake_waiter(mq, 0);
    if (woken != NULL && sCurrent != NULL && thread_priority(woken) > thread_priority(sCurrent))
    {
        reschedule();
    }
    return 0;
}

void recomp_os_send(uint32_t mq, uint32_t msg)
{
    if (mq != 0)
    {
        put_mesg(mq, msg, 0, 0);
    }
}

/* osSendMesg(OSMesgQueue *mq, OSMesg msg, s32 flag) */
void osSendMesg_recomp(uint8_t *rdram, recomp_context *ctx)
{
    RECOMP_STAT();
    RRET(ctx, put_mesg(RA0(ctx), RA1(ctx), (int)RA2(ctx), 0));
}

void osJamMesg_recomp(uint8_t *rdram, recomp_context *ctx)
{
    RECOMP_STAT();
    RRET(ctx, put_mesg(RA0(ctx), RA1(ctx), (int)RA2(ctx), 1));
}

/* osRecvMesg(OSMesgQueue *mq, OSMesg *msg, s32 flag) */
void osRecvMesg_recomp(uint8_t *rdram, recomp_context *ctx)
{
    RECOMP_STAT();
    uint32_t mq = RA0(ctx), out = RA1(ctx);
    int flag = (int)RA2(ctx);
    RThread *woken;

    while (rr_u32(mq + MQ_VALID) == 0)
    {
        if (flag != OS_MESG_BLOCK)
        {
            RRET(ctx, -1);
            return;
        }
        block_on(mq, 0);
    }
    int32_t first = (int32_t)rr_u32(mq + MQ_FIRST), count = (int32_t)rr_u32(mq + MQ_COUNT);
    if (out != 0)
    {
        rw_u32(out, rr_u32(rr_u32(mq + MQ_MSG) + 4 * first));
    }
    rw_u32(mq + MQ_FIRST, (uint32_t)((first + 1) % count));
    rw_u32(mq + MQ_VALID, rr_u32(mq + MQ_VALID) - 1);

    woken = wake_waiter(mq, 1);
    if (woken != NULL && sCurrent != NULL && thread_priority(woken) > thread_priority(sCurrent))
    {
        reschedule();
    }
    RRET(ctx, 0);
}

/* ---- events -------------------------------------------------------------------------------- */
/* osSetEventMesg(OSEvent e, OSMesgQueue *mq, OSMesg msg) */
void osSetEventMesg_recomp(uint8_t *rdram, recomp_context *ctx)
{
    RECOMP_STAT();
    uint32_t e = RA0(ctx);

    if (e < OS_NUM_EVENTS)
    {
        sEvents[e].mq = RA1(ctx);
        sEvents[e].msg = RA2(ctx);
    }
}

/* osViSetEvent(OSMesgQueue *mq, OSMesg msg, u32 retraceCount) */
void osViSetEvent_recomp(uint8_t *rdram, recomp_context *ctx)
{
    RECOMP_STAT();
    sViEvent.mq = RA0(ctx);
    sViEvent.msg = RA1(ctx);
}

void recomp_os_post_event(int event)
{
    if (event >= 0 && event < OS_NUM_EVENTS && sEvents[event].mq != 0)
    {
        put_mesg(sEvents[event].mq, sEvents[event].msg, 0, 0);
    }
}

#define WAIT_FRAME 0xFFFFFFFFu /* wait_queue of a thread parked by recomp_wait_frame */


void recomp_loop_preempt(uint8_t *rdram, recomp_context *ctx, uint32_t loop_vram)
{
    static uint32_t sLogged[256];
    static int sLoggedNum;
    int i;

    gRecompLoopBudget = RECOMP_LOOP_BUDGET;
    if (sCurrent == NULL || sCurrent->os == 0)
    {
        return; /* the boot code has nobody to wait for */
    }
    for (i = 0; i < sLoggedNum && sLogged[i] != loop_vram; i++)
    {
    }
    if (i == sLoggedNum && sLoggedNum < 256)
    {
        sLogged[sLoggedNum++] = loop_vram;
        port_log("recomp: thread %u spins in the loop at 0x%08X: it now waits a frame each round "
                 "(a [[patches.hook]] can make that explicit)", rr_u32(sCurrent->os + TH_ID), loop_vram);
    }
    recomp_wait_frame(rdram, ctx);
}

void recomp_wait_frame(uint8_t *rdram, recomp_context *ctx)
{
    if (sCurrent == NULL)
    {
        return;
    }
    sCurrent->wait_queue = WAIT_FRAME;
    set_state(sCurrent, OS_STATE_WAITING);
    if (sCurrent->os == 0)
    {
        recomp_fatal("recomp_wait_frame in the boot code");
    }
    port_coro_yield();
}

void recomp_os_post_vi_retrace(void)
{
    int i;

    for (i = 0; i < sThreadsNum; i++)
    {
        RThread *t = &sThreads[i];

        if (!t->is_dead && t->wait_queue == WAIT_FRAME)
        {
            t->wait_queue = 0;
            set_state(t, OS_STATE_RUNNABLE);
        }
    }
    if (sViEvent.mq != 0)
    {
        put_mesg(sViEvent.mq, sViEvent.msg, 0, 0);
    }
}

/* ---- interrupts: no preemption, so masking is bookkeeping --------------------------------- */
static uint32_t sIntMask = 0x003FFF01;

void osSetIntMask_recomp(uint8_t *rdram, recomp_context *ctx)
{
    RECOMP_STAT();
    uint32_t old = sIntMask;

    sIntMask = RA0(ctx);
    RRET(ctx, old);
}

void __osDisableInt_recomp(uint8_t *rdram, recomp_context *ctx)
{
    RECOMP_STAT();
    RRET(ctx, 1);
}

void __osRestoreInt_recomp(uint8_t *rdram, recomp_context *ctx)
{
    RECOMP_STAT();
}
