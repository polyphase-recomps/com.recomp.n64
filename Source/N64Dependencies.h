/**
 * @file N64Dependencies.h
 * @brief Editor side of the N64 game packages: "Setup Dependencies" builds the game library
 *        each game's addon links, in the build profile's Build mode:
 *
 *   Decomp (default)  the game's native build (Native/build.ps1, Native/build-console.ps1)
 *                     compiles the decomp and cuts the asset pack out of the user's ROM.
 *   Recomp            com.recomp.n64's Native/tools/recomp/build_recomp.ps1 recompiles the
 *                     user's ROM with N64Recomp (the game package's Recomp/ config) and builds
 *                     it on the recomp runtime. Windows only so far.
 *
 * Game packages are Packages/<id> folders with Native/CMakeLists.txt using N64Port.cmake
 * (decomp) and/or Recomp/CMakeLists.txt using N64Recomp.cmake (recomp). The setup runs before
 * packaging for the platform being packaged (pre-build hook; a failure cancels the build)
 * unless the build profile turns it off, and on demand from the profile's Target Options in the
 * Packaging window. Same model as com.recomp.ps1.
 */
#pragma once

#include <cstdint>

struct PolyphaseBuildContext;
struct EditorUIHooks;

namespace N64Dependencies
{
// Build profile options (Target Options).
constexpr const char* kSetupOption = "n64.setupDependencies"; // "1" (default) runs the setup before packaging
constexpr const char* kDecompOption = "n64.decompDir";        // decomp checkout; empty = the game package's default
// "auto" (default: each game as it was last set up, recomp after Set Up Game), "decomp" or "recomp"
constexpr const char* kModeOption = "n64.buildMode";
// recomp: the user's .z64; empty = the project's copy (Assets/Recomp/Rom), else the game's toml default
constexpr const char* kRomOption = "n64.romPath";

// What a setup builds: the profile's Build mode and its inputs (any may be null or empty).
struct Options
{
    const char* decompDir;
    const char* mode;
    const char* romPath;
};

// Builds every game package for a platform (Platform enum value) now, logging the output.
// False if one failed.
bool SetupAll(int32_t platform, const Options& options);
// Same on a background thread; output and the result show in the log.
void SetupAllAsync(int32_t platform, const Options& options);
// Editor tick: writes queued output of a background setup to the log.
void Tick();
// Logs which game packages have no library for the editor's platform yet.
void CheckReady();
// The "N64 Recomp" section of the Packaging window's Target Options.
void DrawTargetOptions(const PolyphaseBuildContext* ctx);
// Tools > Recomp > N64 > Set Up Game...: pick your ROM (checked against the game package's
// Recomp/game.json, converted to .z64, optionally kept in the project's Assets/Recomp/Rom so
// builds include it) and recompile the game from it.
void RegisterSetUpGame(EditorUIHooks* hooks, uint64_t hookId);
}
