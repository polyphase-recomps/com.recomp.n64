/**
 * @file N64GamePlayerImpl.h
 * @brief Implementation of the N64 game player node (Game/N64GamePlayer.h). Included by exactly
 *        one .cpp of a game package, after its <Name>Player.h.
 *
 * The game boots through Game/N64Launcher.h (the game data it tries, in order, are listed in
 * Game/N64LauncherImpl.h): started by a launcher scene (N64.StartRecomp()), or by this node on
 * its first frame when nothing started it before.
 */
#pragma once

#include "Game/N64GameApi.h"
#include "Game/N64LauncherImpl.h"
#include "Game/N64Provider.h"

// com.recomp.mod.base: mod settings, resolution scaler, shared menus
#include "ModBaseDisplay.h"
#include "ModBaseProvider.h"
#include "ModBaseSettings.h"

#include "AssetManager.h"
#include "Engine.h"
#include "Input/Input.h"
#include "Input/InputTypes.h"
#include "Nodes/Widgets/Quad.h"
#include "Plugins/PolyphaseEngineAPI.h"
#include "System/System.h"

#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#if PLATFORM_3DS
#include "Renderer.h"
#include "Graphics/C3D/C3dTypes.h"
#endif

// com.recomp.netplay, when the game's package depends on it: networked multiplayer (a session's
// inputs drive the game on every machine; see its NetplayN64.h). Other games build as before.
#if defined(__has_include)
#if __has_include("NetplayN64.h")
#include "NetplayN64.h"
#define N64_GAME_NETPLAY 1
#endif
#endif
#ifndef N64_GAME_NETPLAY
#define N64_GAME_NETPLAY 0
#endif

// (the extra level makes the class name macro expand before FORCE_LINK pastes it)
#define N64_GAME_FORCE_LINK_DEF(name) FORCE_LINK_DEF(name)
N64_GAME_FORCE_LINK_DEF(N64_GAME_PLAYER);
DEFINE_NODE(N64_GAME_PLAYER, Node3D);

PolyphaseEngineAPI* N64_GAME_PLAYER::sAPI = nullptr;
uint32_t N64_GAME_PLAYER::sAudioStream = 0;
bool N64_GAME_PLAYER::sBootAttempted = false;

namespace N64GamePlayerDetail
{
const float kFrameTime = 1.0f / 60.0f;
const int kMaxFramesPerTick = 3;
#if N64_GAME_NETPLAY
// this machine's controller (port 1's input), which a netplay session sends to the others
PortPad sLocalPad = {};
#endif
#if PLATFORM_DOLPHIN
// com.recomp.n64's GPU path (port_host.h). Weak: a game library built before it existed
// still links (the picture then fills the screen as before).
extern "C" void n64_set_display_rect(float x, float y, float width, float height) __attribute__((weak));
#endif

// With a GPU backend the game draws straight into the screen: tell it where the resolution
// scaler puts the 320x240 picture (4:3, integer, ...; the console's 16:9 setting included).
void ApplyGpuDisplayRect()
{
#if PLATFORM_DOLPHIN
    if (n64_set_display_rect != nullptr)
    {
        EngineState* engine = GetEngineState();
        const RecompRect r = Recomp_DisplayFit(320, 240, 4.0f / 3.0f, (float)engine->mWindowWidth,
                                               (float)engine->mWindowHeight, engine->mAspectRatioScale,
                                               Recomp_DisplaySettings());
        n64_set_display_rect(r.x, r.y, r.w, r.h);
    }
#endif
    N64Provider::Get().SetFrame(320, 240);
}

#if PLATFORM_3DS
// The game's latest frame, replayed in the engine's forward pass (after the scene, before the
// UI) on the top screen. The engine caches which shader and material it last bound; after
// citro3d state of our own it has to bind them again.
uint64_t sRenderPassId = 0;

void RenderGame3ds(void*)
{
    if (gC3dContext.mCurrentScreen != 0)
    {
        return;
    }
    port_gpu_c3d_render(400, 240);
    gC3dContext.mLastBoundShaderId = ShaderId::Count;
    gC3dContext.mLastBoundMaterial = nullptr;
}
#endif

#if EDITOR && N64_HAS_NATIVE_LIB
// port_set_development is new in com.recomp.n64: a game library built before it still links
// (weakly: the stand-in does nothing) until Setup Dependencies rebuilds it.
extern "C" void n64_game_set_development_stand_in(int) {}
#if defined(_MSC_VER)
#pragma comment(linker, "/alternatename:port_set_development=n64_game_set_development_stand_in")
#else
extern "C" void port_set_development(int) __attribute__((weak, alias("n64_game_set_development_stand_in")));
#endif
#endif

#if N64_HAS_NATIVE_LIB
// n64_set_recomp_dir is new in com.recomp.n64 too: a library built before it (any decomp build
// so far) links against this stand-in, which does nothing.
extern "C" void n64_game_set_recomp_dir_stand_in(const char*) {}
#if defined(_MSC_VER)
#pragma comment(linker, "/alternatename:n64_set_recomp_dir=n64_game_set_recomp_dir_stand_in")
#else
extern "C" void n64_set_recomp_dir(const char*) __attribute__((weak, alias("n64_game_set_recomp_dir_stand_in")));
#endif
#endif

#ifndef N64_GAME_CUSTOM_PAD_MAPPING
// The controller layout: gamepad buttons -> N64 buttons, where they sit on an N64 pad. A game
// with a layout of its own defines N64_GAME_CUSTOM_PAD_MAPPING and this function itself.
// (The left stick is the N64 stick, and keyboard port 1 is mapped by the player.)
void N64GameMapGamepad(int port, PortPad& pad)
{
    auto down = [port](int32_t button) { return INP_IsGamepadButtonDown(button, port); };
    const float cX = INP_GetGamepadAxisValue(GAMEPAD_AXIS_RTHUMB_X, port);
    const float cY = INP_GetGamepadAxisValue(GAMEPAD_AXIS_RTHUMB_Y, port);

#if PLATFORM_3DS
    // A / B as on the N64, X / Y and the C stick (New 3DS) = C buttons, L = Z, R = R, ZL = L
    if (down(GAMEPAD_A)) pad.buttons |= N64_BTN_A;
    if (down(GAMEPAD_B)) pad.buttons |= N64_BTN_B;
    if (down(GAMEPAD_X)) pad.buttons |= N64_BTN_CUP;
    if (down(GAMEPAD_Y)) pad.buttons |= N64_BTN_CLEFT;
    if (down(GAMEPAD_L1)) pad.buttons |= N64_BTN_Z;
    if (down(GAMEPAD_R1) || down(GAMEPAD_R2)) pad.buttons |= N64_BTN_R;
    if (down(GAMEPAD_L2) || down(GAMEPAD_SELECT)) pad.buttons |= N64_BTN_L;
#elif PLATFORM_DOLPHIN
#if PLATFORM_WII
    // HOME leaves the game (back to the Homebrew Channel)
    if (down(GAMEPAD_HOME))
    {
        exit(0);
    }
#endif
    if (GetEngineState()->mInput.mGamepads[port].mType == GamepadType::Wiimote)
    {
        // Wii Remote + Nunchuk: A, B (trigger) as on the N64, Z = Z, C = C down, - = L,
        // 1 / 2 = R / C left, d-pad = C buttons
        if (down(GAMEPAD_A)) pad.buttons |= N64_BTN_A;
        if (down(GAMEPAD_B)) pad.buttons |= N64_BTN_B;
        if (down(GAMEPAD_Z)) pad.buttons |= N64_BTN_Z;
        if (down(GAMEPAD_C)) pad.buttons |= N64_BTN_CDOWN;
        if (down(GAMEPAD_SELECT)) pad.buttons |= N64_BTN_L;
        if (down(GAMEPAD_X)) pad.buttons |= N64_BTN_R;
        if (down(GAMEPAD_Y)) pad.buttons |= N64_BTN_CLEFT;
        if (down(GAMEPAD_UP)) pad.buttons |= N64_BTN_CUP;
        if (down(GAMEPAD_DOWN)) pad.buttons |= N64_BTN_CDOWN;
        if (down(GAMEPAD_LEFT)) pad.buttons |= N64_BTN_CLEFT;
        if (down(GAMEPAD_RIGHT)) pad.buttons |= N64_BTN_CRIGHT;
        if (down(GAMEPAD_START)) pad.buttons |= N64_BTN_START;
        return;
    }
    // GameCube pad / Classic Controller: A, B as on the N64, X / Y = C down / C left,
    // L (analog) = Z, R = R, Z = L, C stick = C buttons
    if (down(GAMEPAD_A)) pad.buttons |= N64_BTN_A;
    if (down(GAMEPAD_B)) pad.buttons |= N64_BTN_B;
    if (down(GAMEPAD_X)) pad.buttons |= N64_BTN_CDOWN;
    if (down(GAMEPAD_Y)) pad.buttons |= N64_BTN_CLEFT;
    if (down(GAMEPAD_L1) || INP_GetGamepadAxisValue(GAMEPAD_AXIS_LTRIGGER, port) > 0.3f) pad.buttons |= N64_BTN_Z;
    if (down(GAMEPAD_R1) || INP_GetGamepadAxisValue(GAMEPAD_AXIS_RTRIGGER, port) > 0.3f) pad.buttons |= N64_BTN_R;
    if (down(GAMEPAD_Z)) pad.buttons |= N64_BTN_L;
#else
    // Xbox-style pads: A = A, X = B (where they sit), B / Y = C down / C left, right stick =
    // C buttons, left trigger = Z, bumpers / right trigger = L / R
    if (down(GAMEPAD_A)) pad.buttons |= N64_BTN_A;
    if (down(GAMEPAD_X)) pad.buttons |= N64_BTN_B;
    if (down(GAMEPAD_B) || down(GAMEPAD_R_DOWN)) pad.buttons |= N64_BTN_CDOWN;
    if (down(GAMEPAD_Y) || down(GAMEPAD_R_LEFT)) pad.buttons |= N64_BTN_CLEFT;
    if (down(GAMEPAD_R_UP)) pad.buttons |= N64_BTN_CUP;
    if (down(GAMEPAD_R_RIGHT)) pad.buttons |= N64_BTN_CRIGHT;
    if (down(GAMEPAD_L2) || INP_GetGamepadAxisValue(GAMEPAD_AXIS_LTRIGGER, port) > 0.3f) pad.buttons |= N64_BTN_Z;
    if (down(GAMEPAD_L1)) pad.buttons |= N64_BTN_L;
    if (down(GAMEPAD_R1) || INP_GetGamepadAxisValue(GAMEPAD_AXIS_RTRIGGER, port) > 0.3f) pad.buttons |= N64_BTN_R;
#endif
    if (down(GAMEPAD_START)) pad.buttons |= N64_BTN_START;
    if (down(GAMEPAD_UP)) pad.buttons |= N64_BTN_DUP;
    if (down(GAMEPAD_DOWN)) pad.buttons |= N64_BTN_DDOWN;
    if (down(GAMEPAD_LEFT)) pad.buttons |= N64_BTN_DLEFT;
    if (down(GAMEPAD_RIGHT)) pad.buttons |= N64_BTN_DRIGHT;
    // the right / C stick as C buttons
    if (cY > 0.5f) pad.buttons |= N64_BTN_CUP;
    if (cY < -0.5f) pad.buttons |= N64_BTN_CDOWN;
    if (cX < -0.5f) pad.buttons |= N64_BTN_CLEFT;
    if (cX > 0.5f) pad.buttons |= N64_BTN_CRIGHT;
}
#endif
} // namespace N64GamePlayerDetail

N64_GAME_PLAYER::N64_GAME_PLAYER()
{
    // Relative paths are resolved against the project directory.
    mRomPath = N64LauncherDetail::kAssetPackPath;
}

N64_GAME_PLAYER::~N64_GAME_PLAYER()
{
}

void N64_GAME_PLAYER::SetEngineAPI(PolyphaseEngineAPI* api)
{
    sAPI = api;
}

void N64_GAME_PLAYER::ShutdownRuntime()
{
#if PLATFORM_3DS
    if (N64GamePlayerDetail::sRenderPassId != 0 && Renderer::Get() != nullptr)
    {
        Renderer::Get()->UnregisterCustomRenderPass(N64GamePlayerDetail::sRenderPassId);
    }
    N64GamePlayerDetail::sRenderPassId = 0;
#endif
    if (sAudioStream != 0 && sAPI && sAPI->Audio_CloseStream)
    {
        sAPI->Audio_CloseStream(sAudioStream);
    }
    sAudioStream = 0;
    if (sBootAttempted || N64Launcher::IsStarted())
    {
        n64_shutdown();
        sBootAttempted = false;
    }
    N64Launcher::Reset();
    port_set_fault_containment(0);
    port_set_log_sink(nullptr);
}

void N64_GAME_PLAYER::Create()
{
    Node3D::Create();
    SetName(GetClassName());
    EnsureDisplayQuad();
}

void N64_GAME_PLAYER::Destroy()
{
    // Only touch the quad through the WeakPtr: an auto-created child is already gone here.
    if (Quad* quad = mBoundQuad.Get())
    {
        quad->SetTexture(nullptr);
    }
    mDisplayQuad = nullptr;
    mBoundQuad = WeakPtr<Quad>();

    if (mFrameTexture)
    {
        mFrameTexture->Destroy();
        mFrameTexture = nullptr;
    }
    Node3D::Destroy();
}

void N64_GAME_PLAYER::GatherProperties(std::vector<Property>& outProps)
{
    Node3D::GatherProperties(outProps);
    outProps.push_back(Property(DatumType::String, "ROM Path", this, &mRomPath));
}

void N64_GAME_PLAYER::SaveStream(Stream& stream, Platform platform)
{
    Node3D::SaveStream(stream, platform);
    stream.WriteString(mRomPath);
}

void N64_GAME_PLAYER::LoadStream(Stream& stream, Platform platform, uint32_t version)
{
    Node3D::LoadStream(stream, platform, version);
    stream.ReadString(mRomPath);
}

void N64_GAME_PLAYER::EnsureBooted()
{
    using namespace N64GamePlayerDetail;

    if (sBootAttempted)
    {
        return;
    }
    sBootAttempted = true;

    // a launcher scene may have started the game already (N64.StartRecomp)
    N64Launcher::BootFromPlayer(mRomPath, true);
#if PLATFORM_3DS
    if (sRenderPassId == 0 && Renderer::Get() != nullptr)
    {
        sRenderPassId = Renderer::Get()->RegisterCustomRenderPass(RenderGame3ds, nullptr);
    }
#endif
}

void N64_GAME_PLAYER::SendInput()
{
    // Controller port n is driven by gamepad n; the keyboard also drives port 1.
    for (int32_t port = 0; port < 4; port++)
    {
        PortPad pad = {};
        float stickX = 0.0f;
        float stickY = 0.0f;
        const bool hasGamepad = INP_IsGamepadConnected(port);

        pad.connected = (port == 0 || hasGamepad) ? 1 : 0;

        if (port == 0 && sAPI && sAPI->IsKeyDown)
        {
            auto down = [](int32_t key) { return sAPI->IsKeyDown(key); };

            // arrows = stick, X / Z = A / B, C = Z, Enter = Start, A / S = L / R, I J K L = C buttons
            if (down(POLYPHASE_KEY_LEFT))  stickX -= 1.0f;
            if (down(POLYPHASE_KEY_RIGHT)) stickX += 1.0f;
            if (down(POLYPHASE_KEY_UP))    stickY += 1.0f;
            if (down(POLYPHASE_KEY_DOWN))  stickY -= 1.0f;

            if (down(POLYPHASE_KEY_X))     pad.buttons |= N64_BTN_A;
            if (down(POLYPHASE_KEY_Z))     pad.buttons |= N64_BTN_B;
            if (down(POLYPHASE_KEY_C))     pad.buttons |= N64_BTN_Z;
            if (down(POLYPHASE_KEY_ENTER)) pad.buttons |= N64_BTN_START;
            if (down(POLYPHASE_KEY_A))     pad.buttons |= N64_BTN_L;
            if (down(POLYPHASE_KEY_S))     pad.buttons |= N64_BTN_R;
            if (down(POLYPHASE_KEY_I))     pad.buttons |= N64_BTN_CUP;
            if (down(POLYPHASE_KEY_K))     pad.buttons |= N64_BTN_CDOWN;
            if (down(POLYPHASE_KEY_J))     pad.buttons |= N64_BTN_CLEFT;
            if (down(POLYPHASE_KEY_L))     pad.buttons |= N64_BTN_CRIGHT;
        }

        if (hasGamepad)
        {
            stickX += INP_GetGamepadAxisValue(GAMEPAD_AXIS_LTHUMB_X, port);
            stickY += INP_GetGamepadAxisValue(GAMEPAD_AXIS_LTHUMB_Y, port);
            N64GamePlayerDetail::N64GameMapGamepad(port, pad);
        }

        // The N64 stick reports roughly -80..80.
        auto toStick = [](float value)
        {
            if (value > 1.0f) value = 1.0f;
            if (value < -1.0f) value = -1.0f;
            return (signed char)(value * 80.0f);
        };
        pad.stick_x = toStick(stickX);
        pad.stick_y = toStick(stickY);

        // an open settings menu (RecompMenuController) navigates with the gamepad: the game
        // gets nothing, and not the press that closes it either
        static bool sHoldUntilRelease = false;
        if (port == 0)
        {
            const bool captured = Recomp_IsInputCaptured();
            if (captured) sHoldUntilRelease = true;
            else if (sHoldUntilRelease && pad.buttons == 0) sHoldUntilRelease = false;
        }
        if (sHoldUntilRelease)
        {
            pad.buttons = 0;
            pad.stick_x = pad.stick_y = 0;
        }

#if N64_GAME_NETPLAY
        if (port == 0)
        {
            N64GamePlayerDetail::sLocalPad = pad;
        }
#endif
        n64_set_pad(port, &pad);
    }
}

void N64_GAME_PLAYER::SubmitAudio()
{
    int frames = 0;
    int sampleRate = 0;
    const short* pcm = n64_audio(&frames, &sampleRate);

    if (pcm == nullptr || frames <= 0 || sAPI == nullptr || sAPI->Audio_OpenStream == nullptr)
    {
        return;
    }
    if (sAudioStream == 0)
    {
        // 16-bit stereo; 0 means this platform has no streaming audio (the game then runs silent).
        sAudioStream = sAPI->Audio_OpenStream(uint32_t(sampleRate), 2, 16);
        if (sAudioStream == 0)
        {
            return;
        }
    }
    // The engine's stream takes little-endian 16-bit samples on every platform; the game
    // produces them in host byte order, which is big-endian on GameCube / Wii.
    const uint16_t endianProbe = 1;
    if (*(const uint8_t*)&endianProbe == 0)
    {
        static std::vector<uint8_t> swapped;
        swapped.resize(size_t(frames) * 4);
        const uint8_t* src = (const uint8_t*)pcm;
        for (size_t i = 0; i < swapped.size(); i += 2)
        {
            swapped[i] = src[i + 1];
            swapped[i + 1] = src[i];
        }
        sAPI->Audio_SubmitStreamBuffer(sAudioStream, swapped.data(), uint32_t(swapped.size()));
        return;
    }
    // The backend copies the samples; if its queue is full this frame's audio is dropped.
    sAPI->Audio_SubmitStreamBuffer(sAudioStream, (const uint8_t*)pcm, uint32_t(frames) * 4);
}

void N64_GAME_PLAYER::EnsureDisplayQuad()
{
    if (mDisplayQuad != nullptr)
    {
        return;
    }
    mDisplayQuad = CreateChild<Quad>(N64_GAME_TITLE " Display");
    mDisplayQuad->SetAnchorMode(AnchorMode::FullStretch);
    mDisplayQuad->SetSize(1.0f, 1.0f);
    mDisplayQuad->SetObjectFit(ObjectFit::Fill);
    // Hidden until the first game frame is bound: a Quad without a texture falls back to the
    // engine's white texture, which is not available on every console build.
    mDisplayQuad->SetVisible(false);
    mBoundQuad = ResolveWeakPtr<Quad>(mDisplayQuad);
}

void N64_GAME_PLAYER::UpdateDisplayTexture()
{
    int width = 0;
    int height = 0;
    const unsigned char* pixels = n64_framebuffer(&width, &height);

    if (pixels == nullptr || width <= 0 || height <= 0)
    {
        return;
    }

    EnsureDisplayQuad();

    if (mFrameTexture == nullptr ||
        mFrameTexture->GetWidth() != uint32_t(width) ||
        mFrameTexture->GetHeight() != uint32_t(height))
    {
        if (mFrameTexture)
        {
            mFrameTexture->Destroy();
            mFrameTexture = nullptr;
        }

        // Must be a transient asset: Quad drops textures the AssetManager does not know.
        mFrameTexture = NewTransientAsset<Texture>();
        mFrameTexture->SetName("T_" N64_GAME_ID "_Frame");
        mFrameTexture->SetMipmapped(false);
        mFrameTexture->SetFilterType(Recomp_DisplayFilterLinear() ? FilterType::Linear : FilterType::Nearest);
        mFrameTexture->SetWrapMode(WrapMode::Clamp);
        mFrameTexture->Init(uint32_t(width), uint32_t(height), (uint8_t*)pixels);
        mFrameTexture->Create();
    }

    if (Quad* quad = mBoundQuad.Get())
    {
        quad->SetTexture(mFrameTexture);
        quad->SetVisible(true);
        // the resolution scaler (mod settings "Screen" / "Filter") places our own display;
        // a Quad the user bound keeps the layout they gave it
        if (quad == mDisplayQuad && Recomp_DisplayApply(quad, mFrameTexture, width, height, 4.0f / 3.0f))
        {
            mFrameTexture->UpdatePixels(pixels, size_t(width) * size_t(height) * 4);
            mFrameTexture->Destroy(); // filter changed: a new texture next frame
            mFrameTexture = nullptr;
            return;
        }
    }
    mFrameTexture->UpdatePixels(pixels, size_t(width) * size_t(height) * 4);
    N64Provider::Get().SetFrame(width, height);
    Recomp_DisplayApplyWindow(width, height);
}

void N64_GAME_PLAYER::Tick(float deltaTime)
{
    using namespace N64GamePlayerDetail;

    Node3D::Tick(deltaTime);

    EnsureBooted();
    // mod settings: written to the game once it runs, kept, saved
    ModSettings::Get().Tick(&N64Provider::Get());
    if (!n64_is_running())
    {
        return;
    }

#if N64_GAME_NETPLAY
    // com.recomp.netplay: while a session plays, every machine's input drives the game (the
    // session reboots it with the host's save when it starts, and with ours when it ends)
    {
        const std::string saveDir = GetEngineState()->mProjectDirectory + "Saves";
        NetplayN64::Configure(N64_GAME_ID, N64_GAME_TITLE, "n64", "", saveDir + "/" N64_GAME_ID ".sra");
        if (NetplaySession::IsPlaying())
        {
            SendInput(); // this machine's controller (sLocalPad)
        }
        auto reboot = [this](const std::string& save) {
            n64_shutdown();
            n64_set_save_path(save.c_str());
            std::string path = mRomPath;
            if (!path.empty() && !N64LauncherDetail::IsAbsolute(path))
            {
                path = GetEngineState()->mProjectDirectory + path;
            }
            if (!N64LauncherDetail::BootShipped() && !N64LauncherDetail::Boot(path))
            {
                LogError("netplay: the game did not boot again");
            }
        };
        const int ran = NetplayN64::RunFrames(deltaTime, sLocalPad, reboot, [this] { SubmitAudio(); }, saveDir,
                                              mFrameAccumulator);
        if (ran >= 0)
        {
            if (ran > 0)
            {
                UpdateDisplayTexture();
            }
            return;
        }
    }
#endif

    // The game logic is locked to 60 Hz; run as many game frames as real time has covered.
    mFrameAccumulator += deltaTime;
    int frames = 0;
    // With a GPU backend (GameCube / Wii) the game draws straight into the frame the engine is
    // about to present, so every engine frame needs a game frame or it would show nothing.
    // (The 3DS backend replays its last frame instead, so there the engine runs at its own pace.)
#if !PLATFORM_3DS
    if (n64_draws_to_screen() && mFrameAccumulator < kFrameTime)
    {
        mFrameAccumulator = kFrameTime;
    }
#endif
    if (n64_draws_to_screen())
    {
        ApplyGpuDisplayRect();
    }
    // Only the last of several game frames run now is shown: the others skip drawing, which is
    // most of a frame's cost on slow CPUs (3DS).
    int toRun = 0;
    for (float pending = mFrameAccumulator; pending >= kFrameTime && toRun < kMaxFramesPerTick; pending -= kFrameTime)
    {
        toRun++;
    }
    while (frames < toRun)
    {
        SendInput();
        n64_set_skip_draw(frames + 1 < toRun);
        n64_run_frame();
        SubmitAudio();
        mFrameAccumulator -= kFrameTime;
        frames++;
    }
    n64_set_skip_draw(0);
    if (mFrameAccumulator > kFrameTime)
    {
        mFrameAccumulator = 0.0f; // fell behind: drop the backlog instead of fast-forwarding
    }

    if (frames > 0)
    {
        UpdateDisplayTexture();
    }
}
