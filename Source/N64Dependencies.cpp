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

#include "AssetManager.h"
#include "Engine.h"
#include "EngineTypes.h"
#include "Log.h"
#include "Plugins/EditorUIHooks.h"
#include "Plugins/PolyphaseBuildTargetAPI.h"

#include "imgui.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <utility>
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

// ---- recomp game data: Recomp/game.json, the ROM --------------------------------------

std::string ReadText(const std::string& path)
{
    std::ifstream in(path.c_str(), std::ios::binary);
    std::stringstream text;

    if (in.is_open())
    {
        text << in.rdbuf();
    }
    return text.str();
}

// The value of the first "key": "value" or "key": number in a JSON text (game.json is flat
// enough for this; it is ours).
std::string JsonValue(const std::string& text, const char* key)
{
    const std::string quoted = std::string("\"") + key + "\"";
    size_t at = text.find(quoted);

    if (at == std::string::npos || (at = text.find(':', at + quoted.size())) == std::string::npos)
    {
        return "";
    }
    at = text.find_first_not_of(" \t\r\n", at + 1);
    if (at == std::string::npos)
    {
        return "";
    }
    if (text[at] == '"')
    {
        const size_t end = text.find('"', at + 1);
        return end == std::string::npos ? "" : text.substr(at + 1, end - at - 1);
    }
    const size_t end = text.find_first_of(",}\r\n", at);
    return text.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

struct GameInfo
{
    std::string title;   // "Super Smash Bros. (US)"
    std::string romFile; // the name the ROM gets in the project: "ssb64.us.z64"
    std::string sha1;    // of the .z64 the package recompiles (lower case hex)
    uint64_t romSize = 0;
};

GameInfo ReadGameInfo(const GamePackage& game)
{
    const std::string text = ReadText(game.nativeDir + "../Recomp/game.json");
    GameInfo info;

    info.title = JsonValue(text, "title");
    info.romFile = JsonValue(text, "file");
    info.sha1 = JsonValue(text, "sha1");
    info.romSize = strtoull(JsonValue(text, "size").c_str(), nullptr, 10);
    for (char& c : info.sha1)
    {
        c = (char)tolower((unsigned char)c);
    }
    if (info.title.empty())
    {
        info.title = game.id;
    }
    if (info.romFile.empty())
    {
        info.romFile = game.id + ".z64";
    }
    return info;
}

// SHA-1 (FIPS 180-1), for checking that a ROM is the one a package recompiles
std::string Sha1Hex(const std::vector<uint8_t>& data)
{
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    std::vector<uint8_t> msg(data);
    const uint64_t bits = (uint64_t)data.size() * 8u;

    msg.push_back(0x80);
    while (msg.size() % 64 != 56)
    {
        msg.push_back(0);
    }
    for (int i = 7; i >= 0; --i)
    {
        msg.push_back((uint8_t)(bits >> (i * 8)));
    }
    auto rol = [](uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
    for (size_t chunk = 0; chunk < msg.size(); chunk += 64)
    {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
        {
            const uint8_t* p = &msg[chunk + i * 4];
            w[i] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
        }
        for (int i = 16; i < 80; ++i)
        {
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i)
        {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    char hex[41];
    for (int i = 0; i < 5; ++i)
    {
        snprintf(hex + i * 8, 9, "%08x", h[i]);
    }
    return std::string(hex, 40);
}

// Reads a ROM dump and puts it in the .z64 (big-endian) byte order whatever it was dumped in:
// .v64 swaps the bytes of every halfword, .n64 the bytes of every word. Empty on failure.
std::vector<uint8_t> ReadRomAsZ64(const std::string& path, std::string& error)
{
    std::ifstream in(path.c_str(), std::ios::binary);
    std::vector<uint8_t> rom((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    if (!in.is_open() && rom.empty())
    {
        error = "cannot open " + path;
        return {};
    }
    if (rom.size() < 0x1000 || rom.size() % 4 != 0)
    {
        error = path + " is not an N64 ROM (size)";
        return {};
    }
    const uint32_t magic = ((uint32_t)rom[0] << 24) | ((uint32_t)rom[1] << 16) | ((uint32_t)rom[2] << 8) | rom[3];
    if (magic == 0x37804012u) // .v64
    {
        for (size_t i = 0; i < rom.size(); i += 2)
        {
            std::swap(rom[i], rom[i + 1]);
        }
    }
    else if (magic == 0x40123780u) // .n64
    {
        for (size_t i = 0; i < rom.size(); i += 4)
        {
            std::swap(rom[i], rom[i + 3]);
            std::swap(rom[i + 1], rom[i + 2]);
        }
    }
    else if (magic != 0x80371240u)
    {
        error = path + " is not an N64 ROM (unknown header)";
        return {};
    }
    return rom;
}

bool MakeDirs(const std::string& dir)
{
    std::string path;

    for (size_t i = 0; i < dir.size(); ++i)
    {
        path += dir[i];
        if ((dir[i] == '/' || i + 1 == dir.size()) && path.size() > 3)
        {
#if PLATFORM_WINDOWS
            CreateDirectoryA(path.c_str(), nullptr);
#else
            mkdir(path.c_str(), 0755);
#endif
        }
    }
    return Exists(dir);
}

bool WriteFileBytes(const std::string& path, const std::vector<uint8_t>& data)
{
    std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);

    out.write((const char*)data.data(), (std::streamsize)data.size());
    return out.good();
}

// Keeps `line` in a .gitignore (a project that is a git repository, or already has one).
void EnsureIgnored(const std::string& projectDir, const char* line, const char* comment)
{
    const std::string path = projectDir + ".gitignore";

    if (!Exists(path) && !Exists(projectDir + ".git"))
    {
        return;
    }
    const std::string text = ReadText(path);
    if (text.find(line) != std::string::npos)
    {
        return;
    }
    std::ofstream out(path.c_str(), std::ios::binary | std::ios::app);
    out << (text.empty() || text.back() == '\n' ? "" : "\n") << comment << "\n" << line << "\n";
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

enum class BuildMode
{
    Auto,   // each game as it was last set up: recomp if its Windows library is (or it has no decomp)
    Decomp,
    Recomp
};

BuildMode ParseMode(const char* mode)
{
    const std::string value = mode ? mode : "";
    return value == "recomp" ? BuildMode::Recomp : value == "decomp" ? BuildMode::Decomp : BuildMode::Auto;
}

bool UseRecomp(const GamePackage& game, BuildMode mode)
{
    switch (mode)
    {
    case BuildMode::Recomp: return true;
    case BuildMode::Decomp: return false;
    default: return !game.hasDecomp || (game.hasRecomp && WindowsLibIsRecomp(game));
    }
}

struct SetupRequest
{
    std::string decomp, rom;
    BuildMode mode;
};

SetupRequest MakeRequest(const N64Dependencies::Options& options)
{
    return {Slashes(options.decompDir ? options.decompDir : ""), Slashes(options.romPath ? options.romPath : ""),
            ParseMode(options.mode)};
}

bool SetupGames(int32_t platform, const SetupRequest& request, bool background)
{
    bool ok = true;

    for (const GamePackage& game : FindGamePackages())
    {
        ok = SetupGame(game, platform, request.decomp, UseRecomp(game, request.mode), request.rom, background) && ok;
    }
    return ok;
}

const char* kDoneHint = "Reload Native Addons so the editor links the rebuilt game";

// ---- Set Up Game (Tools > Recomp > N64 > Set Up Game...) ---------------------------------
// The user's ROM -> checked, in .z64 order, optionally kept in the project's Assets/Recomp/Rom
// (packaged with the project's builds, git-ignored) -> the game recompiled from it.

const char* kSetUpTitle = "Set Up N64 Game";
const char* kProjectRomDir = "Assets/Recomp/Rom/";

EditorUIHooks* sHooks = nullptr;
uint64_t sHookId = 0;
std::string sSetUpStatus;        // the dialog's progress line (under sLock)
std::string sPendingRawAsset;    // a ROM copied into the project, registered for packaging by Tick (under sLock)

void SetUpStatus(const std::string& line)
{
    {
        std::lock_guard<std::mutex> guard(sLock);
        sSetUpStatus = line;
    }
    Emit("[n64] " + line, true);
}

bool SetUpGame(const GamePackage& game, const std::string& romIn, bool copyToProject)
{
    const GameInfo info = ReadGameInfo(game);
    std::string error;

    SetUpStatus("reading " + romIn);
    const std::vector<uint8_t> rom = ReadRomAsZ64(Slashes(romIn), error);
    if (rom.empty())
    {
        SetUpStatus("SETUP FAILED: " + error);
        return false;
    }
    SetUpStatus("checking the ROM");
    const std::string sha1 = Sha1Hex(rom);
    if (!info.sha1.empty() && sha1 != info.sha1)
    {
        SetUpStatus("SETUP FAILED: this is not the ROM " + game.id + " recompiles (" + info.title + "): sha1 " + sha1 +
                    ", expected " + info.sha1 + ". Another region or revision, or a bad dump?");
        return false;
    }

    // Where the .z64 is kept: in the project (shipped with its builds), or in the package's
    // build folder (this machine only)
    const std::string dir = copyToProject ? ProjectDir() + kProjectRomDir : game.nativeDir + "build/recomp/rom/";
    const std::string target = dir + info.romFile;
    if (!MakeDirs(dir))
    {
        SetUpStatus("SETUP FAILED: cannot create " + dir);
        return false;
    }
    std::string existingError;
    const std::vector<uint8_t> existing = Exists(target) ? ReadRomAsZ64(target, existingError) : std::vector<uint8_t>();
    if (existing != rom)
    {
        SetUpStatus("writing " + target);
        if (!WriteFileBytes(target, rom))
        {
            SetUpStatus("SETUP FAILED: cannot write " + target);
            return false;
        }
    }
    if (copyToProject)
    {
        EnsureIgnored(ProjectDir(), "Assets/Recomp/Rom/",
                      "# Your own N64 ROMs (Set Up Game): shipped with your builds, never committed");
        std::lock_guard<std::mutex> guard(sLock);
        sPendingRawAsset = std::string(GetEngineState()->mProjectDirectory) + kProjectRomDir + info.romFile;
    }

    SetUpStatus("recompiling " + info.title + " (the first time takes a minute)");
    const bool ok = SetupRecomp(game, (int32_t)Platform::Windows, "", target, true);
    SetUpStatus(ok ? info.title + " is set up. " + kDoneHint + ", then press Play."
                   : std::string("SETUP FAILED while building (see the log)"));
    return ok;
}

// The games that can be set up from a ROM, as the dialog lists them
struct SetUpChoice
{
    GamePackage game;
    GameInfo info;
};

std::vector<SetUpChoice> SetUpChoices()
{
    std::vector<SetUpChoice> choices;

    for (const GamePackage& game : FindGamePackages())
    {
        if (game.hasRecomp)
        {
            choices.push_back({game, ReadGameInfo(game)});
        }
    }
    return choices;
}

bool DrawSetUpGame(void*)
{
    static std::vector<SetUpChoice> choices;
    static bool listed = false;
    static int selected = 0;
    static char rom[1024] = "";
    static bool copyToProject = true;

    if (!listed)
    {
        choices = SetUpChoices();
        listed = true;
    }
    if (choices.empty())
    {
        ImGui::TextUnformatted("No N64 game package in this project can be recompiled from a ROM\n"
                               "(a Packages/<game>/Recomp folder with game.json and CMakeLists.txt).");
        const bool close = ImGui::Button("Close");
        if (close) listed = false;
        return !close;
    }
    if (selected >= (int)choices.size())
    {
        selected = 0;
    }

    const bool running = sRunning;
    if (running) ImGui::BeginDisabled();

    if (ImGui::BeginCombo("Game", choices[selected].info.title.c_str()))
    {
        for (int i = 0; i < (int)choices.size(); ++i)
        {
            if (ImGui::Selectable(choices[i].info.title.c_str(), i == selected)) selected = i;
        }
        ImGui::EndCombo();
    }
    const GameInfo& info = choices[selected].info;

    ImGui::InputText("ROM", rom, sizeof(rom));
    ImGui::SameLine();
    if (ImGui::Button("Browse...") && sHooks != nullptr && sHooks->ShowOpenFileDialog != nullptr)
    {
        sHooks->ShowOpenFileDialog("Your N64 ROM", "N64 ROM|*.z64;*.n64;*.v64", nullptr, rom, (int)sizeof(rom));
    }
    if (!info.sha1.empty())
    {
        ImGui::TextDisabled("Expects %s, sha1 %.12s... (.z64, .v64 and .n64 dumps all work)", info.title.c_str(),
                            info.sha1.c_str());
    }

    ImGui::Checkbox("Keep a copy in the project (Assets/Recomp/Rom)", &copyToProject);
    ImGui::TextWrapped(copyToProject
                           ? "The project's builds then include your ROM, so they run anywhere without asking for it. "
                             "They are for your own use: do not share them. The copy is git-ignored."
                           : "The ROM is kept in the game package's build folder: this machine only. Packaged builds "
                             "will ask the player for their ROM.");

    const bool canStart = rom[0] != 0;
    if (!canStart) ImGui::BeginDisabled();
    if (ImGui::Button("Set Up"))
    {
        if (sThread.joinable())
        {
            sThread.join();
        }
        sRunning = true;
        const GamePackage game = choices[selected].game;
        const std::string path = rom;
        const bool copy = copyToProject;
        sThread = std::thread([game, path, copy]() {
            SetUpGame(game, path, copy);
            sRunning = false;
            sFinished = true;
        });
    }
    if (!canStart) ImGui::EndDisabled();
    if (running) ImGui::EndDisabled();

    ImGui::SameLine();
    const bool close = ImGui::Button("Close");

    std::string status;
    {
        std::lock_guard<std::mutex> guard(sLock);
        status = sSetUpStatus;
    }
    if (!status.empty())
    {
        ImGui::Separator();
        ImGui::TextWrapped("%s%s", running ? "Working: " : "", status.c_str());
    }
    if (close)
    {
        listed = false; // list the packages again next time
    }
    return !close;
}
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

    // A ROM Set Up Game copied into the project: packaged with the project's builds from now
    // on (the editor otherwise finds raw assets when it scans the project)
    std::string rawAsset;
    {
        std::lock_guard<std::mutex> guard(sLock);
        rawAsset.swap(sPendingRawAsset);
    }
    if (!rawAsset.empty() && AssetManager::Get() != nullptr)
    {
        RawAssetEntry entry;
        entry.mAbsolutePath = rawAsset;
        entry.mEngineAsset = false;
        AssetManager::Get()->AddRawAssetEntry(entry);
    }
}

void N64Dependencies::RegisterSetUpGame(EditorUIHooks* hooks, uint64_t hookId)
{
    sHooks = hooks;
    sHookId = hookId;
    if (hooks == nullptr || hooks->AddMenuItem == nullptr)
    {
        return;
    }
    hooks->AddMenuItem(hookId, "Tools", "Recomp/N64/Set Up Game...",
        [](void*) {
            if (sHooks != nullptr && sHooks->OpenModal != nullptr)
            {
                sHooks->OpenModal(sHookId, kSetUpTitle, DrawSetUpGame, nullptr);
            }
            else
            {
                LogWarning("[n64] Set Up Game needs an editor with addon dialogs (OpenModal)");
            }
        },
        nullptr, nullptr);
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
    const char* modeValues[] = {"auto", "decomp", "recomp"};
    const BuildMode parsed = ParseMode(modeValue);
    int mode = parsed == BuildMode::Decomp ? 1 : parsed == BuildMode::Recomp ? 2 : 0;
    const char* modes[] = {"Auto (as each game was last set up)", "Decomp", "Recomp (from your ROM)"};
    if (ImGui::Combo("Build mode", &mode, modes, 3) && ctx->SetProfileSetting != nullptr)
    {
        ctx->SetProfileSetting(kModeOption, modeValues[mode]);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Decomp: the game is compiled from its decompilation (Native/), with the asset pack\n"
                          "cut from your ROM. Every target.\n"
                          "Recomp: the game is recompiled from your ROM by N64Recomp (the package's Recomp/\n"
                          "config) and runs on the recomp runtime; it boots from the whole ROM. For games\n"
                          "whose decomp is unfinished. Windows only so far.\n"
                          "Auto: recomp for a game set up from a ROM (Tools > Recomp > N64 > Set Up Game) or\n"
                          "without a decomp, decomp otherwise.\n"
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
    if (mode != 1)
    {
        if (ImGui::InputText("ROM (.z64)", rom, sizeof(rom)) && ctx->SetProfileSetting != nullptr)
        {
            ctx->SetProfileSetting(kRomOption, rom);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Your own ROM of the game, big-endian (.z64), for recomp mode. Empty: the copy Set\n"
                              "Up Game keeps in the project (Assets/Recomp/Rom), else the path in the game\n"
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
        SetupAllAsync(ctx->basePlatform, {decomp, modeValues[mode], rom});
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

void N64Dependencies::RegisterSetUpGame(EditorUIHooks*, uint64_t)
{
}

#endif
