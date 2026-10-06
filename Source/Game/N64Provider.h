/**
 * @file N64Provider.h
 * @brief The N64 runtime's RecompProvider (com.recomp.mod.base): mod settings, the
 *        Recomp/Mods Lua tables, Recomp* widgets and the editor's Mods windows work on the game
 *        through it. Shared by the template's game packages (Game/N64ProviderImpl.h).
 *
 * Variables and requests are the game's script bridge (n64_bridge_*).
 */
#pragma once

#include "ModBaseProvider.h"

class N64Provider : public RecompProvider
{
public:
    static N64Provider& Get();

    void SetFrame(int width, int height);

    const char* RuntimeId() const override { return "n64"; }
    std::string GamePackage() const override;
    bool IsLive() const override;
    void Variables(std::vector<RecompVarInfo>& out) const override;
    void Requests(std::vector<RecompRequestInfo>& out) const override;
    bool Get(const std::string& name, int index, RecompValue& out) override;
    bool Set(const std::string& name, int index, const RecompValue& value) override;
    int Request(const std::string& name, const std::vector<int>& args) override;
    bool Result(int id, int& result) override;
    RecompFrameInfo FrameInfo() const override;

private:
    int mWidth = 0;
    int mHeight = 0;
};
