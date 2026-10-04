/**
 * @file N64Dependencies.h
 * @brief Editor side of the N64 game packages: "Setup Dependencies" runs each game's native
 *        build (Native/build.ps1, Native/build-console.ps1), which compiles the game library
 *        the game's addon links and cuts the asset pack out of the user's ROM.
 *
 * Game packages are Packages/<id>/Native folders whose CMakeLists.txt uses N64Port.cmake.
 * The setup runs before packaging for the platform being packaged (pre-build hook; a
 * failure cancels the build) unless the build profile turns it off, and on demand from the
 * profile's Target Options in the Packaging window. Same model as com.recomp.ps1.
 */
#pragma once

#include <cstdint>

struct PolyphaseBuildContext;

namespace N64Dependencies
{
// Build profile options (Target Options).
constexpr const char* kSetupOption = "n64.setupDependencies"; // "1" (default) runs the setup before packaging
constexpr const char* kDecompOption = "n64.decompDir";        // decomp checkout; empty = the game package's default

// Builds every game package for a platform (Platform enum value) now, logging the output.
// False if one failed. `decompDir` may be null or empty.
bool SetupAll(int32_t platform, const char* decompDir);
// Same on a background thread; output and the result show in the log.
void SetupAllAsync(int32_t platform, const char* decompDir);
// Editor tick: writes queued output of a background setup to the log.
void Tick();
// Logs which game packages have no library for the editor's platform yet.
void CheckReady();
// The "N64 Recomp" section of the Packaging window's Target Options.
void DrawTargetOptions(const PolyphaseBuildContext* ctx);
}
