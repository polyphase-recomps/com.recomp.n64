/*
 * GameCube / Wii backend (devkitPPC + libogc): coroutines are LWP threads handing control back
 * and forth through semaphores, so only one of them ever runs at a time. Pointers are 32-bit,
 * so the arena can be any memory.
 *
 * NOTE: builds with devkitPPC but has not been run on hardware or in Dolphin yet.
 */
#if defined(GEKKO)
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <malloc.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

#include "port_host.h"
#include "port_host_plat.h"

void port_plat_abort(void)
{
    abort();
}

/* the time base (TB_TIMER_CLOCK kHz: bus clock / 4) */
unsigned long long port_ticks(void)
{
    return gettime();
}

/* ---- arena ----------------------------------------------------------------- */
unsigned char *port_plat_arena_reserve(unsigned long long size)
{
    unsigned char *base = memalign(32, (size_t)size);

    if (base != NULL)
    {
        memset(base, 0, (size_t)size);
    }
    return base;
}

int port_plat_arena_commit(unsigned char *addr, unsigned long long size)
{
    return 1;
}

/* ---- coroutines ------------------------------------------------------------
 * Each coroutine runs on a worker thread. Workers are never destroyed: when a coroutine ends
 * (or is destroyed) its worker goes back to a pool and serves the next coroutine created.
 * Games create and destroy N64 threads at every scene change, and tearing down libogc threads
 * (join, freeing the stack) at that rate proved unsafe - the runtime deadlocked with every
 * thread asleep. A pooled worker only ever waits on its two semaphores.
 */
struct PortCoro
{
    lwp_t thread;
    void *stack; /* our own allocation: libogc's built-in stack pool is far too small */
    u32 stack_words;
    sem_t run;  /* posted to let the coroutine continue */
    sem_t back; /* posted when the coroutine hands control back */
    void (*entry)(void *);
    void *arg;
    jmp_buf unwind; /* back to the worker loop when destroyed while suspended */
    int kill;
    int finished;
    struct PortCoro *next_free; /* pool of idle workers */
};

static PortCoro *sCurrentCoro;
static PortCoro *sFreeWorkers;

/* Last coroutine events, for the hang report of the standalone runner (host/main_ogc.c). */
#define TRACE_MAX 48
static struct { char what; PortCoro *coro; s32 result; } sTrace[TRACE_MAX];
static unsigned int sTraceNum;

static void trace(char what, PortCoro *coro, s32 result)
{
    sTrace[sTraceNum % TRACE_MAX].what = what;
    sTrace[sTraceNum % TRACE_MAX].coro = coro;
    sTrace[sTraceNum % TRACE_MAX].result = result;
    sTraceNum++;
}

void port_plat_dump_trace(void)
{
    unsigned int i, first = (sTraceNum > TRACE_MAX) ? sTraceNum - TRACE_MAX : 0;

    SYS_Report("coro trace (%u events; C create, c reuse, D destroy, R resume, B back, Y yield, W woke, F finished):\n", sTraceNum);
    for (i = first; i < sTraceNum; i++)
    {
        SYS_Report("  %c %p %d\n", sTrace[i % TRACE_MAX].what, (void *)sTrace[i % TRACE_MAX].coro, (int)sTrace[i % TRACE_MAX].result);
    }
    SYS_Report("  current %p\n", (void *)sCurrentCoro);
    if (sCurrentCoro != NULL && sCurrentCoro->stack != NULL)
    {
        /* The stuck coroutine's stack, from the deepest word that was ever written: code
         * addresses found there are (mostly) its call chain, innermost first. */
        const u32 *word = sCurrentCoro->stack, *end = word + sCurrentCoro->stack_words;
        int shown = 0;

        while (word < end && *word == 0)
        {
            word++;
        }
        SYS_Report("  stack of current, %u words deep:\n", (unsigned)(end - word));
        for (; word < end && shown < 90; word++)
        {
            if (*word >= 0x80004000u && *word < 0x80300000u && (*word & 3) == 0)
            {
                SYS_Report("  s %08X\n", (unsigned)*word);
                shown++;
            }
        }
    }
}

static void coro_wait_turn(PortCoro *coro)
{
    s32 result = LWP_SemWait(coro->run);

    trace('W', coro, result);
    if (coro->kill)
    {
        longjmp(coro->unwind, 1);
    }
}

static void *coro_worker(void *param)
{
    PortCoro *coro = param;

    for (;;)
    {
        /* Idle here until a coroutine is assigned and resumed (or destroyed unstarted). */
        if (setjmp(coro->unwind) == 0)
        {
            coro_wait_turn(coro);
            coro->entry(coro->arg);
        }
        coro->finished = 1;
        trace('F', coro, LWP_SemPost(coro->back));
    }
    return NULL;
}

PortCoro *port_coro_create(void (*entry)(void *), void *arg, unsigned long long stack_size)
{
    PortCoro *coro = sFreeWorkers;

    if (coro != NULL)
    {
        /* An idle worker: it is waiting for `run` at the top of its loop. */
        sFreeWorkers = coro->next_free;
        coro->next_free = NULL;
        coro->entry = entry;
        coro->arg = arg;
        coro->kill = 0;
        coro->finished = 0;
        trace('c', coro, 0);
        return coro;
    }
    coro = calloc(1, sizeof(*coro));
    coro->entry = entry;
    coro->arg = arg;
    if (stack_size < 0x10000)
    {
        stack_size = 0x10000;
    }
    coro->stack = memalign(32, (size_t)stack_size);
    if (coro->stack == NULL)
    {
        port_fatal("out of memory for a %u KB thread stack", (unsigned)(stack_size >> 10));
        return coro;
    }
    memset(coro->stack, 0, (size_t)stack_size); /* keeps the hang report's stack scan readable */
    coro->stack_words = (u32)(stack_size / 4);
    /* libogc reports failure as a non-zero value (a positive errno in recent versions). The
     * priority hardly matters: the semaphores alone decide who runs. */
    if (LWP_SemInit(&coro->run, 0, 1) != 0 || LWP_SemInit(&coro->back, 0, 1) != 0)
    {
        port_fatal("LWP_SemInit failed");
        return coro;
    }
    if (LWP_CreateThread(&coro->thread, coro_worker, coro, coro->stack, (u32)stack_size, 64) != 0)
    {
        port_fatal("LWP_CreateThread failed");
    }
    trace('C', coro, 0);
    return coro;
}

void port_coro_destroy(PortCoro *coro)
{
    if (coro != NULL)
    {
        if (coro == sCurrentCoro)
        {
            port_fatal("coroutine destroyed itself");
        }
        if (!coro->finished)
        {
            /* Suspended or never started: unwind it to the worker loop. */
            coro->kill = 1;
            trace('D', coro, LWP_SemPost(coro->run));
            trace('B', coro, LWP_SemWait(coro->back));
        }
        coro->next_free = sFreeWorkers;
        sFreeWorkers = coro;
    }
}

void port_coro_resume(PortCoro *coro)
{
    PortCoro *previous = sCurrentCoro;

    if (coro->finished)
    {
        return;
    }
    sCurrentCoro = coro;
    trace('R', coro, LWP_SemPost(coro->run));
    trace('B', coro, LWP_SemWait(coro->back));
    sCurrentCoro = previous;
}

void port_coro_yield(void)
{
    PortCoro *coro = sCurrentCoro;

    if (coro == NULL)
    {
        port_fatal("port_coro_yield outside a coroutine");
    }
    trace('Y', coro, LWP_SemPost(coro->back));
    coro_wait_turn(coro);
}

int port_coro_finished(PortCoro *coro)
{
    return coro->finished;
}

void port_plat_coro_abandon(void)
{
    gPortFaulted = 1;
    if (sCurrentCoro != NULL)
    {
        longjmp(sCurrentCoro->unwind, 1); /* ends the coroutine through the worker loop */
    }
}

void port_set_fault_containment(int enable)
{
    /* Fatal errors raised by the port are contained; hardware faults are not caught here. */
    gPortContainFaults = enable;
}

#endif /* GEKKO */
