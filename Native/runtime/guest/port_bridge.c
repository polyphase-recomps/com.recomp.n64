/*
 * Script bridge (see port_bridge.h): the tables games and mods publish, the queue of requests
 * the host hands to the game, and the queue of events the game hands to the host. The game's
 * threads run as coroutines of the host's thread, so none of this needs locking.
 */
#include <port_bridge.h>
#include <port_host.h>

#define MAX_TABLES 32
#define MAX_QUEUE 32
#define MAX_RESULTS 64
#define MAX_EVENTS 64
#define NAME_CAP 48

typedef struct BridgeTable
{
    const PortBridgeVar *vars;
    int nvars;
    const PortBridgeRequest *requests;
    int nrequests;
} BridgeTable;

typedef struct BridgeCall
{
    int id;
    char name[NAME_CAP];
    int args[PB_MAX_ARGS], nargs;
} BridgeCall;

static BridgeTable sTables[MAX_TABLES];
static int sTableCount;

static BridgeCall sQueue[MAX_QUEUE];
static int sQueueHead, sQueueCount;
static int sNextId = 1;

static struct { int id, result; } sResults[MAX_RESULTS];
static int sResultPos;

static BridgeCall sEvents[MAX_EVENTS];
static int sEventHead, sEventCount;

static int same(const char *a, const char *b)
{
    while (*a && *a == *b) a++, b++;
    return *a == *b;
}

static void copy_name(char *dst, unsigned cap, const char *src)
{
    unsigned i = 0;

    if (cap == 0) return;
    for (; src[i] != 0 && i + 1 < cap; i++) dst[i] = src[i];
    dst[i] = 0;
}

static int element_size(int type)
{
    switch (type)
    {
    case PB_U16: case PB_S16: return 2;
    case PB_U32: case PB_S32: case PB_F32: return 4;
    default: return 1;
    }
}

static const PortBridgeVar *find_var(const char *name)
{
    int t, i;

    for (t = 0; t < sTableCount; t++)
    {
        for (i = 0; i < sTables[t].nvars; i++)
        {
            if (same(sTables[t].vars[i].name, name)) return &sTables[t].vars[i];
        }
    }
    return 0;
}

void port_bridge_add(const PortBridgeVar *vars, int nvars, const PortBridgeRequest *requests, int nrequests)
{
    if (sTableCount >= MAX_TABLES)
    {
        port_log("bridge: too many tables");
        return;
    }
    sTables[sTableCount].vars = vars;
    sTables[sTableCount].nvars = nvars;
    sTables[sTableCount].requests = requests;
    sTables[sTableCount].nrequests = nrequests;
    sTableCount++;
}

/* "set <name>": args value[, index] */
static int set_variable(const char *name, const int *args, int nargs)
{
    const PortBridgeVar *v = find_var(name);
    int index = (nargs > 1) ? args[1] : 0;
    unsigned char *p;

    if (v == 0) return PB_RESULT_UNKNOWN;
    if (nargs < 1 || v->type == PB_STR || index < 0 || index >= v->count) return PB_RESULT_BAD_ARGS;
    p = (unsigned char *)v->addr + index * (v->stride ? v->stride : element_size(v->type));
    switch (v->type)
    {
    case PB_U8: case PB_S8: *p = (unsigned char)args[0]; break;
    case PB_U16: case PB_S16: *(unsigned short *)p = (unsigned short)args[0]; break;
    case PB_F32: *(float *)p = (float)args[0] / 65536.0f; break;
    default: *(unsigned *)p = (unsigned)args[0]; break;
    }
    return 0;
}

void port_bridge_pump(void)
{
    while (sQueueCount > 0)
    {
        BridgeCall *call = &sQueue[sQueueHead];
        int result = PB_RESULT_UNKNOWN, t, i, found = 0;

        sQueueHead = (sQueueHead + 1) % MAX_QUEUE;
        sQueueCount--;
        if (call->name[0] == 's' && call->name[1] == 'e' && call->name[2] == 't' && call->name[3] == ' ')
        {
            result = set_variable(call->name + 4, call->args, call->nargs);
        }
        else
        {
            for (t = 0; t < sTableCount && !found; t++)
            {
                for (i = 0; i < sTables[t].nrequests; i++)
                {
                    if (same(sTables[t].requests[i].name, call->name))
                    {
                        result = sTables[t].requests[i].fn(call->args, call->nargs);
                        found = 1;
                        break;
                    }
                }
            }
        }
        sResults[sResultPos].id = call->id;
        sResults[sResultPos].result = result;
        sResultPos = (sResultPos + 1) % MAX_RESULTS;
    }
}

void port_bridge_emit(const char *name, const int *args, int nargs)
{
    BridgeCall *event;
    int i;

    if (sEventCount == MAX_EVENTS)
    {
        sEventHead = (sEventHead + 1) % MAX_EVENTS; /* nobody is listening: drop the oldest */
        sEventCount--;
    }
    event = &sEvents[(sEventHead + sEventCount) % MAX_EVENTS];
    sEventCount++;
    copy_name(event->name, NAME_CAP, name);
    event->nargs = (nargs > PB_MAX_ARGS) ? PB_MAX_ARGS : nargs;
    for (i = 0; i < event->nargs; i++) event->args[i] = args[i];
}

/* ---- host side --------------------------------------------------------------------- */

static const void *nth_entry(int index, int want_requests)
{
    int t;

    for (t = 0; t < sTableCount; t++)
    {
        int n = want_requests ? sTables[t].nrequests : sTables[t].nvars;

        if (index < n)
        {
            return want_requests ? (const void *)&sTables[t].requests[index] : (const void *)&sTables[t].vars[index];
        }
        index -= n;
    }
    return 0;
}

int n64_bridge_var_count(void)
{
    int t, n = 0;

    for (t = 0; t < sTableCount; t++) n += sTables[t].nvars;
    return n;
}

const PortBridgeVar *n64_bridge_var(int index)
{
    return (index < 0) ? 0 : (const PortBridgeVar *)nth_entry(index, 0);
}

int n64_bridge_request_count(void)
{
    int t, n = 0;

    for (t = 0; t < sTableCount; t++) n += sTables[t].nrequests;
    return n;
}

const PortBridgeRequest *n64_bridge_request_info(int index)
{
    return (index < 0) ? 0 : (const PortBridgeRequest *)nth_entry(index, 1);
}

int n64_bridge_get(const char *name, int index, double *value, char *text, unsigned text_cap)
{
    const PortBridgeVar *v = find_var(name);
    const unsigned char *p;

    if (v == 0 || index < 0) return 0;
    if (v->type == PB_STR)
    {
        unsigned len = (unsigned)(v->stride ? v->stride : v->count), i;

        if (v->stride != 0 && index >= v->count) return 0;
        if (v->stride == 0 && index != 0) return 0;
        p = (const unsigned char *)v->addr + index * v->stride;
        if (text == 0 || text_cap == 0) return 0;
        for (i = 0; i < len && i + 1 < text_cap && p[i] != 0; i++) text[i] = (char)p[i];
        text[i] = 0;
        return 2;
    }
    if (index >= v->count) return 0;
    p = (const unsigned char *)v->addr + index * (v->stride ? v->stride : element_size(v->type));
    switch (v->type)
    {
    case PB_U8: *value = *p; break;
    case PB_S8: *value = *(const signed char *)p; break;
    case PB_U16: *value = *(const unsigned short *)p; break;
    case PB_S16: *value = *(const short *)p; break;
    case PB_U32: *value = *(const unsigned *)p; break;
    case PB_S32: *value = *(const int *)p; break;
    case PB_F32: *value = *(const float *)p; break;
    default: return 0;
    }
    return 1;
}

int n64_bridge_request(const char *name, const int *args, int nargs)
{
    BridgeCall *call;
    int i;

    if (sQueueCount == MAX_QUEUE) return 0;
    call = &sQueue[(sQueueHead + sQueueCount) % MAX_QUEUE];
    sQueueCount++;
    call->id = sNextId++;
    if (sNextId <= 0) sNextId = 1;
    copy_name(call->name, NAME_CAP, name);
    call->nargs = (nargs > PB_MAX_ARGS) ? PB_MAX_ARGS : nargs;
    for (i = 0; i < call->nargs; i++) call->args[i] = args[i];
    return call->id;
}

int n64_bridge_result(int id, int *result)
{
    int i;

    for (i = 0; i < MAX_RESULTS; i++)
    {
        if (sResults[i].id == id && id != 0)
        {
            *result = sResults[i].result;
            return 1;
        }
    }
    return 0;
}

int n64_bridge_poll_event(char *name, unsigned name_cap, int *args, int max_args, int *nargs)
{
    BridgeCall *event;
    int i;

    if (sEventCount == 0) return 0;
    event = &sEvents[sEventHead];
    sEventHead = (sEventHead + 1) % MAX_EVENTS;
    sEventCount--;
    copy_name(name, name_cap, event->name);
    *nargs = (event->nargs > max_args) ? max_args : event->nargs;
    for (i = 0; i < *nargs; i++) args[i] = event->args[i];
    return 1;
}
