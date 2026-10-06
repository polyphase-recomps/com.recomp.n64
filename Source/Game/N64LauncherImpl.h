/**
 * @file N64LauncherImpl.h
 * @brief Implementation of Game/N64Launcher.h. Included by exactly one .cpp of a game package,
 *        after the game's names are defined:
 *
 *   N64_GAME_ID         "mk64": Saves/<id>.sra, Saves/<id>.rom.txt, Assets/<id>.n64pak
 *   N64_GAME_PACKAGE    "com.recomp.mk64"
 *   N64_GAME_TITLE      "Mario Kart 64 (US)"
 *   N64_GAME_ROM_FILE   the ROM's name in the project (Assets/Recomp/Rom/<this>)
 *
 * Game data, tried in order when the game starts:
 *   1. the ROM set with SetRomLocation (or picked by the player before: Saves/<id>.rom.txt)
 *   2. the asset pack the decomp build cuts from the ROM (Packages/<package>/Assets/<id>.n64pak);
 *      a recompiled game refuses it (in the editor it then boots the ROM it was built from)
 *   3. the ROM Set Up Game keeps in the project (Assets/Recomp/Rom/<rom file>)
 *   4. (player node) its ROM Path property; a packaged game (Windows, Linux) then asks the
 *      player for their ROM, and remembers it
 * A Recomp (live) build recompiles the ROM it boots from the recompiler data build_recomp.ps1
 * -Live ships in Packages/<package>/Assets/Recomp/Live.
 *
 * Only the runtime's C functions this needs are declared here, so it builds next to either the
 * shared Game/N64GameApi.h or a game's own copy of those declarations.
 */
#pragma once

#include "Game/N64Launcher.h"

#include "ModBaseLauncher.h" // com.recomp.mod.base
#include "ModBaseSettings.h"
#include "ModBaseUtil.h"

#include "Engine.h"
#include "Log.h"
#include "System/System.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#if !EDITOR && PLATFORM_WINDOWS
// MessageBoxA, for asking a packaged game's player for their ROM
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// Plain mkdir: the packaged Standalone project builds as C++14, so no <filesystem>.
#if PLATFORM_WINDOWS
#include <direct.h>
#elif PLATFORM_LINUX || PLATFORM_DOLPHIN || PLATFORM_3DS
#include <sys/stat.h>
#endif

#if !defined(N64_GAME_ID) || !defined(N64_GAME_PACKAGE) || !defined(N64_GAME_TITLE) || !defined(N64_GAME_ROM_FILE)
#error "define N64_GAME_ID, N64_GAME_PACKAGE, N64_GAME_TITLE and N64_GAME_ROM_FILE before Game/N64LauncherImpl.h"
#endif

extern "C" {
void port_set_log_sink(void (*sink)(const char* line));
void port_set_fault_containment(int enable);
void port_set_development(int on);
void n64_set_save_path(const char* path);
void n64_set_recomp_dir(const char* path);
int n64_boot(const char* rom_path);
int n64_is_running(void);
}

namespace N64LauncherDetail
{
enum class State
{
    Idle,
    Running,
    Failed
};

State sState = State::Idle;
std::string sMessage;
bool sPrepared = false;

// Game data extracted from the user's own ROM by the decomp build: asset files only, no game code.
const char* const kAssetPackPath = "Packages/" N64_GAME_PACKAGE "/Assets/" N64_GAME_ID ".n64pak";
// The user's ROM, kept in the project by Set Up Game.
const char* const kProjectRomPath = "Assets/Recomp/Rom/" N64_GAME_ROM_FILE;
// What a Recomp (live) build recompiles the ROM with (and its game.json: the ROM's sha1).
const char* const kLiveDataPath = "Packages/" N64_GAME_PACKAGE "/Assets/Recomp/Live";
// The package's own recomp config (in the editor; packaged builds have the Live copy).
const char* const kRecompDataPath = "Packages/" N64_GAME_PACKAGE "/Recomp";

std::string ProjectDir()
{
    return GetEngineState()->mProjectDirectory;
}

std::string SaveDir()
{
    return ProjectDir() + "Saves";
}

std::string ChosenRomFile()
{
    return SaveDir() + "/" N64_GAME_ID ".rom.txt";
}

void LogSink(const char* line)
{
    LogDebug("%s", line);
}

// The sha1 of the ROM the game was made from, "" when this build does not know it.
std::string ExpectedSha1()
{
    for (const char* dir : {kLiveDataPath, kRecompDataPath})
    {
        const std::string sha1 = RecompUtil::JsonString(RecompUtil::ReadText(ProjectDir() + dir + "/game.json"), "sha1");
        if (!sha1.empty())
        {
            return sha1;
        }
    }
    return "";
}

// The file as a big-endian (.z64) ROM; empty, with error set, if it is no N64 ROM.
std::vector<uint8_t> ReadRomAsZ64(const std::string& path, std::string& error)
{
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in.is_open())
    {
        error = "cannot open '" + path + "'";
        return {};
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    in.seekg(0, std::ios::beg);
    if (size < 0x101000 || size > (64 << 20) || (size % 4) != 0)
    {
        error = "'" + path + "' is not an N64 ROM (wrong size)";
        return {};
    }
    std::vector<uint8_t> rom((size_t)size);
    in.read(reinterpret_cast<char*>(rom.data()), size);
    const uint32_t magic = (uint32_t)rom[0] << 24 | (uint32_t)rom[1] << 16 | (uint32_t)rom[2] << 8 | rom[3];
    if (magic == 0x37804012u) // .v64: 16-bit words swapped
    {
        for (size_t i = 0; i + 1 < rom.size(); i += 2)
        {
            std::swap(rom[i], rom[i + 1]);
        }
    }
    else if (magic == 0x40123780u) // .n64: 32-bit words little-endian
    {
        for (size_t i = 0; i + 3 < rom.size(); i += 4)
        {
            std::swap(rom[i], rom[i + 3]);
            std::swap(rom[i + 1], rom[i + 2]);
        }
    }
    else if (magic != 0x80371240u)
    {
        error = "'" + path + "' is not an N64 ROM";
        return {};
    }
    return rom;
}

std::string HeaderTitle(const std::vector<uint8_t>& rom)
{
    std::string title(reinterpret_cast<const char*>(rom.data() + 0x20), 20);
    while (!title.empty() && (title.back() == ' ' || title.back() == '\0'))
    {
        title.pop_back();
    }
    return title;
}

std::string Trimmed(std::string line)
{
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' '))
    {
        line.pop_back();
    }
    return line;
}

bool IsAbsolute(const std::string& path)
{
    return path.size() > 1 && (path[1] == ':' || path[0] == '/' || path[0] == '\\');
}

// What every boot needs once: the log, crash containment, the save, the live data.
void Prepare()
{
    if (sPrepared)
    {
        return;
    }
    sPrepared = true;
    port_set_log_sink(LogSink);
    // A crash in game code stops the game, not the editor.
    port_set_fault_containment(1);
    // Unlocks, records and options persist in the project's Saves folder.
    const std::string saveDir = SaveDir();
#if PLATFORM_WINDOWS
    _mkdir(saveDir.c_str()); // fails harmlessly if it already exists
#elif PLATFORM_LINUX || PLATFORM_DOLPHIN || PLATFORM_3DS
    mkdir(saveDir.c_str(), 0755);
#endif
    n64_set_save_path((saveDir + "/" N64_GAME_ID ".sra").c_str());
    // a Recomp (live) build recompiles the ROM from this when it boots
    n64_set_recomp_dir((ProjectDir() + kLiveDataPath).c_str());
#if EDITOR
    // The developer's machine: a recompiled game may fall back to the ROM it was built from.
    port_set_development(1);
#endif
}

bool Boot(const std::string& path)
{
    return !path.empty() && SYS_DoesFileExist(path.c_str(), false) && n64_boot(path.c_str());
}

// The chosen ROM, then what the project ships.
bool BootShipped()
{
    const std::string chosen = N64Launcher::GetRomLocation();
    return Boot(chosen) || Boot(ProjectDir() + kAssetPackPath) || Boot(ProjectDir() + kProjectRomPath);
}

void Finish(bool booted, const std::string& failure)
{
    sState = booted ? State::Running : State::Failed;
    sMessage = booted ? std::string(N64_GAME_TITLE " is running") : failure;
    if (booted)
    {
        LogDebug("%s", sMessage.c_str());
    }
    else
    {
        LogError("%s", sMessage.c_str());
    }
}

std::string NoDataMessage()
{
    return std::string(N64_GAME_TITLE ": no game data. Set your ROM (N64.SetRomLocation), or keep it in the project "
                       "at '") + kProjectRomPath + "' (Tools > Recomp > N64 > Set Up Game)";
}

#if !EDITOR && (PLATFORM_WINDOWS || PLATFORM_LINUX)
// Asks the player for their ROM (.z64, .v64 or .n64) until one boots or they cancel, and
// remembers it.
bool AskForRom()
{
    const char* text = N64_GAME_TITLE " needs your own copy of the game: choose your ROM of " N64_GAME_TITLE
                       " (.z64, .v64 or .n64). It is remembered for next time.";
    for (int attempt = 0; attempt < 5; ++attempt)
    {
#if PLATFORM_WINDOWS
        if (MessageBoxA(nullptr, attempt == 0 ? text : "That file is not a ROM of " N64_GAME_TITLE ". Choose another?",
                        N64_GAME_TITLE, MB_OKCANCEL | MB_ICONINFORMATION) != IDOK)
        {
            return false;
        }
#else
        LogWarning("%s", text);
#endif
        const std::string picked = N64Launcher::BrowseForRom();
        if (picked.empty())
        {
            return false;
        }
        std::string message;
        if (N64Launcher::SetRomLocation(picked, message) && n64_boot(picked.c_str()))
        {
            return true;
        }
        LogError("%s: %s", N64_GAME_TITLE, message.c_str());
    }
    return false;
}
#endif
} // namespace N64LauncherDetail

bool N64Launcher::CheckRom(const std::string& path, std::string& message)
{
    using namespace N64LauncherDetail;

    std::string error;
    const std::vector<uint8_t> rom = ReadRomAsZ64(path, error);
    if (rom.empty())
    {
        message = error;
        return false;
    }
    const std::string title = HeaderTitle(rom);
    const std::string want = ExpectedSha1();
    if (!want.empty())
    {
        const std::string sha1 = RecompUtil::Sha1Hex(rom);
        if (sha1 != want)
        {
            message = "'" + title + "' is not the ROM of " N64_GAME_TITLE " this game is made from (sha1 " + sha1 +
                      ", expected " + want + "): another region or revision, or a bad dump?";
            return false;
        }
        message = title + ": the ROM of " N64_GAME_TITLE;
        return true;
    }
    message = title + ": an N64 ROM (this build cannot check which game it is)";
    return true;
}

bool N64Launcher::SetRomLocation(const std::string& path, std::string& message)
{
    using namespace N64LauncherDetail;

    if (!CheckRom(path, message))
    {
        return false;
    }
    const std::string saveDir = SaveDir();
#if PLATFORM_WINDOWS
    _mkdir(saveDir.c_str());
#elif PLATFORM_LINUX || PLATFORM_DOLPHIN || PLATFORM_3DS
    mkdir(saveDir.c_str(), 0755);
#endif
    std::ofstream out(ChosenRomFile().c_str(), std::ios::trunc);
    out << path << "\n";
    if (!out.good())
    {
        message = "cannot write " + ChosenRomFile();
        return false;
    }
    return true;
}

std::string N64Launcher::GetRomLocation()
{
    using namespace N64LauncherDetail;

    std::ifstream in(ChosenRomFile().c_str());
    std::string path;
    std::getline(in, path);
    path = Trimmed(path);
    if (!path.empty() && !IsAbsolute(path))
    {
        path = ProjectDir() + path;
    }
    return path;
}

void N64Launcher::ClearRomLocation()
{
    remove(N64LauncherDetail::ChosenRomFile().c_str());
}

std::string N64Launcher::BrowseForRom()
{
#if PLATFORM_WINDOWS || PLATFORM_LINUX
    const std::vector<std::string> picked = SYS_OpenFileDialog();
    return picked.empty() ? std::string() : picked[0];
#else
    return std::string();
#endif
}

bool N64Launcher::LoadMods()
{
    return ModSettings::Get().Bind(N64_GAME_PACKAGE) != nullptr;
}

bool N64Launcher::StartRecomp(std::string& message)
{
    using namespace N64LauncherDetail;

    if (sState == State::Running && n64_is_running())
    {
        message = sMessage;
        return true;
    }
    // what the player set up goes to the game as it starts
    if (ModSettings::Get().GetMap() != nullptr)
    {
        ModSettings::Get().Save();
    }
    Prepare();
    const std::string chosen = GetRomLocation();
    std::string check;
    if (!chosen.empty() && !CheckRom(chosen, check))
    {
        Finish(false, check);
        message = sMessage;
        return false;
    }
    Finish(BootShipped(), chosen.empty() ? NoDataMessage()
                                         : "'" + chosen + "' did not start " N64_GAME_TITLE " (see the log)");
    message = sMessage;
    return sState == State::Running;
}

bool N64Launcher::IsStarted()
{
    return N64LauncherDetail::sState == N64LauncherDetail::State::Running;
}

const char* N64Launcher::GetStatus()
{
    using namespace N64LauncherDetail;

    switch (sState)
    {
    case State::Running: return n64_is_running() ? "running" : "failed"; // (a fault stops it)
    case State::Failed: return "failed";
    default: return "idle";
    }
}

std::string N64Launcher::GetStatusMessage()
{
    return N64LauncherDetail::sMessage;
}

bool N64Launcher::BootFromPlayer(const std::string& romPath, bool askPlayer)
{
    using namespace N64LauncherDetail;

    if (IsStarted())
    {
        return true;
    }
    Prepare();
    std::string path = romPath;
    if (!path.empty() && !IsAbsolute(path))
    {
        path = ProjectDir() + path;
    }
    // (the ROM Path property defaults to the asset pack BootShipped tries already)
    bool booted = BootShipped() || (path != ProjectDir() + kAssetPackPath && Boot(path));
#if !EDITOR && (PLATFORM_WINDOWS || PLATFORM_LINUX)
    // A packaged game without game data of its own: the player points it at their ROM once.
    if (!booted && askPlayer)
    {
        booted = AskForRom();
    }
#else
    (void)askPlayer;
#endif
    Finish(booted, NoDataMessage() + "; also tried '" + path + "'");
    return booted;
}

void N64Launcher::Reset()
{
    N64LauncherDetail::sState = N64LauncherDetail::State::Idle;
    N64LauncherDetail::sMessage.clear();
    N64LauncherDetail::sPrepared = false;
}

bool N64Launcher::HasShippedData()
{
    using namespace N64LauncherDetail;

    return SYS_DoesFileExist((ProjectDir() + kAssetPackPath).c_str(), false) ||
           SYS_DoesFileExist((ProjectDir() + kProjectRomPath).c_str(), false);
}

namespace N64LauncherDetail
{
// The game as a mod.base launcher: the same calls as above.
class RecompGameLauncherN64 : public RecompGameLauncher
{
public:
    const char* RuntimeId() const override { return "n64"; }
    std::string GamePackage() const override { return N64_GAME_PACKAGE; }
    std::string GameTitle() const override { return N64_GAME_TITLE; }
    bool CheckRom(const std::string& path, std::string& message) override { return N64Launcher::CheckRom(path, message); }
    bool SetRomLocation(const std::string& path, std::string& message) override
    {
        return N64Launcher::SetRomLocation(path, message);
    }
    std::string GetRomLocation() override { return N64Launcher::GetRomLocation(); }
    void ClearRomLocation() override { N64Launcher::ClearRomLocation(); }
    bool HasShippedData() override { return N64Launcher::HasShippedData(); }
    bool StartGame(std::string& message) override { return N64Launcher::StartRecomp(message); }
    bool IsStarted() override { return N64Launcher::IsStarted(); }
    std::string LastMessage() override { return N64Launcher::GetStatusMessage(); }
};
} // namespace N64LauncherDetail

RecompGameLauncher* N64Launcher::AsRecompLauncher()
{
    static N64LauncherDetail::RecompGameLauncherN64 sLauncher;
    return &sLauncher;
}
