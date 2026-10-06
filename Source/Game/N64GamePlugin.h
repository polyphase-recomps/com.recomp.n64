/**
 * @file N64GamePlugin.h
 * @brief The native addon of an N64 game package: registers the player node, the `N64` Lua
 *        table and the game's mod.base provider. Included by exactly one .cpp of a game package
 *        (the template's Source/Com<Name>.cpp), after its N64Game.h, which also defines
 *        N64_GAME_PLUGIN_ENTRY: PolyphasePlugin_GetDesc_<package id with _ for .>, the entry point
 *        name of a packaged (statically linked) build.
 */
#pragma once

#include "Plugins/PolyphasePluginAPI.h"
#include "Plugins/PolyphaseEngineAPI.h"

#include "Game/N64GamePlayer.h"
#include "Game/N64Launcher.h"
#include "Game/N64Lua.h"

#include "ModBaseLauncher.h" // com.recomp.mod.base
#include "Game/N64Provider.h"

#ifndef N64_GAME_PLUGIN_ENTRY
#error "N64Game.h must define N64_GAME_PLUGIN_ENTRY (PolyphasePlugin_GetDesc_com_recomp_<id>)"
#endif

// (the extra level makes the class name macro expand before FORCE_LINK pastes it)
#define N64_GAME_FORCE_LINK_CALL(name) FORCE_LINK_CALL(name)

namespace N64GamePluginDetail
{
PolyphaseEngineAPI* sEngineAPI = nullptr;

int OnLoad(PolyphaseEngineAPI* api)
{
    sEngineAPI = api;
    N64_GAME_PLAYER::SetEngineAPI(api);
    N64_GAME_FORCE_LINK_CALL(N64_GAME_PLAYER);
    // com.recomp.mod.base (mod settings, Recomp / Mods Lua, Mods windows) sees the game
    Recomp_RegisterProvider(&N64Provider::Get());
    // ... and can start it from a launcher (ModBaseLauncher.h)
    Recomp_RegisterLauncher(N64Launcher::AsRecompLauncher());

    if (api && api->LogDebug)
    {
        api->LogDebug(N64_GAME_PACKAGE " loaded!");
    }
    return 0;
}

void OnUnload()
{
    // The game runs its threads as fibers on the engine's main thread; stop them
    // before the module (and the code they would resume into) goes away.
    N64_GAME_PLAYER::ShutdownRuntime();
    Recomp_UnregisterLauncher(N64Launcher::AsRecompLauncher());
    Recomp_UnregisterProvider(&N64Provider::Get());
    N64_GAME_PLAYER::SetEngineAPI(nullptr);

    if (sEngineAPI && sEngineAPI->LogDebug)
    {
        sEngineAPI->LogDebug(N64_GAME_PACKAGE " unloaded.");
    }
    sEngineAPI = nullptr;
}

void Tick(float)
{
}

void TickEditor(float)
{
}

void RegisterTypes(void*)
{
}

void RegisterScriptFuncs(lua_State* L)
{
    N64Lua::Register(L, sEngineAPI);
}

#if EDITOR
void RegisterEditorUI(EditorUIHooks*, uint64_t)
{
}
#endif

int FillDesc(PolyphasePluginDesc* desc)
{
    desc->apiVersion = OCTAVE_PLUGIN_API_VERSION;
    desc->pluginName = N64_GAME_PACKAGE;
    desc->pluginVersion = "1.0.0";
    desc->OnLoad = OnLoad;
    desc->OnUnload = OnUnload;
    desc->Tick = Tick;
    desc->TickEditor = TickEditor;
    desc->RegisterTypes = RegisterTypes;
    desc->RegisterScriptFuncs = RegisterScriptFuncs;
#if EDITOR
    desc->RegisterEditorUI = RegisterEditorUI;
#else
    desc->RegisterEditorUI = nullptr;
#endif
    desc->OnEditorPreInit = nullptr;
    desc->OnEditorReady = nullptr;
    return 0;
}
} // namespace N64GamePluginDetail

#if EDITOR
extern "C" OCTAVE_PLUGIN_API int PolyphasePlugin_GetDesc(PolyphasePluginDesc* desc)
{
    return N64GamePluginDetail::FillDesc(desc);
}
#else
extern "C" int N64_GAME_PLUGIN_ENTRY(PolyphasePluginDesc* desc)
{
    return N64GamePluginDetail::FillDesc(desc);
}
#endif
