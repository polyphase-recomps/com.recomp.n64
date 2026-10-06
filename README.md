# com.recomp.n64 — shared runtime for native N64 ports in Polyphase

The game-independent half of an N64 native port: a replacement for the console's OS and
hardware (threads, controllers, audio, a display list renderer in software, for GX and for citro3d) that a
decompiled game is compiled against, plus the build pieces and tools game packages share.

> **No game is included.** This package contains no ROM, no game assets and no game code.
> A game package (for example `com.recomp.ssb64`) builds the game from **your own ROM** and a
> decompilation, on your machine. See that package's README for the steps; nothing here needs
> building on its own.

## Layout

```
Native/include/         API for game packages and hosts (port_host.h, port_game.h, port_gpu.h, port_bridge.h)
Native/runtime/guest/   OS / hardware replacement the game code links against
Native/runtime/host/    platform backends: Windows, POSIX, libogc (Wii / GameCube), libctru (3DS), GX and citro3d renderers
Native/host/            standalone test runners (desktop, Wii / GameCube, 3DS)
Native/cmake/           N64Port.cmake + toolchain files (PowerPC / ARM / AArch64 Linux, Wii, GameCube, 3DS)
Native/tools/           asset pack builder, overlay tooling, Dolphin test harness
Docs/Modding.md         mods and the script bridge (Lua `N64` table)
ThirdParty/N64Recomp/   vendored N64Recomp (MIT) + its libraries, for recomp-assisted games (see ThirdParty/THIRD_PARTY.md)
```

The canonical copy of this package lives at `P:\Projects\Recomp\Platforms\N64`. Game projects
take it as `Packages/com.recomp.n64`, and a game build can point at it directly with
`-DN64PORT_ROOT=<path>/Native`.

Projects stay **Decomp** projects (the game's own C, built as described below) unless they opt
into the planned **Recomp-assisted** mode. That mode uses N64Recomp on the player's ROM, with
patches per game. The vendored N64Recomp is not part of any Decomp build.

## Using it for a game

A game package supplies its decompiled sources, a `port_types.h`, the hooks in
`Native/include/port_game.h`, and a `CMakeLists.txt` that includes `Native/cmake/N64Port.cmake`.
`com.recomp.ssb64` is the reference.

Game data is never shipped: `Native/tools/make_rom_pack.py` cuts the asset ranges (no code)
out of the user's ROM at build time into a git-ignored pack the game loads at run time.
