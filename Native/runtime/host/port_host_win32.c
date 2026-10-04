/* Windows backend: arena next to the module image, fibers, vectored fault containment. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>

#include "port_host.h"
#include "port_host_plat.h"

extern char __ImageBase;

static void *sFaultHandler;

void port_plat_abort(void)
{
    /* Raise instead of abort() so an installed crash handler can print the guest call stack. */
    RaiseException(0xE0550064, EXCEPTION_NONCONTINUABLE, 0, NULL);
    abort();
}

/* The reservation goes just above the module image, within 4 GB of __ImageBase. */
unsigned char *port_plat_arena_reserve(unsigned long long size)
{
    MEMORY_BASIC_INFORMATION info;
    unsigned char *addr = (unsigned char *)&__ImageBase;
    unsigned char *limit = addr + (0xFFFFFFFFull - size);

    while (addr < limit && VirtualQuery(addr, &info, sizeof(info)) != 0)
    {
        if (info.State == MEM_FREE && info.RegionSize >= size + 0x10000)
        {
            unsigned char *want = (unsigned char *)(((ULONG_PTR)info.BaseAddress + 0xFFFF) & ~(ULONG_PTR)0xFFFF);
            unsigned char *base = VirtualAlloc(want, size, MEM_RESERVE, PAGE_READWRITE);

            if (base != NULL)
            {
                return base;
            }
        }
        addr = (unsigned char *)info.BaseAddress + info.RegionSize;
    }
    return NULL;
}

int port_plat_arena_commit(unsigned char *addr, unsigned long long size)
{
    return VirtualAlloc(addr, size, MEM_COMMIT, PAGE_READWRITE) != NULL;
}

/* ---- coroutines (Win32 fibers) ------------------------------------------- */
struct PortCoro
{
    void *fiber;
    void *resumer;
    void (*entry)(void *);
    void *arg;
    int finished;
};

static PortCoro *sCurrentCoro;

static void CALLBACK coro_trampoline(void *param)
{
    PortCoro *coro = param;

    coro->entry(coro->arg);
    coro->finished = 1;
    SwitchToFiber(coro->resumer);
    port_fatal("resumed a finished coroutine");
}

PortCoro *port_coro_create(void (*entry)(void *), void *arg, unsigned long long stack_size)
{
    PortCoro *coro = calloc(1, sizeof(*coro));

    coro->entry = entry;
    coro->arg = arg;
    coro->fiber = CreateFiberEx(0, (SIZE_T)stack_size, FIBER_FLAG_FLOAT_SWITCH, coro_trampoline, coro);
    if (coro->fiber == NULL)
    {
        port_fatal("CreateFiber failed (%lu)", GetLastError());
    }
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
        DeleteFiber(coro->fiber);
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
    if (!IsThreadAFiber())
    {
        ConvertThreadToFiberEx(NULL, FIBER_FLAG_FLOAT_SWITCH);
    }
    coro->resumer = GetCurrentFiber();
    sCurrentCoro = coro;
    SwitchToFiber(coro->fiber);
    sCurrentCoro = previous;
}

void port_plat_coro_abandon(void)
{
    gPortFaulted = 1;
    if (sCurrentCoro != NULL)
    {
        PortCoro *coro = sCurrentCoro;

        coro->finished = 1; /* never resumed again; its fiber is leaked on purpose */
        SwitchToFiber(coro->resumer);
    }
}

static LONG CALLBACK port_fault_filter(EXCEPTION_POINTERS *info)
{
    DWORD code = info->ExceptionRecord->ExceptionCode;

    if (code < 0xC0000000 || sCurrentCoro == NULL || gPortFaultGuard != 0)
    {
        return EXCEPTION_CONTINUE_SEARCH; /* not a hardware fault, or not in game code */
    }
    port_log("game fault: exception 0x%08lX at %p (address %p); runtime stopped", code,
             info->ExceptionRecord->ExceptionAddress,
             (code == EXCEPTION_ACCESS_VIOLATION) ? (void *)info->ExceptionRecord->ExceptionInformation[1] : NULL);
    port_plat_coro_abandon();
    return EXCEPTION_CONTINUE_SEARCH;
}

void port_set_fault_containment(int enable)
{
    gPortContainFaults = enable;
    if (enable && sFaultHandler == NULL)
    {
        sFaultHandler = AddVectoredExceptionHandler(1, port_fault_filter);
    }
    else if (!enable && sFaultHandler != NULL)
    {
        RemoveVectoredExceptionHandler(sFaultHandler);
        sFaultHandler = NULL;
    }
}

void port_coro_yield(void)
{
    if (sCurrentCoro == NULL)
    {
        port_fatal("port_coro_yield outside a coroutine");
    }
    SwitchToFiber(sCurrentCoro->resumer);
}

int port_coro_finished(PortCoro *coro)
{
    return coro->finished;
}

#endif /* _WIN32 */
