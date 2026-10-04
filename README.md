# com.recomp.n64 — shared runtime for native N64 ports in Polyphase

The game-independent half of an N64 native port: a replacement for the console's OS and
hardware (threads, controllers, audio, a display list renderer in software and for GX) that a
decompiled game is compiled against, plus the build pieces and tools game packages share.

> **No game is included.** This package contains no ROM, no game assets and no game code.
> A game package (for example `com.recomp.ssmb64`) builds the game from **your own ROM** and a
> decompilation, on your machine. See that package's README for the steps; nothing here needs
> building on its own.

## Layout

```
Native/include/         API for game packages and hosts (port_host.h, port_game.h, port_gpu.h, port_bridge.h)
Native/runtime/guest/   OS / hardware replacement the game code links against
Native/runtime/host/    platform backends: Windows, POSIX, libogc (Wii / GameCube), GX renderer
Native/host/            standalone test runners (desktop, Wii / GameCube)
Native/cmake/           N64Port.cmake + toolchain files (PowerPC Linux, AArch64 Linux, Wii, GameCube)
Native/tools/           asset pack builder, overlay tooling, Dolphin test harness
Docs/Modding.md         mods and the script bridge (Lua `N64` table)
```

## Using it for a game

A game package supplies its decompiled sources, a `port_types.h`, the hooks in
`Native/include/port_game.h`, and a `CMakeLists.txt` that includes `Native/cmake/N64Port.cmake`.
`com.recomp.ssmb64` is the reference.

Game data is never shipped: `Native/tools/make_rom_pack.py` cuts the asset ranges (no code)
out of the user's ROM at build time into a git-ignored pack the game loads at run time.
