/**
 * @file ComRecompN64.cpp
 * @brief Native addon: com.recomp.n64
 */

#include "Plugins/PolyphasePluginAPI.h"
#include "Plugins/PolyphaseEngineAPI.h"

static PolyphaseEngineAPI* sEngineAPI = nullptr;

static int OnLoad(PolyphaseEngineAPI* api)
{
    sEngineAPI = api;
    api->LogDebug("com.recomp.n64 loaded!");
    return 0;
}

static void OnUnload()
{
    if (sEngineAPI)
    {
        sEngineAPI->LogDebug("com.recomp.n64 unloaded.");
    }
    sEngineAPI = nullptr;
}

static void RegisterTypes(void* nodeFactory)
{
    // Register custom node types here
    // Example: REGISTER_NODE(MyCustomNode);
}

static void RegisterScriptFuncs(lua_State* L)
{
    // Register Lua functions here
    // Use L to interact with Lua state
    (void)L; // Suppress unused parameter warning
}

#if EDITOR
static void RegisterEditorUI(EditorUIHooks* hooks, uint64_t hookId)
{
    // Register editor UI extensions here
    // Example:
    // hooks->AddMenuItem(hookId, "Developer", "com.recomp.n64 Tool",
    //     [](void*) { /* do something */ }, nullptr, nullptr);
}
#endif

static int FillDesc(PolyphasePluginDesc* desc)
{
    desc->apiVersion = OCTAVE_PLUGIN_API_VERSION;
    desc->pluginName = "com.recomp.n64";
    desc->pluginVersion = "1.0.0";
    desc->OnLoad = OnLoad;
    desc->OnUnload = OnUnload;
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

#if EDITOR
extern "C" OCTAVE_PLUGIN_API int PolyphasePlugin_GetDesc(PolyphasePluginDesc* desc)
{
    return FillDesc(desc);
}
#else
extern "C" int PolyphasePlugin_GetDesc_com_recomp_n64(PolyphasePluginDesc* desc)
{
    return FillDesc(desc);
}
#endif
