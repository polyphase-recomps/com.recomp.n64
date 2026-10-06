/**
 * @file N64Dependencies.cpp
 * @brief Builds the N64 game packages from the editor (see N64Dependencies.h).
 *
 * What runs where:
 *
 *   Build mode Decomp (in Packages/<game>/Native):
 *   editor on    target     command
 *   Windows      Windows    build.ps1
 *   Windows      Wii / GC   build-console.ps1 -Platform <target>   (after build.ps1 once)
 *   Windows      Linux      build.sh inside WSL                    (after build.ps1 once)
 *   Linux        Linux      build.sh
 *
 *   Build mode Recomp (the game's Recomp/ config, com.recomp.n64's script):
 *   Windows      Windows    Packages/com.recomp.n64/Native/tools/recomp/build_recomp.ps1
 *   (other targets need the recomp runtime's big-endian layout or a Linux script: not yet)
 */

#include "N64Dependencies.h"

#if EDITOR && (PLATFORM_WINDOWS || PLATFORM_LINUX)

#include "Engine.h"
#include "EngineTypes.h"
#include "Log.h"
#include "Plugins/PolyphaseBuildTargetAPI.h"

#include "imgui.h"

#include <atomic>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <glob.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#endif

namespace
{
struct GamePackage
{
    std::string id;
    std::string nativeDir; // .../Packages/<id>/Native/ (forward slashes, trailing)
    bool hasDecomp;        // Native/CMakeLists.txt builds the decomp (N64Port.cmake)
    bool hasRecomp;        // Recomp/CMakeLists.txt builds it recompiled from the ROM (N64Recomp.cmake)
};

struct GameStatus
{
    std::string id;
    bool windowsLib, linuxLib, wiiLib, gameCubeLib, n3dsLib, assetPack;
    bool windowsRecomp; // the Windows library was built in recomp mode (Lib/<lib>.mode)
    bool hasDecomp, hasRecomp;
};

std::mutex sLock;
std::vector<std::string> sPending; // background setup output, written by Tick
std::thread sThread;
std::atomic<bool> sRunning{false};
std::atomic<bool> sFinished{false}; // a background setup ended; Tick refreshes the status

std::vector<GameStatus> sStatus; // for the Target Options panel, refreshed on demand
bool sStatusValid = false;

// ---- small platform layer ---------------------------------------------------------

std::string Slashes(std::string path)
{
    for (char& c : path)
    {
        if (c == '\\') c = '/';
    }
    return path;
}

std::string ProjectDir()
{
    std::string dir = GetEngineState()->mProjectDirectory;
#if PLATFORM_WINDOWS
    char full[MAX_PATH];

    if (GetFullPathNameA(dir.c_str(), sizeof(full), full, nullptr) != 0)
    {
        dir = full;
    }
#else
    char full[PATH_MAX];

    if (realpath(dir.c_str(), full) != nullptr)
    {
        dir = full;
    }
#endif
    dir = Slashes(dir);
    if (!dir.empty() && dir.back() != '/')
    {
        dir += "/";
    }
    return dir;
}

// Whether a text file mentions `needle` (false if it cannot be read).
bool FileMentions(const std::string& path, const char* needle)
{
    std::ifstream in(path.c_str(), std::ios::binary);
    std::stringstream text;

    if (!in.is_open())
    {
        return false;
    }
    text << in.rdbuf();
    return text.str().find(needle) != std::string::npos;
}

bool Exists(const std::string& path)
{
#if PLATFORM_WINDOWS
    return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0;
#endif
}

// True if anything matches the wildcard pattern (wildcards in the file name only).
bool AnyFile(const std::string& pattern)
{
#if PLATFORM_WINDOWS
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);

    if (h == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    FindClose(h);
    return true;
#else
    glob_t found;
    const bool any = glob(pattern.c_str(), 0, nullptr, &found) == 0 && found.gl_pathc > 0;

    globfree(&found);
    return any;
#endif
}

std::vector<std::string> SubDirectories(const std::string& dir)
{
    std::vector<std::string> names;
#if PLATFORM_WINDOWS
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "*").c_str(), &fd);

    if (h == INVALID_HANDLE_VALUE)
    {
        return names;
    }
    do
    {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.')
        {
            names.push_back(fd.cFileName);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir.c_str());

    if (d == nullptr)
    {
        return names;
    }
    while (struct dirent* entry = readdir(d))
    {
        if (entry->d_name[0] != '.' && Exists(dir + entry->d_name + "/"))
        {
            names.push_back(entry->d_name);
        }
    }
    closedir(d);
#endif
    return names;
}

void Emit(const std::string& line, bool background)
{
    if (background)
    {
        std::lock_guard<std::mutex> guard(sLock);
        sPending.push_back(line);
    }
    else if (line.find("FAILED") != std::string::npos || line.find(": error") != std::string::npos)
    {
        LogError("%s", line.c_str());
    }
    else
    {
        LogDebug("%s", line.c_str());
    }
}

void EmitOutput(std::string& line, const char* data, size_t size, bool background)
{
    for (size_t i = 0; i < size; ++i)
    {
        if (data[i] != '\n')
        {
            line += data[i];
            continue;
        }
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // Compiler progress lines would flood the log; keep everything else.
        const bool progress = !line.empty() && line[0] == '[' && line.find("] Building ") != std::string::npos;
        if (!line.empty() && !progress)
        {
            Emit("[n64] " + line, background);
        }
        line.clear();
    }
}

// Runs a command line in a directory; its output goes to the log. True if it exits with 0.
bool RunCommand(const std::string& command, const std::string& workDir, bool background)
{
    std::string line;
    char buf[4096];

#if PLATFORM_WINDOWS
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE readPipe = nullptr, writePipe = nullptr;

    if (!CreatePipe(&readPipe, &writePipe, &sa, 0))
    {
        Emit("[n64] cannot create a pipe for the setup", background);
        return false;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    std::vector<char> cmdBuf(command.begin(), command.end());
    cmdBuf.push_back(0);
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi = {};

    if (!CreateProcessA(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, workDir.c_str(), &si, &pi))
    {
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        Emit("[n64] cannot start: " + command, background);
        return false;
    }
    CloseHandle(writePipe);

    DWORD got = 0;
    while (ReadFile(readPipe, buf, sizeof(buf), &got, nullptr) && got > 0)
    {
        EmitOutput(line, buf, got, background);
    }
    CloseHandle(readPipe);

    DWORD code = 1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
#else
    const std::string full = "cd '" + workDir + "' && " + command + " 2>&1";
    FILE* pipe = popen(full.c_str(), "r");
    int code = 1;

    if (pipe == nullptr)
    {
        Emit("[n64] cannot start: " + command, background);
        return false;
    }
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), pipe)) > 0)
    {
        EmitOutput(line, buf, got, background);
    }
    code = pclose(pipe);
#endif
    if (!line.empty())
    {
        Emit("[n64] " + line, background);
    }
    return code == 0;
}

// ---- game packages ----------------------------------------------------------------

bool FileContains(const std::string& path, const char* text)
{
    std::ifstream file(path, std::ios::binary);
    std::stringstream buffer;

    if (!file)
    {
        return false;
    }
    buffer << file.rdbuf();
    return buffer.str().find(text) != std::string::npos;
}

std::vector<GamePackage> FindGamePackages()
{
    std::vector<GamePackage> games;
    const std::string packages = ProjectDir() + "Packages/";

    for (const std::string& name : SubDirectories(packages))
    {
        const std::string native = packages + name + "/Native/";
        const bool decomp = FileContains(native + "CMakeLists.txt", "N64Port.cmake");
        const bool recomp = FileContains(packages + name + "/Recomp/CMakeLists.txt", "N64Recomp.cmake");

        if (decomp || recomp)
        {
            games.push_back({name, native, decomp, recomp});
        }
    }
    return games;
}

bool IsRecompMode(const char* mode)
{
    return mode != nullptr && std::string(mode) == "recomp";
}

// Lib/<lib>.mode, written by build_recomp.ps1 next to the library it published
bool WindowsLibIsRecomp(const GamePackage& game)
{
    const std::string lib = game.nativeDir + "../Lib/";
#if PLATFORM_WINDOWS
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((lib + "*.mode").c_str(), &fd);
    bool recomp = false;

    if (h == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    do
    {
        recomp = recomp || FileContains(lib + fd.cFileName, "recomp");
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return recomp;
#else
    glob_t found;
    bool recomp = false;

    if (glob((lib + "*.mode").c_str(), 0, nullptr, &found) == 0)
    {
        for (size_t i = 0; i < found.gl_pathc; ++i)
        {
            recomp = recomp || FileContains(found.gl_pathv[i], "recomp");
        }
    }
    globfree(&found);
    return recomp;
#endif
}

// A decomp build republished the library: the recomp marker no longer applies.
void ClearRecompMarker(const GamePackage& game)
{
#if PLATFORM_WINDOWS
    const std::string lib = game.nativeDir + "../Lib/";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((lib + "*.mode").c_str(), &fd);

    if (h == INVALID_HANDLE_VALUE)
    {
        return;
    }
    do
    {
        DeleteFileA((lib + fd.cFileName).c_str());
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    (void)game;
#endif
}

std::vector<GameStatus> GetStatus()
{
    std::vector<GameStatus> status;

    for (const GamePackage& game : FindGamePackages())
    {
        const std::string package = game.nativeDir + "../";
        status.push_back({game.id, AnyFile(package + "Lib/*.lib"), AnyFile(package + "Lib/Linux/*.a"),
                          AnyFile(package + "Lib/Wii/*.a"), AnyFile(package + "Lib/GameCube/*.a"),
                          AnyFile(package + "Lib/3DS/*.a"), AnyFile(package + "Assets/*.n64pak"),
                          WindowsLibIsRecomp(game), game.hasDecomp, game.hasRecomp});
    }
    return status;
}

const char* PlatformName(int32_t platform)
{
    switch ((Platform)platform)
    {
    case Platform::Windows: return "Windows";
    case Platform::Linux: return "Linux";
    case Platform::Wii: return "Wii";
    case Platform::GameCube: return "GameCube";
    case Platform::N3DS: return "3DS";
    default: return nullptr;
    }
}

#if PLATFORM_WINDOWS
bool RunPowerShell(const GamePackage& game, const std::string& script, const std::string& args, bool background)
{
    if (!Exists(game.nativeDir + script))
    {
        Emit("[n64] " + game.id + ": no Native/" + script, background);
        return false;
    }
    Emit("[n64] " + game.id + ": " + script + " " + args, background);
    return RunCommand("powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + game.nativeDir + script + "\" " + args,
                      game.nativeDir, background);
}
#endif

bool SetupDecomp(const GamePackage& game, int32_t platform, const std::string& decompIn, bool background)
{
    const char* name = PlatformName(platform);
    const std::string decomp = Slashes(decompIn);
    bool ok = true;

    if (name == nullptr)
    {
        Emit("[n64] " + game.id + ": the game is not built for this platform; its addon uses its stub there", background);
        return true;
    }
    if ((Platform)platform == Platform::N3DS && !FileMentions(game.nativeDir + "build-console.ps1", "'3DS'"))
    {
        Emit("[n64] " + game.id + ": no 3DS build for this game; its addon uses its stub there", background);
        return true;
    }
#if PLATFORM_WINDOWS
    const std::string decompArg = decomp.empty() ? "" : "-Decomp \"" + decomp + "\"";

    if ((Platform)platform == Platform::Windows)
    {
        ok = RunPowerShell(game, "build.ps1", decompArg, background);
    }
    else
    {
        // Some games' other builds reuse files their Windows build generates (Smash Bros.: its
        // console script stops with "run .\build.ps1 first" without them); the others build on
        // their own.
        if (FileMentions(game.nativeDir + "build-console.ps1", "build.ps1 first") &&
            !Exists(game.nativeDir + "build/RelWithDebInfo/gen/port_reloc_table.c"))
        {
            ok = RunPowerShell(game, "build.ps1", decompArg, background);
        }
        if (ok && (Platform)platform == Platform::Linux)
        {
            // Linux libraries are built by the Linux toolchain inside WSL. Paths are converted
            // there (wslpath), so no Windows path syntax reaches the shell.
            std::string script = "cd \\\"$(wslpath -u '" + game.nativeDir + "')\\\" && ";

            if (!decomp.empty())
            {
                script += "DECOMP=\\\"$(wslpath -u '" + decomp + "')\\\" ";
            }
            script += "sh build.sh";
            Emit("[n64] " + game.id + ": build.sh (in WSL)", background);
            ok = RunCommand("wsl.exe -e sh -c \"" + script + "\"", game.nativeDir, background);
            if (!ok)
            {
                Emit("[n64] " + game.id + ": the Linux build needs WSL with gcc, cmake and python3 installed", background);
            }
        }
        else if (ok)
        {
            ok = RunPowerShell(game, "build-console.ps1", std::string("-Platform ") + name + " " + decompArg, background);
        }
    }
#else
    if ((Platform)platform == Platform::Linux)
    {
        if (!Exists(game.nativeDir + "build.sh"))
        {
            Emit("[n64] " + game.id + ": no Native/build.sh", background);
            ok = false;
        }
        else
        {
            Emit("[n64] " + game.id + ": build.sh", background);
            ok = RunCommand((decomp.empty() ? std::string() : "DECOMP='" + decomp + "' ") + "sh build.sh", game.nativeDir,
                            background);
        }
    }
    else
    {
        Emit("[n64] " + game.id + ": " + name + " libraries are built from the editor on Windows "
             "(Native/build.ps1, build-console.ps1); using the library already in the package, if any", background);
        return true;
    }
#endif
    Emit("[n64] " + game.id + (ok ? std::string(": ready for ") + name : std::string(": SETUP FAILED for ") + name),
         background);
    return ok;
}

// Recomp mode: the game recompiled from the user's ROM (com.recomp.n64's build_recomp script).
bool SetupRecomp(const GamePackage& game, int32_t platform, const std::string& decomp, const std::string& rom,
                 bool background)
{
    const char* name = PlatformName(platform);

    if ((Platform)platform != Platform::Windows)
    {
        Emit(std::string("[n64] ") + game.id + ": Build mode Recomp builds for Windows only so far (" +
                 (name ? name : "this platform") + " needs the recomp runtime's big-endian memory layout or a "
                 "Linux script); package Windows, or use Build mode Decomp. SETUP FAILED",
             background);
        return false;
    }
#if PLATFORM_WINDOWS
    const std::string script = ProjectDir() + "Packages/com.recomp.n64/Native/tools/recomp/build_recomp.ps1";

    if (!Exists(script))
    {
        Emit("[n64] " + game.id + ": no " + script + " (update com.recomp.n64). SETUP FAILED", background);
        return false;
    }
    std::string command = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + script + "\" -Package \"" +
                          game.nativeDir + "..\"";
    if (!rom.empty())
    {
        command += " -Rom \"" + rom + "\"";
    }
    if (!decomp.empty())
    {
        command += " -Decomp \"" + decomp + "\"";
    }
    Emit("[n64] " + game.id + ": recompiling from the ROM (build_recomp.ps1)", background);
    const bool ok = RunCommand(command, game.nativeDir, background);
    Emit("[n64] " + game.id + (ok ? ": ready for Windows (recomp)" : ": SETUP FAILED for Windows (recomp)"), background);
    return ok;
#else
    Emit("[n64] " + game.id + ": Build mode Recomp builds from the editor on Windows only so far. SETUP FAILED",
         background);
    return false;
#endif
}

bool SetupGame(const GamePackage& game, int32_t platform, const std::string& decomp, bool recomp,
               const std::string& rom, bool background)
{
    if (recomp && !game.hasRecomp)
    {
        Emit("[n64] " + game.id + ": no Recomp/ config in the package; building it from the decomp", background);
        recomp = false;
    }
    if (recomp)
    {
        return SetupRecomp(game, platform, decomp, rom, background);
    }
    if (!game.hasDecomp)
    {
        Emit("[n64] " + game.id + ": this game has no decomp build (Native/); set Build mode to Recomp. SETUP FAILED",
             background);
        return false;
    }
    const bool ok = SetupDecomp(game, platform, decomp, background);
    if (ok && (Platform)platform == Platform::Windows)
    {
        ClearRecompMarker(game); // build.ps1 published the decomp library
    }
    return ok;
}

struct SetupRequest
{
    std::string decomp, rom;
    bool recomp;
};

SetupRequest MakeRequest(const N64Dependencies::Options& options)
{
    return {Slashes(options.decompDir ? options.decompDir : ""), Slashes(options.romPath ? options.romPath : ""),
            IsRecompMode(options.mode)};
}

bool SetupGames(int32_t platform, const SetupRequest& request, bool background)
{
    bool ok = true;

    for (const GamePackage& game : FindGamePackages())
    {
        ok = SetupGame(game, platform, request.decomp, request.recomp, request.rom, background) && ok;
    }
    return ok;
}

const char* kDoneHint = "Reload Native Addons so the editor links the rebuilt game";
}

bool N64Dependencies::SetupAll(int32_t platform, const Options& options)
{
    if (sRunning)
    {
        LogWarning("[n64] Setup Dependencies is still running in the background; packaging waits for it");
    }
    if (sThread.joinable())
    {
        sThread.join();
    }
    Tick();
    const bool ok = SetupGames(platform, MakeRequest(options), false);
    sStatusValid = false;
    return ok;
}

void N64Dependencies::SetupAllAsync(int32_t platform, const Options& options)
{
    if (sRunning)
    {
        LogWarning("[n64] Setup Dependencies is already running");
        return;
    }
    if (sThread.joinable())
    {
        sThread.join();
    }
    sRunning = true;
    const SetupRequest request = MakeRequest(options);
    sThread = std::thread([platform, request]() {
        const bool ok = SetupGames(platform, request, true);
        Emit(ok ? std::string("[n64] Setup Dependencies done. ") + kDoneHint
                : std::string("[n64] Setup Dependencies FAILED (see above)"),
             true);
        sRunning = false;
        sFinished = true;
    });
}

void N64Dependencies::Tick()
{
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> guard(sLock);
        lines.swap(sPending);
    }
    for (const std::string& line : lines)
    {
        if (line.find("FAILED") != std::string::npos || line.find(": error") != std::string::npos)
            LogError("%s", line.c_str());
        else
            LogDebug("%s", line.c_str());
    }
    if (sFinished.exchange(false))
    {
        sStatusValid = false;
        if (sThread.joinable())
        {
            sThread.join();
        }
    }
}

void N64Dependencies::CheckReady()
{
    for (const GameStatus& game : GetStatus())
    {
#if PLATFORM_WINDOWS
        const bool lib = game.windowsLib;
        const bool needsPack = !game.windowsRecomp; // a recompiled game boots from the ROM itself
#else
        const bool lib = game.linuxLib;
        const bool needsPack = true;
#endif
        if (!lib || (needsPack && !game.assetPack))
        {
            LogWarning("[n64] %s is not built yet (%s%s%s): Packaging > Target Options > N64 Recomp > Setup "
                       "Dependencies Now. It needs your own ROM and the decomp, see the package's README",
                       game.id.c_str(), lib ? "" : "no game library", (!lib && needsPack && !game.assetPack) ? ", " : "",
                       (!needsPack || game.assetPack) ? "" : "no asset pack");
        }
    }
}

void N64Dependencies::DrawTargetOptions(const PolyphaseBuildContext* ctx)
{
    char value[8] = "";
    const bool hasValue = ctx->GetProfileSetting != nullptr && ctx->GetProfileSetting(kSetupOption, value, sizeof(value)) != 0;
    bool setup = !hasValue || value[0] != '0';
    static char decomp[512];
    static bool decompLoaded = false;

    if (ImGui::Checkbox("Setup Dependencies before packaging", &setup) && ctx->SetProfileSetting != nullptr)
    {
        ctx->SetProfileSetting(kSetupOption, setup ? "1" : "0");
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Before packaging, builds each N64 game package (Packages/<game>/Native) for the\n"
                          "platform being packaged: the game library the addon links, and the asset pack\n"
                          "cut from your own ROM. Only what changed is rebuilt. A failure cancels the\n"
                          "packaging. Linux libraries are built in WSL when the editor runs on Windows.");
    }

    // Build mode: what the game library is made from
    char modeValue[16] = "";
    if (ctx->GetProfileSetting != nullptr)
    {
        ctx->GetProfileSetting(kModeOption, modeValue, sizeof(modeValue));
    }
    int mode = IsRecompMode(modeValue) ? 1 : 0;
    const char* modes[] = {"Decomp", "Recomp (from your ROM)"};
    if (ImGui::Combo("Build mode", &mode, modes, 2) && ctx->SetProfileSetting != nullptr)
    {
        ctx->SetProfileSetting(kModeOption, mode == 1 ? "recomp" : "decomp");
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Decomp: the game is compiled from its decompilation (Native/), with the asset pack\n"
                          "cut from your ROM. Every target.\n"
                          "Recomp: the game is recompiled from your ROM by N64Recomp (the package's Recomp/\n"
                          "config) and runs on the recomp runtime; it boots from the whole ROM. For games\n"
                          "whose decomp is unfinished. Windows only so far.\n"
                          "Both publish the same library name, so the game's addon is the same either way.");
    }

    static char rom[512];
    if (!decompLoaded)
    {
        decomp[0] = 0;
        rom[0] = 0;
        if (ctx->GetProfileSetting != nullptr)
        {
            ctx->GetProfileSetting(kDecompOption, decomp, sizeof(decomp));
            ctx->GetProfileSetting(kRomOption, rom, sizeof(rom));
        }
        decompLoaded = true;
    }
    if (mode == 1)
    {
        if (ImGui::InputText("ROM (.z64)", rom, sizeof(rom)) && ctx->SetProfileSetting != nullptr)
        {
            ctx->SetProfileSetting(kRomOption, rom);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Your own ROM of the game, big-endian (.z64). Empty: the path in the game\n"
                              "package's Recomp/recomp.<region>.toml. It is recompiled on this machine; the\n"
                              "generated code stays in the package's build/ and Lib/ folders (git-ignored).");
        }
    }
    if (ImGui::InputText("Decomp folder", decomp, sizeof(decomp)) && ctx->SetProfileSetting != nullptr)
    {
        ctx->SetProfileSetting(kDecompOption, decomp);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("The patched decompilation checkout with your ROM in it (see the game package's\n"
                          "README). Leave empty for the default: a folder next to the project, as the\n"
                          "game package's build script expects.");
    }

    const bool running = sRunning;
    if (running)
    {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Setup Dependencies Now"))
    {
        SetupAllAsync(ctx->basePlatform, {decomp, mode == 1 ? "recomp" : "decomp", rom});
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Builds for the platform of this profile, in the background (see the log).");
    }
    if (running)
    {
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextUnformatted("running, see the log...");
    }

    if (!sStatusValid && !running)
    {
        sStatus = GetStatus();
        sStatusValid = true;
    }
    for (const GameStatus& game : sStatus)
    {
        ImGui::Text("%s (%s):  Windows %s   Linux %s   Wii %s   GameCube %s   3DS %s   assets %s", game.id.c_str(),
                    game.hasDecomp && game.hasRecomp ? "decomp, recomp" : game.hasRecomp ? "recomp only" : "decomp",
                    !game.windowsLib ? "-" : game.windowsRecomp ? "built (recomp)" : "built", game.linuxLib ? "built" : "-",
                    game.wiiLib ? "built" : "-", game.gameCubeLib ? "built" : "-", game.n3dsLib ? "built" : "-",
                    game.assetPack ? "extracted" : game.windowsRecomp ? "not needed (recomp)" : "MISSING");
    }
    if (sStatus.empty())
    {
        ImGui::TextUnformatted("No N64 game packages in this project.");
    }
}

#else

// Building games needs the editor on Windows or Linux.
bool N64Dependencies::SetupAll(int32_t, const Options&)
{
    return true;
}

void N64Dependencies::SetupAllAsync(int32_t, const Options&)
{
}

void N64Dependencies::Tick()
{
}

void N64Dependencies::CheckReady()
{
}

void N64Dependencies::DrawTargetOptions(const PolyphaseBuildContext*)
{
}

#endif
