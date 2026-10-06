/**
 * @file N64Lua.h
 * @brief Registers the global Lua table `N64` (script bridge to the running game; see
 *        Game/N64LuaImpl.h for the functions).
 */
#pragma once

struct lua_State;
struct PolyphaseEngineAPI;

namespace N64Lua
{
void Register(lua_State* L, PolyphaseEngineAPI* api);
}
