/*
 * Script bridge: what a game (and its mods) publish to Polyphase scripts and UI.
 *
 * Same model and names as the PS1 runtime's bridge (com.recomp.ps1), so scripts and tools
 * carry over between consoles; the Lua table here is `N64`.
 *
 * Game or mod code describes named variables and named requests, and the game calls
 * port_bridge_pump() once per frame at a point where its functions may be called.
 * Polyphase (Lua `N64.*`, C `n64_bridge_*`) then
 *  - reads variables straight from game memory, any time between frames;
 *  - writes variables and runs requests through a queue drained in port_bridge_pump(), so
 *    game code only ever runs on the game's own thread, between frames;
 *  - receives events the game emits (port_bridge_emit), e.g. to open a scene or a UI.
 *
 * Example (mod code):
 *
 *     static s32 sCoins;
 *     static int give(const int *args, int n) { if (n < 1) return PB_RESULT_BAD_ARGS; sCoins += args[0]; return sCoins; }
 *
 *     static const PortBridgeVar kVars[] = { { "coins", &sCoins, PB_S32, 1, 0, "coins collected" } };
 *     static const PortBridgeRequest kRequests[] = { { "give", give, "n: add n coins" } };
 *
 *     port_bridge_add(kVars, 1, kRequests, 1);          // once
 *     port_bridge_emit("coin", &sCoins, 1);             // whenever something happens
 *
 * Every variable can also be written: Lua N64.Set(name, value[, index]) queues the built-in
 * request "set <name>".
 */
#ifndef PORT_BRIDGE_H
#define PORT_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

/* variable types */
enum
{
    PB_U8 = 1,
    PB_S8,
    PB_U16,
    PB_S16,
    PB_U32,
    PB_S32,
    PB_STR, /* text: `count` strings of `stride` bytes each (NUL-terminated or full);
             * stride 0 = a single string of `count` bytes */
    PB_F32  /* written through N64.Set as value / 65536 (requests carry integers) */
};

/* Request results the bridge itself reports (handlers return their own values). */
#define PB_RESULT_UNKNOWN (-1000)  /* no such request or variable */
#define PB_RESULT_BAD_ARGS (-1001) /* wrong number of arguments / index out of range */

#define PB_MAX_ARGS 8

typedef struct PortBridgeVar
{
    const char *name;
    void *addr;     /* first element */
    int type;       /* PB_* */
    int count;      /* elements (array length); see PB_STR for text */
    int stride;     /* bytes between elements; 0 = the element size */
    const char *help;
} PortBridgeVar;

/* Runs on the game thread; returns a result for the script (>= 0 by convention for
 * success, negative for "not now" / errors the request documents). */
typedef int (*PortBridgeFn)(const int *args, int nargs);

typedef struct PortBridgeRequest
{
    const char *name;
    PortBridgeFn fn;
    const char *help; /* arguments and what it does, shown by N64.Requests() */
} PortBridgeRequest;

/* ---- game / mod side ------------------------------------------------------------- */

/* Publishes tables (they must stay valid: make them static). May be called several times,
 * once per mod; names should be unique. */
void port_bridge_add(const PortBridgeVar *vars, int nvars, const PortBridgeRequest *requests, int nrequests);
/* Runs the queued requests; call once per frame where game functions may be called. */
void port_bridge_pump(void);
/* Tells scripts that something happened (kept until polled; the oldest are dropped when
 * nobody listens). */
void port_bridge_emit(const char *name, const int *args, int nargs);

/* ---- host side (the embedding application; main thread, between frames) ------------ */

int n64_bridge_var_count(void);
const PortBridgeVar *n64_bridge_var(int index);
int n64_bridge_request_count(void);
const PortBridgeRequest *n64_bridge_request_info(int index);

/* Reads a variable: 1 = number in *value, 2 = text in text[], 0 = unknown / out of range. */
int n64_bridge_get(const char *name, int index, double *value, char *text, unsigned text_cap);
/* Queues a request ("set <name>" writes a variable: value[, index]). Returns its id (> 0)
 * or 0 when the queue is full. */
int n64_bridge_request(const char *name, const int *args, int nargs);
/* 1 and *result once the game has run request `id`. */
int n64_bridge_result(int id, int *result);
/* Takes the next event: 1 with its name and arguments, 0 when there is none. */
int n64_bridge_poll_event(char *name, unsigned name_cap, int *args, int max_args, int *nargs);

#ifdef __cplusplus
}
#endif

#endif /* PORT_BRIDGE_H */
