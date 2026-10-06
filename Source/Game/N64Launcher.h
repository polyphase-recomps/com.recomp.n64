/**
 * @file N64Launcher.h
 * @brief Starting an N64 game from a front end: a launcher scene (or C++) sets the ROM, lets the
 *        player set mods up, then starts the game, instead of the player node booting it on its
 *        first frame. Also the Lua table `N64` (Game/N64LuaImpl.h).
 *
 * The flow of a launcher scene (no player node in it):
 *   1. SetRomLocation(path)   the player's ROM (.z64 / .v64 / .n64): checked, then remembered in
 *                             Saves/<id>.rom.txt for every later start
 *      (BrowseForRom() opens a file dialog for it; GetRomLocation() is the remembered one)
 *   2. LoadMods()             the game's mod settings, so mod.base's `Mods` table lists and sets
 *                             them before the game runs (they are applied when it does)
 *   3. StartRecomp()          boots the game now. A Recomp (live) build recompiles the ROM here
 *                             (about half a second: show "Starting..." a frame before calling)
 *   4. load the scene with the game's player node: it shows the game started in 3.
 * A player node that ticks before StartRecomp() was called boots the game itself, as always.
 *
 * Implemented by Game/N64LauncherImpl.h, which one .cpp of the game package includes (the
 * template's player .cpp does).
 */
#pragma once

#include <string>

class RecompGameLauncher;

namespace N64Launcher
{
// ---- the ROM ------------------------------------------------------------------------------
// Whether a file is a ROM of this game: an N64 ROM in any byte order, and the one the game was
// made from when the game knows it (game.json's sha1). message says why not, or what it is.
bool CheckRom(const std::string& path, std::string& message);
// Checks the ROM, then remembers it as the one the game starts from (Saves/<id>.rom.txt).
bool SetRomLocation(const std::string& path, std::string& message);
// The ROM set before (this run or an earlier one); "" if none.
std::string GetRomLocation();
void ClearRomLocation();
// A file dialog for picking the ROM (Windows, Linux); "" if cancelled or not available.
std::string BrowseForRom();

// ---- mods -----------------------------------------------------------------------------------
// Loads the game's mod settings (its Mod Map and the player's saved values) so mod.base's
// `Mods` table / ModSettings works before the game runs. False if the game has no Mod Map.
bool LoadMods();

// ---- starting the game ------------------------------------------------------------------------
// Boots the game now: the ROM set with SetRomLocation, else the game data the project ships
// (the decomp build's asset pack, Assets/Recomp/Rom). Mod settings are saved first. Returns
// false, with message saying why, if it could not start (no ROM set, the wrong ROM, ...).
// Starting again once the game runs does nothing and returns true.
bool StartRecomp(std::string& message);
bool IsStarted();
// "idle" (not started), "running", or "failed"; GetMessage() is the last start's message.
const char* GetStatus();
std::string GetMessage();

// ---- for the player node ----------------------------------------------------------------------
// The player's own boot on its first frame (when no launcher started the game): the ROM set
// before, the shipped data, then romPath (the node's ROM Path). A packaged game then asks the
// player for their ROM (askPlayer).
bool BootFromPlayer(const std::string& romPath, bool askPlayer);
// The runtime was shut down (the addon unloads): the next start boots again.
void Reset();

// ---- com.recomp.mod.base -------------------------------------------------------------------------
// This game as a mod.base launcher (ModBaseLauncher.h): the generic launcher scene, the
// RecompLauncher node and Lua Recomp.SetRomLocation / StartGame / ... use it. The game's addon
// registers it (Recomp_RegisterLauncher) next to its provider.
RecompGameLauncher* AsRecompLauncher();
// The build ships the game's data (the decomp build's asset pack, or a ROM in Assets/Recomp/Rom).
bool HasShippedData();
} // namespace N64Launcher
