/**
 * @file N64LuaImpl.h
 * @brief Lua access to the running N64 game's script bridge (n64_bridge_*, see
 *        com.recomp.n64/Native/include/port_bridge.h). Same shape as the PS1 runtime's `Ps1`.
 *
 * Global table `N64`:
 *   N64.IsRunning()              true while the game runs
 *   N64.Get(name [, index])      a published variable (number, or string for text);
 *                                nil if no game runs / unknown / out of range
 *   N64.Set(name, value [, i])   queues a write; returns the request id (nil if refused)
 *   N64.Request(name, ...)       queues a game request with integer arguments; id or nil
 *   N64.Result(id)               the request's result once the game ran it, else nil
 *   N64.PollEvent()              the next event the game sent: name, arg1, arg2, ...; nil if none
 *   N64.Variables()              { {name=, type=, count=, help=}, ... }
 *   N64.Requests()               { {name=, help=}, ... }
 *
 * Requests run on the game thread at its next frame, so Result is nil for at least one
 * frame: poll it from a Tick. Events are how the game drives Polyphase, e.g.
 *
 *     function MyHud:Tick(dt)
 *         while true do
 *             local name, a, b = N64.PollEvent()
 *             if name == nil then break end
 *             if name == "game_status" and a == 2 then self:OpenPauseMenu() end
 *         end
 *     end
 *
 * The same calls work on every recomp runtime through the shared `Recomp` table
 * (com.recomp.mod.base). Every Lua call goes through the engine's Lua_* wrappers
 * (PolyphaseEngineAPI): an addon must not run its own copy of the Lua library on the
 * engine's state (luaL_newlib's version check aborts with "multiple Lua VMs detected").
 */

#pragma once

#include "Game/N64Lua.h"

#include "Constants.h"

#if LUA_ENABLED

#include "Game/N64GameApi.h"
#include "Plugins/PolyphaseEngineAPI.h"

#include <string>

namespace N64LuaDetail
{
PolyphaseEngineAPI* sApi = nullptr;

// lua_CFunction / luaL_Reg layout (the table goes to the engine's LuaL_setfuncs)
typedef int (*LuaFunction)(lua_State* L);
struct LuaReg
{
    const char* name;
    LuaFunction func;
};

constexpr int kLuaTNumber = 3;

const char* TypeName(int type)
{
    switch (type)
    {
    case PB_U8: return "u8";
    case PB_S8: return "s8";
    case PB_U16: return "u16";
    case PB_S16: return "s16";
    case PB_U32: return "u32";
    case PB_S32: return "s32";
    case PB_STR: return "string";
    case PB_F32: return "f32";
    default: return "?";
    }
}

int OptInteger(lua_State* L, int arg, int fallback)
{
    if (sApi->Lua_gettop(L) < arg || sApi->Lua_isnil(L, arg))
    {
        return fallback;
    }
    return (int)sApi->LuaL_checkinteger(L, arg);
}

void PushId(lua_State* L, int id)
{
    if (id > 0)
    {
        sApi->Lua_pushinteger(L, id);
    }
    else
    {
        sApi->Lua_pushnil(L);
    }
}

void SetString(lua_State* L, const char* field, const char* value)
{
    sApi->Lua_pushstring(L, value != nullptr ? value : "");
    sApi->Lua_setfield(L, -2, field);
}

int IsRunning(lua_State* L)
{
    sApi->Lua_pushboolean(L, n64_is_running());
    return 1;
}

int Get(lua_State* L)
{
    const char* name = sApi->LuaL_checkstring(L, 1);
    const int index = OptInteger(L, 2, 0);
    double value = 0.0;
    char text[256];

    switch (n64_is_running() ? n64_bridge_get(name, index, &value, text, sizeof(text)) : 0)
    {
    case 1:
        if (value == (double)(long long)value)
        {
            sApi->Lua_pushinteger(L, (long long)value);
        }
        else
        {
            sApi->Lua_pushnumber(L, value);
        }
        break;
    case 2: sApi->Lua_pushstring(L, text); break;
    default: sApi->Lua_pushnil(L); break;
    }
    return 1;
}

int Set(lua_State* L)
{
    const std::string name = std::string("set ") + sApi->LuaL_checkstring(L, 1);
    int args[2];

    // Fractional values are meant for float variables, which take value * 65536.
    const double value = sApi->LuaL_checknumber(L, 2);
    if (sApi->Lua_type(L, 2) == kLuaTNumber && value == (double)(long long)value)
    {
        args[0] = (int)value;
    }
    else
    {
        args[0] = (int)(value * 65536.0);
    }
    args[1] = OptInteger(L, 3, 0);
    PushId(L, n64_is_running() ? n64_bridge_request(name.c_str(), args, 2) : 0);
    return 1;
}

int Request(lua_State* L)
{
    const char* name = sApi->LuaL_checkstring(L, 1);
    int args[PB_MAX_ARGS];
    int nargs = 0;
    const int top = sApi->Lua_gettop(L);

    for (int i = 2; i <= top && nargs < PB_MAX_ARGS; ++i)
    {
        args[nargs++] = (int)sApi->LuaL_checkinteger(L, i);
    }
    PushId(L, n64_is_running() ? n64_bridge_request(name, args, nargs) : 0);
    return 1;
}

int Result(lua_State* L)
{
    const int id = (int)sApi->LuaL_checkinteger(L, 1);
    int result = 0;

    if (n64_bridge_result(id, &result))
    {
        sApi->Lua_pushinteger(L, result);
    }
    else
    {
        sApi->Lua_pushnil(L);
    }
    return 1;
}

int PollEvent(lua_State* L)
{
    char name[64];
    int args[PB_MAX_ARGS];
    int nargs = 0;

    if (!n64_bridge_poll_event(name, sizeof(name), args, PB_MAX_ARGS, &nargs))
    {
        sApi->Lua_pushnil(L);
        return 1;
    }
    sApi->Lua_pushstring(L, name);
    for (int i = 0; i < nargs; ++i)
    {
        sApi->Lua_pushinteger(L, args[i]);
    }
    return 1 + nargs;
}

int Variables(lua_State* L)
{
    const int count = n64_bridge_var_count();

    sApi->Lua_createtable(L, count, 0);
    for (int i = 0; i < count; ++i)
    {
        const PortBridgeVar* var = n64_bridge_var(i);

        sApi->Lua_pushinteger(L, (long long)i + 1);
        sApi->Lua_createtable(L, 0, 4);
        SetString(L, "name", var->name);
        SetString(L, "type", TypeName(var->type));
        sApi->Lua_pushinteger(L, var->count);
        sApi->Lua_setfield(L, -2, "count");
        SetString(L, "help", var->help);
        sApi->Lua_rawset(L, -3);
    }
    return 1;
}

int Requests(lua_State* L)
{
    const int count = n64_bridge_request_count();

    sApi->Lua_createtable(L, count, 0);
    for (int i = 0; i < count; ++i)
    {
        const PortBridgeRequest* request = n64_bridge_request_info(i);

        sApi->Lua_pushinteger(L, (long long)i + 1);
        sApi->Lua_createtable(L, 0, 2);
        SetString(L, "name", request->name);
        SetString(L, "help", request->help);
        sApi->Lua_rawset(L, -3);
    }
    return 1;
}
}

void N64Lua::Register(lua_State* L, PolyphaseEngineAPI* api)
{
    using namespace N64LuaDetail;

    if (L == nullptr || api == nullptr || api->Lua_createtable == nullptr || api->LuaL_setfuncs == nullptr ||
        api->Lua_setglobal == nullptr)
    {
        return;
    }
    sApi = api;
    static const LuaReg kFuncs[] = {
        {"IsRunning", IsRunning}, {"Get", Get},             {"Set", Set},
        {"Request", Request},     {"Result", Result},       {"PollEvent", PollEvent},
        {"Variables", Variables}, {"Requests", Requests},   {nullptr, nullptr},
    };
    sApi->Lua_createtable(L, 0, 8);
    sApi->LuaL_setfuncs(L, kFuncs, 0);
    sApi->Lua_setglobal(L, "N64");
}

#else

void N64Lua::Register(lua_State*, PolyphaseEngineAPI*)
{
}

#endif
