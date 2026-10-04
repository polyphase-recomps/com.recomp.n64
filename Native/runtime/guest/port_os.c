/*
 * libultra OS replacement: threads, message queues and events.
 *
 * Every OSThread is a coroutine. There are no host threads and no preemption
 * by time: a thread runs until it blocks on a message queue, lowers its
 * priority, stops, or wakes a higher-priority thread - the same points at
 * which the N64 kernel would switch. port_os_run() drives the scheduler from
 * the host until nothing but the idle thread is runnable.
 */
#include <port_types.h>
#include <PR/os.h>
#include <port_host.h>
#include "port_guest.h"

#define PORT_MAX_THREADS 256
#ifndef PORT_THREAD_STACK
#define PORT_THREAD_STACK (512 * 1024)
#endif

typedef struct PortThread
{
    OSThread *os;
    PortCoro *coro;
    void (*entry)(void *);
    void *arg;
    OSMesgQueue *wait_queue; /* queue this thread is blocked on, if any */
    sb32 is_wait_send;       /* blocked sending (queue full) rather than receiving */
    sb32 is_dead;
} PortThread;

static PortThread sThreads[PORT_MAX_THREADS];
static s32 sThreadsNum;
static PortThread *sCurrent; /* thread whose coroutine is executing, NULL in host context */

typedef struct PortEvent
{
    OSMesgQueue *mq;
    OSMesg msg;
} PortEvent;

static PortEvent sEvents[OS_NUM_EVENTS];

/* Threads parked by PORT_BUSY_WAIT() until the next video frame. */
static OSMesg sRetraceWaitMesg[1];
static OSMesgQueue sRetraceWaitQueue;
static s32 sRetraceWaiters;
static PortEvent sViEvent;

s32 osTvType = OS_TV_NTSC;
s32 osResetType = 0;

static PortThread *port_thread_find(OSThread *os)
{
    s32 i;

    for (i = 0; i < sThreadsNum; i++)
    {
        if (sThreads[i].os == os && !sThreads[i].is_dead)
        {
            return &sThreads[i];
        }
    }
    return NULL;
}

static PortThread *port_thread_alloc(void)
{
    s32 i;

    for (i = 0; i < sThreadsNum; i++)
    {
        if (sThreads[i].is_dead && sThreads[i].coro == NULL)
        {
            return &sThreads[i];
        }
    }
    if (sThreadsNum == PORT_MAX_THREADS)
    {
        port_fatal("out of thread slots");
    }
    return &sThreads[sThreadsNum++];
}

static void port_thread_entry(void *param)
{
    PortThread *thread = param;

    thread->entry(thread->arg);
    /* Returning from a thread function behaves like osDestroyThread(NULL). */
    thread->os->state = OS_STATE_STOPPED;
    thread->is_dead = TRUE;
}

/* Give the scheduler a chance to switch; only meaningful inside a thread. */
static void port_reschedule(void)
{
    gPortProgress[3]++;
    if (sCurrent != NULL)
    {
        port_coro_yield();
    }
}

static PortThread *port_pick_runnable(void)
{
    PortThread *best = NULL;
    s32 i;

    for (i = 0; i < sThreadsNum; i++)
    {
        PortThread *thread = &sThreads[i];

        if (thread->is_dead || thread->os->state != OS_STATE_RUNNABLE || thread->os->priority <= OS_PRIORITY_IDLE)
        {
            continue;
        }
        if (best == NULL || thread->os->priority > best->os->priority)
        {
            best = thread;
        }
    }
    return best;
}

/* Run threads until every one of them is blocked (or idle). Host context only. */
void port_os_run(void)
{
    PortThread *thread;
    s32 i;

    if (sCurrent != NULL)
    {
        port_fatal("port_os_run called from inside a thread");
    }
    while (!port_faulted() && (thread = port_pick_runnable()) != NULL)
    {
        sCurrent = thread;
        port_coro_resume(thread->coro);
        sCurrent = NULL;
    }
    if (port_faulted())
    {
        return; /* the faulting coroutine was abandoned; leave everything as it is */
    }
    /* Reap coroutines of threads that finished or were destroyed. */
    for (i = 0; i < sThreadsNum; i++)
    {
        if (sThreads[i].is_dead && sThreads[i].coro != NULL)
        {
            port_coro_destroy(sThreads[i].coro);
            sThreads[i].coro = NULL;
        }
    }
}

void port_os_shutdown(void)
{
    s32 i;

    for (i = 0; i < sThreadsNum; i++)
    {
        if (sThreads[i].coro != NULL)
        {
            port_coro_destroy(sThreads[i].coro);
        }
    }
    sThreadsNum = 0;
    sCurrent = NULL;
    port_memset(sThreads, 0, sizeof(sThreads));
    port_memset(sEvents, 0, sizeof(sEvents));
    port_memset(&sViEvent, 0, sizeof(sViEvent));
    port_memset(&sRetraceWaitQueue, 0, sizeof(sRetraceWaitQueue));
    sRetraceWaiters = 0;
}

void osInitialize(void)
{
}

void osCreateThread(OSThread *os, OSId id, void (*entry)(void *), void *arg, void *sp, OSPri pri)
{
    PortThread *thread = port_thread_find(os);

    if (thread != NULL)
    {
        /* Re-creating over a live OSThread (GObj thread pools do this): retire the old one. */
        thread->is_dead = TRUE;
    }
    thread = port_thread_alloc();
    thread->os = os;
    thread->entry = entry;
    thread->arg = arg;
    thread->wait_queue = NULL;
    thread->is_dead = FALSE;
    thread->coro = port_coro_create(port_thread_entry, thread, PORT_THREAD_STACK);

    os->id = id;
    os->priority = pri;
    os->state = OS_STATE_STOPPED;
    os->next = NULL;
    os->queue = NULL;
}

void osStartThread(OSThread *os)
{
    PortThread *thread = port_thread_find(os);

    if (thread == NULL)
    {
        port_fatal("osStartThread: unknown thread %p", os);
    }
    if (os->state == OS_STATE_STOPPED)
    {
        os->state = (thread->wait_queue != NULL) ? OS_STATE_WAITING : OS_STATE_RUNNABLE;
    }
    if (sCurrent != NULL && os->priority > sCurrent->os->priority)
    {
        port_reschedule();
    }
}

void osStopThread(OSThread *os)
{
    if (os == NULL)
    {
        if (sCurrent == NULL)
        {
            port_fatal("osStopThread(NULL) outside a thread");
        }
        sCurrent->os->state = OS_STATE_STOPPED;
        port_reschedule();
        return;
    }
    os->state = OS_STATE_STOPPED;
    if (sCurrent != NULL && sCurrent->os == os)
    {
        port_reschedule();
    }
}

void osDestroyThread(OSThread *os)
{
    PortThread *thread = (os == NULL) ? sCurrent : port_thread_find(os);

    if (thread == NULL)
    {
        return;
    }
    thread->os->state = OS_STATE_STOPPED;
    thread->wait_queue = NULL;
    thread->is_dead = TRUE;
    if (thread == sCurrent)
    {
        /* Never returns: the scheduler reaps the coroutine. */
        port_reschedule();
        port_fatal("destroyed thread was resumed");
    }
}

void osSetThreadPri(OSThread *os, OSPri pri)
{
    if (os == NULL)
    {
        if (sCurrent == NULL)
        {
            return;
        }
        os = sCurrent->os;
    }
    os->priority = pri;
    port_reschedule();
}

OSPri osGetThreadPri(OSThread *os)
{
    if (os == NULL)
    {
        os = (sCurrent != NULL) ? sCurrent->os : NULL;
    }
    return (os != NULL) ? os->priority : 0;
}

void osYieldThread(void)
{
    port_reschedule();
}

/* ---- message queues -------------------------------------------------------- */
void osCreateMesgQueue(OSMesgQueue *mq, OSMesg *msg, s32 count)
{
    mq->mtqueue = NULL;
    mq->fullqueue = NULL;
    mq->validCount = 0;
    mq->first = 0;
    mq->msgCount = count;
    mq->msg = msg;
}

/* Wake the highest-priority thread blocked on mq in the given direction. */
static PortThread *port_wake_waiter(OSMesgQueue *mq, sb32 is_send)
{
    PortThread *best = NULL;
    s32 i;

    for (i = 0; i < sThreadsNum; i++)
    {
        PortThread *thread = &sThreads[i];

        if (thread->is_dead || thread->wait_queue != mq || thread->is_wait_send != is_send)
        {
            continue;
        }
        if (best == NULL || thread->os->priority > best->os->priority)
        {
            best = thread;
        }
    }
    if (best != NULL)
    {
        best->wait_queue = NULL;
        if (best->os->state == OS_STATE_WAITING)
        {
            best->os->state = OS_STATE_RUNNABLE;
        }
    }
    return best;
}

static void port_block_on(OSMesgQueue *mq, sb32 is_send)
{
    if (sCurrent == NULL)
    {
        port_fatal("blocking message queue call from host context");
    }
    sCurrent->wait_queue = mq;
    sCurrent->is_wait_send = is_send;
    sCurrent->os->state = OS_STATE_WAITING;
    port_coro_yield();
}

static s32 port_put_mesg(OSMesgQueue *mq, OSMesg msg, s32 flag, sb32 is_jam)
{
    PortThread *woken;

    while (mq->validCount >= mq->msgCount)
    {
        if (flag != OS_MESG_BLOCK)
        {
            return -1;
        }
        port_block_on(mq, TRUE);
    }
    if (is_jam)
    {
        mq->first = (mq->first + mq->msgCount - 1) % mq->msgCount;
        mq->msg[mq->first] = msg;
    }
    else
    {
        mq->msg[(mq->first + mq->validCount) % mq->msgCount] = msg;
    }
    mq->validCount++;

    woken = port_wake_waiter(mq, FALSE);
    if (woken != NULL && sCurrent != NULL && woken->os->priority > sCurrent->os->priority)
    {
        port_reschedule();
    }
    return 0;
}

s32 osSendMesg(OSMesgQueue *mq, OSMesg msg, s32 flag)
{
    gPortProgress[1]++;
    return port_put_mesg(mq, msg, flag, FALSE);
}

s32 osJamMesg(OSMesgQueue *mq, OSMesg msg, s32 flag)
{
    return port_put_mesg(mq, msg, flag, TRUE);
}

s32 osRecvMesg(OSMesgQueue *mq, OSMesg *msg, s32 flag)
{
    PortThread *woken;

    gPortProgress[0]++;

    while (mq->validCount == 0)
    {
        if (flag != OS_MESG_BLOCK)
        {
            return -1;
        }
        port_block_on(mq, FALSE);
    }
    if (msg != NULL)
    {
        *msg = mq->msg[mq->first];
    }
    mq->first = (mq->first + 1) % mq->msgCount;
    mq->validCount--;

    woken = port_wake_waiter(mq, TRUE);
    if (woken != NULL && sCurrent != NULL && woken->os->priority > sCurrent->os->priority)
    {
        port_reschedule();
    }
    return 0;
}

/* ---- events ------------------------------------------------------------------ */
void osSetEventMesg(OSEvent event, OSMesgQueue *mq, OSMesg msg)
{
    if (event < OS_NUM_EVENTS)
    {
        sEvents[event].mq = mq;
        sEvents[event].msg = msg;
    }
}

void osViSetEvent(OSMesgQueue *mq, OSMesg msg, u32 retrace_count)
{
    sViEvent.mq = mq;
    sViEvent.msg = msg;
}

/* Post a hardware event. Never blocks: a full queue drops the event, as on hardware. */
void port_os_post_event(s32 event)
{
    if (event < OS_NUM_EVENTS && sEvents[event].mq != NULL)
    {
        osSendMesg(sEvents[event].mq, sEvents[event].msg, OS_MESG_NOBLOCK);
    }
}

void port_os_wait_retrace(void)
{
    if (sRetraceWaitQueue.msg == NULL)
    {
        osCreateMesgQueue(&sRetraceWaitQueue, sRetraceWaitMesg, 1);
    }
    sRetraceWaiters++;
    osRecvMesg(&sRetraceWaitQueue, NULL, OS_MESG_BLOCK);
    sRetraceWaiters--;
}

void port_os_post_vi_retrace(void)
{
    if (sRetraceWaiters > 0)
    {
        osSendMesg(&sRetraceWaitQueue, NULL, OS_MESG_NOBLOCK);
    }
    if (sViEvent.mq != NULL)
    {
        osSendMesg(sViEvent.mq, sViEvent.msg, OS_MESG_NOBLOCK);
    }
}

OSIntMask osSetIntMask(OSIntMask mask)
{
    return mask;
}
