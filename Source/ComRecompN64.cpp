/**
 * @file ComRecompN64.cpp
 * @brief Native addon: com.recomp.n64
 */

#include "Plugins/PolyphasePluginAPI.h"
#include "Plugins/PolyphaseEngineAPI.h"
#include "Plugins/EditorUIHooks.h"
#include "Plugins/PolyphaseBuildTargetAPI.h"

#include "N64Dependencies.h"

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
static EditorUIHooks* sHooks = nullptr;

// Builds the game packages for the platform being packaged unless the profile turned it
// off (Target Options); a failure cancels the build.
static bool OnPreBuild(int32_t platform, void* userData)
{
    (void)userData;
    char value[8] = "";
    char decomp[512] = "";

    if (sHooks != nullptr && sHooks->GetBuildSetting != nullptr)
    {
        if (sHooks->GetBuildSetting(N64Dependencies::kSetupOption, value, sizeof(value)) && value[0] == '0')
        {
            return true;
        }
        sHooks->GetBuildSetting(N64Dependencies::kDecompOption, decomp, sizeof(decomp));
    }
    if (!N64Dependencies::SetupAll(platform, decomp))
    {
        if (sEngineAPI && sEngineAPI->LogError)
        {
            sEngineAPI->LogError("[n64] Setup Dependencies failed, packaging cancelled (see the log)");
        }
        return false;
    }
    return true;
}

static void RegisterEditorUI(EditorUIHooks* hooks, uint64_t hookId)
{
    sHooks = hooks;
    if (hooks->AddTargetOptions != nullptr)
    {
        hooks->AddTargetOptions(hookId, "N64 Recomp",
            [](const PolyphaseBuildContext* ctx, void*) { N64Dependencies::DrawTargetOptions(ctx); }, nullptr);
    }
    else
    {
        // engines without Target Options sections for every target
        hooks->AddMenuItem(hookId, "Developer", "N64/Setup Dependencies (Windows)",
            [](void*) { N64Dependencies::SetupAllAsync(0, nullptr); }, nullptr, nullptr);
    }
    hooks->RegisterOnPreBuild(hookId, OnPreBuild, nullptr);
    N64Dependencies::CheckReady();
}

static void TickEditor(float deltaTime)
{
    (void)deltaTime;
    N64Dependencies::Tick();
}
#endif

static int FillDesc(PolyphasePluginDesc* desc)
{
    desc->apiVersion = OCTAVE_PLUGIN_API_VERSION;
    desc->pluginName = "com.recomp.n64";
    desc->pluginVersion = "1.0.0";
    desc->OnLoad = OnLoad;
    desc->OnUnload = OnUnload;
    desc->Tick = nullptr;
    desc->RegisterTypes = RegisterTypes;
    desc->RegisterScriptFuncs = RegisterScriptFuncs;
#if EDITOR
    desc->RegisterEditorUI = RegisterEditorUI;
    desc->TickEditor = TickEditor;
#else
    desc->RegisterEditorUI = nullptr;
    desc->TickEditor = nullptr;
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
