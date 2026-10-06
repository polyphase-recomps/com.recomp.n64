/*
 * Nintendo 3DS backend (devkitARM + libctru). Coroutines switch stacks in place on the calling
 * thread (a few instructions, no kernel threads), so the game runs on whatever thread drives
 * it - the engine's main thread inside Polyphase - and only one coroutine ever runs at a time.
 * Pointers are 32-bit, so the arena can be any memory.
 */
#if defined(__3DS__)
#include <3ds.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include "port_host.h"
#include "port_host_plat.h"

void port_plat_abort(void)
{
    abort();
}

unsigned long long port_ticks(void)
{
    return svcGetSystemTick();
}

/* ---- arena ----------------------------------------------------------------- */
/* The heap first; the engine and the executable leave little of it on an Old 3DS, so the
 * linear (GPU-visible) heap is the fallback. */
unsigned char *port_plat_arena_reserve(unsigned long long size)
{
    unsigned char *base = memalign(16, (size_t)size);
    const char *where = "heap";

    if (base == NULL)
    {
        base = linearMemAlign((size_t)size, 16);
        where = "linear heap";
    }
    port_log("arena: %u KB from the %s (linear heap free %u KB, application memory free %u KB)",
             (unsigned)(size >> 10), (base != NULL) ? where : "NOWHERE", (unsigned)(linearSpaceFree() >> 10),
             (unsigned)(osGetMemRegionFree(MEMREGION_APPLICATION) >> 10));
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
 * n64port_ctr_switch(save, load) stores the callee-saved registers (r4-r11, the VFP d8-d15, lr)
 * on the current stack, writes the stack pointer to *save and continues on the stack `load`,
 * returning from that stack's own n64port_ctr_switch call. A new coroutine's stack is set up to
 * "return" into coro_start.
 */
__asm__(
    ".text\n"
    ".arm\n"
    ".align 2\n"
    ".global n64port_ctr_switch\n"
    ".type n64port_ctr_switch, %function\n"
    "n64port_ctr_switch:\n"
    "    push {r4-r11, ip, lr}\n" /* ten words: keeps the stack 8-byte aligned */
    "    vpush {d8-d15}\n"
    "    str sp, [r0]\n"
    "    mov sp, r1\n"
    "    vpop {d8-d15}\n"
    "    pop {r4-r11, ip, lr}\n"
    "    bx lr\n"
    ".size n64port_ctr_switch, .-n64port_ctr_switch\n");

void n64port_ctr_switch(void **save, void *load);

#define SWITCH_FRAME (64 + 10 * 4) /* d8-d15, then r4-r11, ip, lr */

struct PortCoro
{
    void *sp;        /* saved stack pointer while suspended */
    void *caller_sp; /* stack pointer of whoever resumed it */
    unsigned char *stack;
    unsigned int stack_size;
    void (*entry)(void *);
    void *arg;
    int finished;
    struct PortCoro *next_free; /* idle coroutines keep their stack for the next one */
};

static PortCoro *sCurrentCoro;
static PortCoro *sFreeCoros;

static void coro_start(void)
{
    PortCoro *coro = sCurrentCoro;
    void *dead;

    coro->entry(coro->arg);
    coro->finished = 1;
    n64port_ctr_switch(&dead, coro->caller_sp); /* never comes back */
    abort();
}

static void coro_prepare(PortCoro *coro)
{
    unsigned char *top = coro->stack + (coro->stack_size & ~7u);
    unsigned int *frame = (unsigned int *)(top - SWITCH_FRAME);

    memset(frame, 0, SWITCH_FRAME);
    frame[SWITCH_FRAME / 4 - 1] = (unsigned int)(uintptr_t)coro_start; /* lr */
    coro->sp = frame;
    coro->finished = 0;
}

PortCoro *port_coro_create(void (*entry)(void *), void *arg, unsigned long long stack_size)
{
    PortCoro *coro, **link;

    if (stack_size < 0x8000)
    {
        stack_size = 0x8000;
    }
    /* reuse an idle coroutine whose stack is big enough */
    for (link = &sFreeCoros; *link != NULL; link = &(*link)->next_free)
    {
        if ((*link)->stack_size >= stack_size)
        {
            coro = *link;
            *link = coro->next_free;
            coro->next_free = NULL;
            coro->entry = entry;
            coro->arg = arg;
            coro_prepare(coro);
            return coro;
        }
    }
    coro = calloc(1, sizeof(*coro));
    if (coro != NULL)
    {
        coro->stack = memalign(8, (size_t)stack_size);
        if (coro->stack == NULL)
        {
            coro->stack = linearMemAlign((size_t)stack_size, 8);
        }
    }
    if (coro == NULL || coro->stack == NULL)
    {
        port_fatal("out of memory for a %u KB thread stack", (unsigned)(stack_size >> 10));
        return coro;
    }
    coro->stack_size = (unsigned int)stack_size;
    coro->entry = entry;
    coro->arg = arg;
    coro_prepare(coro);
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
        /* Nothing on a suspended stack needs unwinding: it is simply never resumed again. */
        coro->finished = 1;
        coro->next_free = sFreeCoros;
        sFreeCoros = coro;
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
    n64port_ctr_switch(&coro->caller_sp, coro->sp);
    sCurrentCoro = previous;
}

void port_coro_yield(void)
{
    PortCoro *coro = sCurrentCoro;

    if (coro == NULL)
    {
        port_fatal("port_coro_yield outside a coroutine");
    }
    n64port_ctr_switch(&coro->sp, coro->caller_sp);
}

int port_coro_finished(PortCoro *coro)
{
    return coro->finished;
}

void port_plat_coro_abandon(void)
{
    PortCoro *coro = sCurrentCoro;
    void *dead;

    gPortFaulted = 1;
    if (coro != NULL)
    {
        coro->finished = 1;
        n64port_ctr_switch(&dead, coro->caller_sp); /* back to the resumer, never resumed again */
    }
}

void port_set_fault_containment(int enable)
{
    /* Fatal errors raised by the port are contained; hardware faults are not caught here. */
    gPortContainFaults = enable;
}

#endif /* __3DS__ */
