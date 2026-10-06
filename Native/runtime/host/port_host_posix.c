/*
 * POSIX backend (Linux, Android, and anything else with pthreads): each coroutine is a thread
 * that only ever runs while its resumer waits, so the game stays single-threaded in effect.
 * Faults are not contained here; a crash ends the process.
 */
#if !defined(_WIN32) && !defined(GEKKO) && !defined(__3DS__)
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "port_host.h"
#include "port_host_plat.h"

#ifndef PTHREAD_STACK_MIN /* (newlib on the 3DS) */
#define PTHREAD_STACK_MIN 16384
#endif

void port_plat_abort(void)
{
    abort();
}

/* ---- arena ----------------------------------------------------------------- */
#if UINTPTR_MAX > 0xFFFFFFFFu
#include <sys/mman.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

extern char __ehdr_start __attribute__((visibility("hidden")));

/* Tokens are 32-bit offsets from the image base, so the arena has to sit within 4 GB above it. */
unsigned char *port_plat_arena_reserve(unsigned long long size)
{
    uintptr_t base = ((uintptr_t)&__ehdr_start + 0x40000000u) & ~(uintptr_t)0xFFFFFFF;
    uintptr_t limit = (uintptr_t)&__ehdr_start + 0xFFFFFFFFull - size;

    for (; base < limit; base += 0x10000000u)
    {
        void *got = mmap((void *)base, size, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);

        if (got == (void *)base)
        {
            return got;
        }
        if (got != MAP_FAILED)
        {
            munmap(got, size); /* a kernel without MAP_FIXED_NOREPLACE put it elsewhere */
        }
    }
    return NULL;
}
#else
/* 32-bit: a token is the pointer itself, any memory will do. */
unsigned char *port_plat_arena_reserve(unsigned long long size)
{
    return calloc(1, (size_t)size);
}
#endif

int port_plat_arena_commit(unsigned char *addr, unsigned long long size)
{
    return 1; /* the whole reservation is usable and reads as zero */
}

#if defined(__3DS__)
/* ---- coroutines (3DS) ------------------------------------------------------- */
/* newlib's pthread condition variables do not hand control over reliably here: each
 * coroutine is a libctru thread, and control passes with two light semaphores. */
#include <3ds.h>

struct PortCoro
{
    Thread thread;
    LightSemaphore go;   /* released to let the coroutine run */
    LightSemaphore back; /* released when it yields or finishes */
    void (*entry)(void *);
    void *arg;
    volatile int kill;   /* destroyed while suspended: unwind instead of continuing */
    volatile int finished;
};

static PortCoro *sCurrentCoro;

static void coro_wait_turn(PortCoro *coro)
{
    LightSemaphore_Acquire(&coro->go, 1);
    if (coro->kill)
    {
        threadExit(0);
    }
}

static void coro_trampoline(void *param)
{
    PortCoro *coro = param;

    coro_wait_turn(coro);
    coro->entry(coro->arg);
    coro->finished = 1;
    LightSemaphore_Release(&coro->back, 1);
}

PortCoro *port_coro_create(void (*entry)(void *), void *arg, unsigned long long stack_size)
{
    PortCoro *coro = calloc(1, sizeof(*coro));
    s32 priority = 0x30;

    coro->entry = entry;
    coro->arg = arg;
    LightSemaphore_Init(&coro->go, 0, 1);
    LightSemaphore_Init(&coro->back, 0, 1);
    /* the same priority and core as the caller: only one of them runs at a time anyway */
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    coro->thread = threadCreate(coro_trampoline, coro, (size_t)((stack_size + 7) & ~7ull), priority, -2, false);
    if (coro->thread == NULL)
    {
        port_fatal("threadCreate failed (%u KB stack)", (unsigned)(stack_size >> 10));
    }
    return coro;
}

void port_coro_destroy(PortCoro *coro)
{
    if (coro != NULL)
    {
        if (!coro->finished)
        {
            coro->kill = 1;
            LightSemaphore_Release(&coro->go, 1);
        }
        threadJoin(coro->thread, U64_MAX);
        threadFree(coro->thread);
        free(coro);
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
    LightSemaphore_Release(&coro->go, 1);
    LightSemaphore_Acquire(&coro->back, 1);
    sCurrentCoro = previous;
}

void port_coro_yield(void)
{
    PortCoro *coro = sCurrentCoro;

    if (coro == NULL)
    {
        port_fatal("port_coro_yield outside a coroutine");
    }
    LightSemaphore_Release(&coro->back, 1);
    coro_wait_turn(coro);
}

int port_coro_finished(PortCoro *coro)
{
    return coro->finished;
}

void port_plat_coro_abandon(void)
{
    PortCoro *coro = sCurrentCoro;

    gPortFaulted = 1;
    if (coro != NULL)
    {
        /* Never resumed again; the thread ends here and is reaped by port_coro_destroy(). */
        coro->finished = 1;
        LightSemaphore_Release(&coro->back, 1);
        threadExit(0);
    }
}

#else
/* ---- coroutines ------------------------------------------------------------ */
struct PortCoro
{
    pthread_t thread;
    pthread_cond_t wake;  /* signalled when `run` or `back` changes */
    void (*entry)(void *);
    void *arg;
    int run;      /* the coroutine has been told to continue */
    int back;     /* the coroutine has handed control back */
    int kill;     /* destroyed while suspended: unwind instead of continuing */
    int finished;
};

static pthread_mutex_t sLock = PTHREAD_MUTEX_INITIALIZER;
static PortCoro *sCurrentCoro;

/* Called on the coroutine's thread with sLock held: sleep until resumed. */
static void coro_wait_turn(PortCoro *coro)
{
    while (!coro->run)
    {
        pthread_cond_wait(&coro->wake, &sLock);
    }
    coro->run = 0;
    if (coro->kill)
    {
        pthread_mutex_unlock(&sLock);
        pthread_exit(NULL);
    }
}

static void coro_hand_back(PortCoro *coro)
{
    coro->back = 1;
    pthread_cond_broadcast(&coro->wake);
}

static void *coro_trampoline(void *param)
{
    PortCoro *coro = param;

    pthread_mutex_lock(&sLock);
    coro_wait_turn(coro);
    pthread_mutex_unlock(&sLock);

    coro->entry(coro->arg);

    pthread_mutex_lock(&sLock);
    coro->finished = 1;
    coro_hand_back(coro);
    pthread_mutex_unlock(&sLock);
    return NULL;
}

PortCoro *port_coro_create(void (*entry)(void *), void *arg, unsigned long long stack_size)
{
    PortCoro *coro = calloc(1, sizeof(*coro));
    pthread_attr_t attr;

    coro->entry = entry;
    coro->arg = arg;
    pthread_cond_init(&coro->wake, NULL);
    pthread_attr_init(&attr);
    if (stack_size < PTHREAD_STACK_MIN)
    {
        stack_size = PTHREAD_STACK_MIN;
    }
    pthread_attr_setstacksize(&attr, (size_t)stack_size);
    if (pthread_create(&coro->thread, &attr, coro_trampoline, coro) != 0)
    {
        port_fatal("pthread_create failed");
    }
    pthread_attr_destroy(&attr);
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
        pthread_mutex_lock(&sLock);
        coro->kill = 1;
        coro->run = 1;
        pthread_cond_broadcast(&coro->wake);
        pthread_mutex_unlock(&sLock);
        pthread_join(coro->thread, NULL);
        pthread_cond_destroy(&coro->wake);
        free(coro);
    }
}

void port_coro_resume(PortCoro *coro)
{
    PortCoro *previous = sCurrentCoro;

    if (coro->finished)
    {
        return;
    }
    pthread_mutex_lock(&sLock);
    sCurrentCoro = coro;
    coro->run = 1;
    pthread_cond_broadcast(&coro->wake);
    while (!coro->back)
    {
        pthread_cond_wait(&coro->wake, &sLock);
    }
    coro->back = 0;
    sCurrentCoro = previous;
    pthread_mutex_unlock(&sLock);
}

void port_coro_yield(void)
{
    PortCoro *coro = sCurrentCoro;

    if (coro == NULL)
    {
        port_fatal("port_coro_yield outside a coroutine");
    }
    pthread_mutex_lock(&sLock);
    coro_hand_back(coro);
    coro_wait_turn(coro);
    pthread_mutex_unlock(&sLock);
}

int port_coro_finished(PortCoro *coro)
{
    return coro->finished;
}

void port_plat_coro_abandon(void)
{
    PortCoro *coro = sCurrentCoro;

    gPortFaulted = 1;
    if (coro != NULL)
    {
        /* Never resumed again; the thread ends here and is reaped by port_coro_destroy(). */
        pthread_mutex_lock(&sLock);
        coro->finished = 1;
        coro_hand_back(coro);
        pthread_mutex_unlock(&sLock);
        pthread_exit(NULL);
    }
}

#endif /* __3DS__ */

void port_set_fault_containment(int enable)
{
    /* Fatal errors raised by the port are contained; hardware faults are not caught here. */
    gPortContainFaults = enable;
}

#endif /* !_WIN32 && !GEKKO */
