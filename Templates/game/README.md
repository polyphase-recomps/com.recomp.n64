# {{TITLE}}

{{TITLE}} for Polyphase, recompiled from the player's own ROM by N64Recomp and run on
com.recomp.n64's recomp runtime. This package holds data only: no game code or assets.
It was made from com.recomp.n64's game template (Tools > Recomp > N64 > New Game Package).

## What is in here

| Path | What |
|---|---|
| `Recomp/game.json` | Title, N64Recomp config, the ROM's file name, sha1 and size. Set Up Game fills in the sha1 and size from the first ROM it is given. |
| `Recomp/recomp.{{REGION}}.toml` | N64Recomp config. Game-specific fixes go here as `[[patches.hook]]` / `[[patches.instruction]]`. |
| `Recomp/{{ID}}.{{REGION}}.syms.toml` | The game's functions (sections, names, addresses, sizes). **Provide this**: see the file. |
| `Recomp/CMakeLists.txt` | Builds the generated C on com.recomp.n64's recomp runtime. |
| `Source/N64Game.h` | The game's names. The rest of `Source/` includes com.recomp.n64's shared player, Lua and mod.base code. |

## Setting it up

1. Put the game's symbol list in `Recomp/{{ID}}.{{REGION}}.syms.toml`.
2. **Tools > Recomp > N64 > Set Up Game...**, then pick this game and your ROM (`.z64`, `.v64` or `.n64`).
   - The first time, it records the ROM's sha1 and size in `game.json` and its entry point in the toml.
   - Later it refuses any other ROM.
   - With **Keep a copy in the project** ticked, the ROM goes to `Assets/Recomp/Rom/{{ID}}.{{REGION}}.z64`. Builds of the project then include it, and it is git-ignored. Those builds are for your own use.
3. Reload Native Addons, add a **{{NAME}}Player** node to a scene, and press Play.

The game runs from its first frame if N64Recomp's output and the runtime cover everything it
uses. Otherwise, fix what it needs in the toml:
- `[[patches.hook]]` to run runtime code at an address;
- `[[patches.instruction]]` to change an instruction;
- MIPS C patches (later).

Busy-wait loops on another thread are handled by the runtime and logged
(`recomp: thread N spins in the loop at 0x...`).

## Releasing it

Set **Build mode** to **Recomp (live)** in Packaging > Target Options > N64 Recomp, and move
any ROM out of `Assets/Recomp/Rom`. The build then contains no game code or data: the game
recompiles the player's own ROM when it starts, from the symbols in `Assets/Recomp/Live`
(copied from `Recomp/` by the build, git-ignored), and asks the player for the ROM the first
time. Live builds can't use `[[patches.hook]]` (it is C source); `[[patches.instruction]]` works.

## Limits (for now)

- Windows only, with the F3DEX2 graphics microcode and the standard (n_)aspMain audio.
- The default controller layout puts the buttons where they sit on an N64 pad. For a layout of
  the game's own, see `Source/N64Game.h`.
