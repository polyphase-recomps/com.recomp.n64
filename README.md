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
Native/runtime/recomp/  recomp mode: libultra and the hardware for a game recompiled by N64Recomp; gbi/ holds the
                        runtime's own GBI / ABI headers (no SDK or decomp headers needed)
Native/host/            standalone test runners (desktop, Wii / GameCube, 3DS)
Native/cmake/           N64Port.cmake (decomp builds), N64Recomp.cmake (recomp builds), toolchain files
Native/tools/           asset pack builder, overlay tooling, Dolphin test harness;
                        tools/recomp/: build_recomp.ps1, new_game.ps1, symbol and header checks
Source/                 the editor addon (Setup Dependencies, Build mode, Set Up Game, New Game Package);
                        Source/Game/: the player node, Lua and mod.base code template-made game packages include
Templates/game/         the game package template (Tools > Recomp > N64 > New Game Package)
Docs/Modding.md         mods and the script bridge (Lua `N64` table)
Docs/Launcher.md        a front-end scene: set the ROM, set mods up, start the game (N64.SetRomLocation / StartRecomp)
ThirdParty/N64Recomp/   vendored N64Recomp (MIT) + its libraries, for recomp-assisted games (see ThirdParty/THIRD_PARTY.md)
```

The canonical copy of this package lives at `P:\Projects\Recomp\Platforms\N64`. Game projects
take it as `Packages/com.recomp.n64`, and a game build can point at it directly with
`-DN64PORT_ROOT=<path>/Native`.

It depends on [com.recomp.mod.base](https://github.com/polyphase-recomps/com.recomp.mod.base)
(`package.json`): the mod settings, resolution scaler and launcher the shared game code in
`Source/Game` uses. Keep a clone of it in the project's `Packages/` next to this one.

A game builds in one of these ways (Build mode in Packaging > Target Options > N64 Recomp; Auto
keeps whichever a game was last set up in):

- **Decomp:** the game's own C from a decompilation, built as described below. Every target.
- **Recomp:** the game recompiled from the player's ROM by N64Recomp, with patches per game.
  Needs no decomp, only the game's symbol list. N64Recomp's C output is compiled into the
  library, so the library holds code made from your ROM: for development. Windows so far.
- **Recomp (live):** for PC releases. The library holds no game code: it carries the recomp
  runtime and N64Recomp's LiveRecomp, and the game recompiles the player's own ROM when it
  boots, straight to x86-64 / ARM64 (about half a second for SSB64). It ships only the game's
  recompiler data (`game.json`, the N64Recomp config and symbols, copied to
  `Packages/<game>/Assets/Recomp/Live` by `build_recomp.ps1 -Live`). The ROM must be the one
  `game.json` names (sha1); `.v64` / `.n64` dumps are fine. `[[patches.hook]]` entries (C text)
  need the Recomp mode. Runs exactly as Recomp does (same frames and audio). 64-bit PCs;
  Windows so far. A release must not keep a ROM in `Assets/Recomp/Rom` (builds ship it):
  packaged games ask the player for theirs.

The vendored N64Recomp is not part of any Decomp build.

## A new game, recompiled from the ROM

1. **Tools > Recomp > N64 > New Game Package...** makes `Packages/com.recomp.<id>` from
   `Templates/game`. That package is data only: game.json, an N64Recomp config, a symbol file to
   fill in, and a few lines of `Source/` naming the game.
2. Put the game's functions in its `Recomp/<id>.<region>.syms.toml`.
3. **Tools > Recomp > N64 > Set Up Game...** picks the ROM. The first ROM a new package is given
   becomes the one it recompiles: its sha1 and size are recorded, and its entry point too. The
   game is then recompiled and built.
4. Reload Native Addons, add the `<Name>Player` node to a scene, and Play.

The template's README (in every package made from it) has the details.

## Using it for a game

A game package supplies its decompiled sources, a `port_types.h`, the hooks in
`Native/include/port_game.h`, and a `CMakeLists.txt` that includes `Native/cmake/N64Port.cmake`.
`com.recomp.ssb64` is the reference.

Game data is never shipped: `Native/tools/make_rom_pack.py` cuts the asset ranges (no code)
out of the user's ROM at build time into a git-ignored pack the game loads at run time.
