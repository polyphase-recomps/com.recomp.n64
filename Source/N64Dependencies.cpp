/**
 * @file N64Dependencies.cpp
 * @brief Builds the N64 game packages from the editor (see N64Dependencies.h).
 *
 * What runs where:
 *
 *   editor on    target     command (in Packages/<game>/Native)
 *   Windows      Windows    build.ps1
 *   Windows      Wii / GC   build-console.ps1 -Platform <target>   (after build.ps1 once)
 *   Windows      Linux      build.sh inside WSL                    (after build.ps1 once)
 *   Linux        Linux      build.sh
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
};

struct GameStatus
{
    std::string id;
    bool windowsLib, linuxLib, wiiLib, gameCubeLib, n3dsLib, assetPack;
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

        if (FileContains(native + "CMakeLists.txt", "N64Port.cmake"))
        {
            games.push_back({name, native});
        }
    }
    return games;
}

std::vector<GameStatus> GetStatus()
{
    std::vector<GameStatus> status;

    for (const GamePackage& game : FindGamePackages())
    {
        const std::string package = game.nativeDir + "../";
        status.push_back({game.id, AnyFile(package + "Lib/*.lib"), AnyFile(package + "Lib/Linux/*.a"),
                          AnyFile(package + "Lib/Wii/*.a"), AnyFile(package + "Lib/GameCube/*.a"),
                          AnyFile(package + "Lib/3DS/*.a"), AnyFile(package + "Assets/*.n64pak")});
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

bool SetupGame(const GamePackage& game, int32_t platform, const std::string& decompIn, bool background)
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

bool SetupGames(int32_t platform, const std::string& decompDir, bool background)
{
    bool ok = true;

    for (const GamePackage& game : FindGamePackages())
    {
        ok = SetupGame(game, platform, decompDir, background) && ok;
    }
    return ok;
}

const char* kDoneHint = "Reload Native Addons so the editor links the rebuilt game";
}

bool N64Dependencies::SetupAll(int32_t platform, const char* decompDir)
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
    const bool ok = SetupGames(platform, decompDir ? decompDir : "", false);
    sStatusValid = false;
    return ok;
}

void N64Dependencies::SetupAllAsync(int32_t platform, const char* decompDir)
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
    const std::string decomp = decompDir ? decompDir : "";
    sThread = std::thread([platform, decomp]() {
        const bool ok = SetupGames(platform, decomp, true);
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
#else
        const bool lib = game.linuxLib;
#endif
        if (!lib || !game.assetPack)
        {
            LogWarning("[n64] %s is not built yet (%s%s%s): Packaging > Target Options > N64 Recomp > Setup "
                       "Dependencies Now. It needs your own ROM and the decomp, see the package's README",
                       game.id.c_str(), lib ? "" : "no game library", (!lib && !game.assetPack) ? ", " : "",
                       game.assetPack ? "" : "no asset pack");
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

    if (!decompLoaded)
    {
        decomp[0] = 0;
        if (ctx->GetProfileSetting != nullptr)
        {
            ctx->GetProfileSetting(kDecompOption, decomp, sizeof(decomp));
        }
        decompLoaded = true;
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
        SetupAllAsync(ctx->basePlatform, decomp);
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
        ImGui::Text("%s:  Windows %s   Linux %s   Wii %s   GameCube %s   3DS %s   assets %s", game.id.c_str(),
                    game.windowsLib ? "built" : "-", game.linuxLib ? "built" : "-", game.wiiLib ? "built" : "-",
                    game.gameCubeLib ? "built" : "-", game.n3dsLib ? "built" : "-", game.assetPack ? "extracted" : "MISSING");
    }
    if (sStatus.empty())
    {
        ImGui::TextUnformatted("No N64 game packages in this project.");
    }
}

#else

// Building games needs the editor on Windows or Linux.
bool N64Dependencies::SetupAll(int32_t, const char*)
{
    return true;
}

void N64Dependencies::SetupAllAsync(int32_t, const char*)
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
