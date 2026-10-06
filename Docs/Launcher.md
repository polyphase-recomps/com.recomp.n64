# A launcher for an N64 game

By default the game's player node starts the game on its first frame. A launcher is a scene
of your own (no player node in it) that runs first. In it the player sets their ROM up and
picks mods, and then starts the game. After that you load the scene that has the player node.

There are three ways to make one. Use the first unless you need something it can't do.

1. **The generated launcher (no script).** Use **Tools > Recomp > Mods > Launcher** from com.recomp.mod.base. Pick the game's Mod Map, then set the title, logo, background, button labels and the Game Scene (the scene with the player node). Then **Generate Scene**. The panel and buttons follow the map's Menu Style. To start from it, make it the scene the project opens with.
2. **Your own UI, made of mod.base's widgets.** Add a `RecompLauncher` node, `RecompButton`s with Setting `@launcher:play` / `browse` / `forget` / `mods` / `quit`, and `RecompText`s with `{@launcher.romfile}`, `{@launcher.message}` and the like. See mod.base's README, "A launcher".
3. **Script it.** Lua calls are available two ways:
   - mod.base's `Recomp.SetRomLocation / GetRomLocation / BrowseForRom / LoadMods / StartGame / ...` work for any runtime;
   - this game's own `N64.*` table (below) has the same calls for N64 games.

   From C++, use `N64Launcher` (`Source/Game/N64Launcher.h`).

Every game package made from the template registers its game with mod.base's launchers, and so does `com.recomp.ssb64`. That registration is what makes options 1 and 2 work for it.

## Lua (`N64` table)

```lua
N64.SetRomLocation(path)   -- checks the ROM and remembers it (Saves/<id>.rom.txt): ok, message
N64.GetRomLocation()       -- the ROM set before (this run or an earlier one), or nil
N64.ClearRomLocation()     -- forgets it
N64.CheckRom(path)         -- ok, message, and saves nothing
N64.BrowseForRom()         -- a file dialog (Windows, Linux): the picked path, or nil
N64.LoadMods()             -- loads the game's mod settings, so Mods.* works before the game runs
N64.StartRecomp()          -- starts the game now: ok, message
N64.IsStarted()            -- true once started
N64.GetStatus()            -- "idle" / "running" / "failed", and the last start's message
```

- **The ROM:** `.z64`, `.v64` and `.n64` dumps all work.
  - When the build knows the ROM's sha1 (`game.json`), another region, revision or a bad dump is refused, and the message says so.
  - A Recomp (live) build always knows it. So does the editor, from the package's `Recomp/game.json`.
  - `message` names the ROM from its header (`SUPER SMASH BROS.: the ROM of ...`), so it can go straight into the UI.
- **Mods:** after `N64.LoadMods()`, mod.base's `Mods` table works before the game runs:
  - `Mods.List()`, `Mods.Get(id)`, `Mods.Text(id)`, `Mods.Set(id, v)`, `Mods.Step(id, dir)`, `Mods.Reset()`, `Mods.Save()`.
  - The values are saved, and they reach the game as soon as it runs.
  - `StartRecomp` saves them too.
- **Starting:** `StartRecomp` boots from the ROM set with `SetRomLocation`. If none is set, it uses what the project ships (the decomp build's asset pack, or `Assets/Recomp/Rom`).
  - A Recomp (live) build recompiles the ROM first, which takes about half a second. Set a "Starting..." text one frame before you call it.
  - It never shows a dialog. On failure it returns `false` and the reason.
  - Calling it again once the game runs does nothing.

## Example

```lua
Launcher = {}

function Launcher:Start()
    self.status = self:FindChild("Status", true)
    N64.LoadMods()
    local rom = N64.GetRomLocation()
    self.status:SetText(rom and ("ROM: " .. rom) or "Choose your ROM to play")
end

function Launcher:OnBrowse()                 -- a "Choose ROM..." button
    local path = N64.BrowseForRom()
    if path then
        local ok, message = N64.SetRomLocation(path)
        self.status:SetText(message)
    end
end

function Launcher:OnToggleMod(id)            -- one per mod the menu lists (Mods.List())
    Mods.Step(id, 1)
end

function Launcher:OnPlay()
    if N64.GetRomLocation() == nil then
        self.status:SetText("Choose your ROM first")
        return
    end
    self.status:SetText("Starting...")
    self.startNextFrame = true               -- let the text show before the recompile
end

function Launcher:Tick(dt)
    if self.startNextFrame then
        self.startNextFrame = false
        local ok, message = N64.StartRecomp()
        if ok then
            self:GetWorld():LoadScene("SC_Game")   -- the scene with the game's player node
        else
            self.status:SetText(message)
        end
    end
end
```

## C++

```cpp
#include "Game/N64Launcher.h"

std::string message;
if (N64Launcher::SetRomLocation(path, message) && N64Launcher::StartRecomp(message)) { ... }
```

## Without a launcher

Nothing changes. The player node starts the game on its first frame and tries, in order:
1. the ROM set before;
2. the shipped game data;
3. its own ROM Path property.

A packaged game then asks the player for their ROM with a dialog.
