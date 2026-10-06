/**
 * @file N64ProviderImpl.h
 * @brief The N64 runtime's RecompProvider (Game/N64Provider.h). Included by exactly one .cpp of
 *        a game package, after its N64Game.h (N64_GAME_PACKAGE).
 */
#pragma once

#include "Game/N64Provider.h"
#include "Game/N64GameApi.h"

#include <cmath>

namespace
{
RecompType ToRecompType(int type)
{
    switch (type)
    {
    case PB_U8: return RecompType::U8;
    case PB_S8: return RecompType::S8;
    case PB_U16: return RecompType::U16;
    case PB_S16: return RecompType::S16;
    case PB_U32: return RecompType::U32;
    case PB_STR: return RecompType::Str;
    case PB_F32: return RecompType::F32;
    default: return RecompType::S32;
    }
}

const PortBridgeVar* FindVar(const std::string& name)
{
    const int count = n64_bridge_var_count();
    for (int i = 0; i < count; ++i)
    {
        const PortBridgeVar* v = n64_bridge_var(i);
        if (v != nullptr && name == v->name) return v;
    }
    return nullptr;
}
}

std::string N64Provider::GamePackage() const
{
    return N64_GAME_PACKAGE;
}

N64Provider& N64Provider::Get()
{
    static N64Provider sProvider;
    return sProvider;
}

void N64Provider::SetFrame(int width, int height)
{
    mWidth = width;
    mHeight = height;
}

bool N64Provider::IsLive() const
{
    return n64_is_running() != 0;
}

void N64Provider::Variables(std::vector<RecompVarInfo>& out) const
{
    const int count = n64_bridge_var_count();
    for (int i = 0; i < count; ++i)
    {
        const PortBridgeVar* v = n64_bridge_var(i);
        if (v == nullptr) continue;
        RecompVarInfo info;
        info.name = v->name;
        info.help = v->help ? v->help : "";
        info.type = ToRecompType(v->type);
        info.count = v->count;
        info.writable = v->type != PB_STR;
        out.push_back(info);
    }
}

void N64Provider::Requests(std::vector<RecompRequestInfo>& out) const
{
    const int count = n64_bridge_request_count();
    for (int i = 0; i < count; ++i)
    {
        const PortBridgeRequest* r = n64_bridge_request_info(i);
        if (r != nullptr) out.push_back({r->name, r->help ? r->help : ""});
    }
}

bool N64Provider::Get(const std::string& name, int index, RecompValue& out)
{
    double value = 0;
    char text[256] = "";
    switch (n64_bridge_get(name.c_str(), index, &value, text, sizeof(text)))
    {
    case 1: out = RecompValue::Number(value); return true;
    case 2: out = RecompValue::Text(text); return true;
    default: return false;
    }
}

bool N64Provider::Set(const std::string& name, int index, const RecompValue& value)
{
    const PortBridgeVar* var = FindVar(name);
    if (var == nullptr || value.isText || var->type == PB_STR)
    {
        return false;
    }
    // the bridge's built-in "set": floats travel as 16.16 fixed point
    int args[2];
    args[0] = var->type == PB_F32 ? (int)std::lround(value.number * 65536.0) : (int)std::lround(value.number);
    args[1] = index;
    return n64_bridge_request(("set " + name).c_str(), args, 2) > 0;
}

int N64Provider::Request(const std::string& name, const std::vector<int>& args)
{
    return n64_bridge_request(name.c_str(), args.data(), (int)args.size());
}

bool N64Provider::Result(int id, int& result)
{
    return n64_bridge_result(id, &result) != 0;
}

RecompFrameInfo N64Provider::FrameInfo() const
{
    RecompFrameInfo info;
    info.width = mWidth;
    info.height = mHeight;
    info.displayAspect = 4.0f / 3.0f;
    return info;
}
