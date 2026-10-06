/**
 * @file N64GamePlayer.h
 * @brief The player node of an N64 game package: boots the game library, steps it at 60 Hz,
 *        feeds it the gamepads and keyboard, streams its audio and shows its frames on a Quad.
 *
 * Shared by the game packages made from com.recomp.n64's template (Templates/game): the
 * package's Source/N64Game.h names the game, then
 *
 *   Source/<Name>Player.h     #include "N64Game.h"  +  #include "Game/N64GamePlayer.h"
 *   Source/<Name>Player.cpp   #include "<Name>Player.h"  +  #include "Game/N64GamePlayerImpl.h"
 *
 * N64Game.h defines:
 *   N64_GAME_PLAYER     the node's class name (unique per package: MarioKart64Player)
 *   N64_GAME_ID         short id, used for file names: "mk64" (Saves/mk64.sra, Assets/mk64.n64pak)
 *   N64_GAME_PACKAGE    "com.recomp.mk64"
 *   N64_GAME_TITLE      "Mario Kart 64 (US)" (shown to players)
 *   N64_GAME_ROM_FILE   the ROM's name in the project, Assets/Recomp/Rom/<this> (Recomp/game.json rom.file)
 * and optionally N64_GAME_CUSTOM_PAD_MAPPING (then it defines N64GameMapGamepad itself, see
 * Game/N64GamePlayerImpl.h).
 */
#pragma once

#include "Nodes/3D/Node3D.h"

#include <string>

#ifndef N64_GAME_PLAYER
#error "define the game (N64_GAME_PLAYER, N64_GAME_ID, ...) in the package's Source/N64Game.h first"
#endif

struct PolyphaseEngineAPI;

class N64_GAME_PLAYER : public Node3D
{
public:
    DECLARE_NODE(N64_GAME_PLAYER, Node3D);

    N64_GAME_PLAYER();
    virtual ~N64_GAME_PLAYER();

    virtual void Create() override;
    virtual void Destroy() override;
    virtual void Tick(float deltaTime) override;
    virtual void GatherProperties(std::vector<Property>& outProps) override;

    virtual void SaveStream(Stream& stream, Platform platform) override;
    virtual void LoadStream(Stream& stream, Platform platform, uint32_t version) override;

    static void SetEngineAPI(PolyphaseEngineAPI* api);
    // Stops the game runtime; called when the addon unloads.
    static void ShutdownRuntime();

private:
    void EnsureBooted();
    void SendInput();
    void SubmitAudio();
    void EnsureDisplayQuad();
    void UpdateDisplayTexture();

    static PolyphaseEngineAPI* sAPI;
    // Engine streaming voice the game's audio is pushed to (0 = not open / unavailable).
    static uint32_t sAudioStream;
    // The game keeps its state in module globals, so there is one runtime per addon load.
    // It is booted by the first player that ticks and keeps running across play sessions.
    static bool sBootAttempted;

    class Quad* mDisplayQuad = nullptr;
    WeakPtr<class Quad> mBoundQuad;
    class Texture* mFrameTexture = nullptr;
    std::string mRomPath;
    float mFrameAccumulator = 0.0f;
};
