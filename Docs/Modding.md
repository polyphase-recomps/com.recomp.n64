# Modding N64 recomp games

This covers every game built on `com.recomp.n64` (Super Smash Bros. is the reference game).
What a specific game publishes and where its hooks are is in that game's package, e.g.
[`com.recomp.ssb64/Docs/Modding.md`](../../com.recomp.ssb64/Docs/Modding.md).

A recompiled game is the game's own C code (from its decompilation) running on this runtime.
A mod is therefore ordinary C compiled into the game, and it runs on every target
(Windows, Linux, Wii, GameCube, ...). The same model and names are used by the PS1 runtime
(`com.recomp.ps1`, Lua table `Ps1`), so scripts and habits carry over.

| You want to... | Use |
|---|---|
| Change a rule of the game, add a cheat or a feature | **Mod code** (C, in the game package's `Native/mods/`) |
| Show game state in a Polyphase UI (HUD, menus, overlays) | **Variables** published over the script bridge, read from Lua |
| Trigger something in the game from Polyphase (a button, a script) | **Requests** |
| Open a Polyphase scene or UI when something happens in the game | **Events** |

## The script bridge

```
 Lua / UI (main thread)                         game (its own thread, once per frame)
 N64.Get("player_damage", 0) ─ reads memory ─►  variables the game / mods published
 N64.Request("win", 0)  ─► queue ─────────────► port_bridge_pump() runs the handler
 N64.Result(id)         ◄─ result ◄────────────  handler's return value
 N64.PollEvent()        ◄─ queue ◄─────────────  port_bridge_emit("ko", args, 2)
```

Game code only ever runs on the game's thread, between two frames; scripts never call into
it directly. That is why a request's result arrives a frame later.

### Lua: the `N64` table

```lua
N64.IsRunning()                 -- true while the game runs
N64.Get(name [, index])         -- number, or string for text; nil if unavailable
N64.Set(name, value [, index])  -- queues a write; returns a request id (or nil)
N64.Request(name, ...)          -- queues a request with integer arguments; id or nil
N64.Result(id)                  -- result once the game ran it, else nil
N64.PollEvent()                 -- next event: name, arg1, arg2, ...  (nil when none)
N64.Variables()                 -- { {name=, type=, count=, help=}, ... }
N64.Requests()                  -- { {name=, help=}, ... }
```

Events are a single queue: have one script poll it and hand events on, e.g.

```lua
function GameEvents:Tick(dt)
    while true do
        local name, a, b = N64.PollEvent()
        if name == nil then break end
        if name == "battle_end" then
            self:OpenResultsUi()          -- load a scene, show a widget, ...
        elseif name == "ko" then
            self.hud:FlashPlayer(a)
        end
    end
    self.damageText:SetText(N64.Get("player_damage", 0) .. "%")
end
```

Float variables read as numbers; `N64.Set` on a float takes the plain value
(`N64.Set("speed", 1.5)`).

### C: publishing from game or mod code (`port_bridge.h`)

```c
#include <port_bridge.h>

static s32 sCoins;

static int give(const int *args, int nargs)      /* runs on the game thread */
{
    if (nargs < 1) return PB_RESULT_BAD_ARGS;
    sCoins += args[0];
    return sCoins;                               /* what N64.Result(id) gets */
}

static const PortBridgeVar kVars[] = {
    { "coins", &sCoins, PB_S32, 1, 0, "coins collected" },
};
static const PortBridgeRequest kRequests[] = {
    { "give", give, "n: add n coins" },
};

port_bridge_add(kVars, 1, kRequests, 1);         /* once */
port_bridge_emit("coin", &sCoins, 1);            /* when something happens */
```

- Variable types: `PB_U8 PB_S8 PB_U16 PB_S16 PB_U32 PB_S32 PB_F32 PB_STR`. `count` is the
  array length (`N64.Get(name, index)`), `stride` the bytes between elements when they sit
  inside a larger struct (0 = packed).
- A variable's address must stay valid. State that lives in objects the game creates and
  destroys (fighters, the running match) should be **mirrored** into a static variable each
  frame, or changed through a request.
- Results: `>= 0` success by convention, negative for "not now"; the bridge itself returns
  `PB_RESULT_UNKNOWN` (-1000) and `PB_RESULT_BAD_ARGS` (-1001).
- The game must call `port_bridge_pump()` once per frame from a place where its functions
  may be called. A game package does this in its mod entry point.

### C++/other hosts

`n64_bridge_var_count / n64_bridge_var / n64_bridge_request_count / n64_bridge_request_info /
n64_bridge_get / n64_bridge_request / n64_bridge_result / n64_bridge_poll_event` (same header)
are what the Lua table is built on. Call them from the thread that runs `n64_run_frame`.

### Trying it without the engine

The headless runner logs the bridge:

```
ssb64_host --rom <rom> --frames 10000 --fuzz 34 --fuzz-until 8300 --bridge --request-at 8320 "win 1"
```

`--bridge` prints every event, the request's result, and all variables (before / after the
request and at the end). `--request-at F "set <name> <value> [index]"` writes a variable.
