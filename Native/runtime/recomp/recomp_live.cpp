/*
 * Recomp (live) mode: the game's code is recompiled from the player's ROM when it boots, by
 * N64Recomp's LiveRecomp (sljit: MIPS straight to x86-64 / ARM64, no C compiler), instead of
 * being compiled into the build from N64Recomp's C output. A release then ships the runtime and
 * the game's recompiler data (game.json, the N64Recomp config and symbol files) and no game code.
 *
 * This file stands in for what the C output brings in the other mode: the function / section
 * table (recomp_sections.cpp + recomp_overlays.inl) and the entry point (lookup.cpp). It builds
 * the recompiler's context as the N64Recomp tool does for a symbol file (src/main.cpp), so both
 * modes run the same functions:
 *   - libultra's hardware functions (N64Recomp's ignored / reimplemented lists, the config's
 *     [patches] ignored) are the runtime's <name>_recomp (recomp_ultra.h);
 *   - functions only calls reveal (statics) are added and recompiled until none are left;
 *   - every loop back edge gets the runtime's preemption check (tools/recomp/add_loop_checks.py
 *     in the other mode).
 * The ROM must be the one game.json names (sha1): the symbols describe that ROM's code.
 */
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "config.h"
#include "recompiler/context.h"
#include "recompiler/live_recompiler.h"

#include "librecomp/sections.h"
#include "recomp_rt.h"

extern "C" {
#include "port_host.h"
}

extern "C" int32_t *section_addresses;
extern "C" int32_t gRecompLoopBudget;
extern "C" void recomp_loop_preempt(uint8_t *rdram, recomp_context *ctx, uint32_t loop_vram);
extern "C" void switch_error(const char *func, uint32_t vram, uint32_t jtbl);
extern "C" void do_break(uint32_t vram);
extern "C" void recomp_syscall_handler(uint8_t *rdram, recomp_context *ctx, int32_t instruction_vram);
extern "C" gpr cop0_status_read(recomp_context *ctx);
extern "C" void cop0_status_write(recomp_context *ctx, gpr value);
extern "C" void pause_self(uint8_t *rdram);

namespace
{
/* ---- what the runtime implements, by name ---------------------------------------------- */
const std::unordered_map<std::string, recomp_func_t *> &runtime_functions()
{
#define RECOMP_LIVE_ENTRY(name) {#name, name##_recomp},
    static const std::unordered_map<std::string, recomp_func_t *> table = {RECOMP_ULTRA_FUNCS(RECOMP_LIVE_ENTRY)};
#undef RECOMP_LIVE_ENTRY
    return table;
}

/* A libultra function the game calls that the runtime does not implement: stops with its name. */
std::vector<std::string> sMissingNames;

void missing_function(uint8_t *, recomp_context *, uintptr_t index)
{
    recomp_fatal("the game called %s, which this runtime does not implement",
                 index < sMissingNames.size() ? sMissingNames[index].c_str() : "?");
}

/* ---- SHA-1 (game.json names the ROM by it) ---------------------------------------------- */
std::string sha1_hex(const std::vector<uint8_t> &data)
{
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    auto rol = [](uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
    auto block = [&](const uint8_t *p) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
        {
            w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
        }
        for (int i = 16; i < 80; i++)
        {
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++)
        {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    };
    size_t full = data.size() / 64 * 64;
    for (size_t i = 0; i < full; i += 64)
    {
        block(data.data() + i);
    }
    uint8_t tail[128] = {0};
    size_t rest = data.size() - full;
    memcpy(tail, data.data() + full, rest);
    tail[rest] = 0x80;
    size_t tail_size = (rest + 9 <= 64) ? 64 : 128;
    uint64_t bits = (uint64_t)data.size() * 8;
    for (int i = 0; i < 8; i++)
    {
        tail[tail_size - 1 - i] = (uint8_t)(bits >> (i * 8));
    }
    block(tail);
    if (tail_size == 128)
    {
        block(tail + 64);
    }
    char out[41];
    for (int i = 0; i < 5; i++)
    {
        snprintf(out + i * 8, 9, "%08x", h[i]);
    }
    return out;
}

/* ---- game.json: the two strings needed here ("config", and "sha1" under "rom") ---------- */
std::string json_string(const std::string &json, const char *key)
{
    std::string quoted = std::string("\"") + key + "\"";
    size_t at = json.find(quoted);
    if (at == std::string::npos) return "";
    at = json.find(':', at + quoted.size());
    if (at == std::string::npos) return "";
    at = json.find('"', at);
    if (at == std::string::npos) return "";
    size_t end = json.find('"', at + 1);
    return end == std::string::npos ? "" : json.substr(at + 1, end - at - 1);
}

/* ---- the live code ---------------------------------------------------------------------- */
struct LiveGame
{
    std::string rom_sha1;
    uint32_t entrypoint = 0;
    N64Recomp::LiveGeneratorOutput output;
    std::vector<std::unique_ptr<N64Recomp::ShimFunction>> shims;
    std::vector<std::vector<FuncEntry>> funcs;
    std::vector<SectionTableEntry> sections;
    std::vector<int32_t> section_addresses;
};
std::unique_ptr<LiveGame> sGame;

std::filesystem::path utf8_path(const std::string &text)
{
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::string path_utf8(const std::filesystem::path &path)
{
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

bool fail(const char *message, const std::string &detail = "")
{
    port_log("recomp (live): %s%s%s", message, detail.empty() ? "" : ": ", detail.c_str());
    return false;
}

bool find_function(const N64Recomp::Context &context, const std::string &name, size_t &index)
{
    auto it = context.functions_by_name.find(name);
    if (it == context.functions_by_name.end()) return false;
    index = it->second;
    return true;
}

/* N64Recomp's context for the ROM, set up as the tool sets it up for a symbol file */
bool build_context(const N64Recomp::Config &config, std::vector<uint8_t> rom, N64Recomp::Context &context)
{
    if (config.symbols_file_path.empty()) return fail("the config names no symbols_file_path");
    if (!config.has_entrypoint) return fail("the config names no entrypoint");
    if (!N64Recomp::Context::from_symbol_file(config.symbols_file_path, std::move(rom), context, true))
    {
        return fail("could not load the symbol file", config.symbols_file_path.string());
    }
    auto rename = [&context](size_t index, const std::string &name) {
        N64Recomp::Function &func = context.functions[index];
        context.functions_by_name.erase(func.name);
        func.name = name;
        context.functions_by_name[func.name] = index;
    };
    for (size_t i = 0; i < context.functions.size(); i++)
    {
        N64Recomp::Function &func = context.functions[i];
        if (N64Recomp::reimplemented_funcs.contains(func.name))
        {
            rename(i, func.name + "_recomp");
            func.reimplemented = true;
            func.ignored = true;
        }
        else if (N64Recomp::ignored_funcs.contains(func.name))
        {
            rename(i, func.name + "_recomp");
            func.ignored = true;
        }
        else if (N64Recomp::renamed_funcs.contains(func.name))
        {
            rename(i, func.name + "_recomp");
            func.ignored = false;
        }
    }
    bool found_entrypoint = false;
    for (uint32_t index : context.functions_by_vram[config.entrypoint])
    {
        if (context.functions[index].rom == 0x1000)
        {
            rename(index, "recomp_entrypoint");
            found_entrypoint = true;
            break;
        }
    }
    if (!found_entrypoint) return fail("the symbol file has no function at the entrypoint");

    size_t index;
    for (const std::string &name : config.stubbed_funcs)
    {
        if (!find_function(context, name, index)) return fail("stubbed function not found", name);
        context.functions[index].stubbed = true;
    }
    for (const std::string &name : config.ignored_funcs)
    {
        if (!find_function(context, name, index)) return fail("ignored function not found", name);
        context.functions[index].ignored = true;
    }
    for (const std::string &name : config.renamed_funcs)
    {
        if (!find_function(context, name, index)) return fail("renamed function not found", name);
        context.functions[index].name += "_recomp";
    }
    for (const N64Recomp::InstructionPatch &patch : config.instruction_patches)
    {
        if (!find_function(context, patch.func_name, index)) return fail("patched function not found", patch.func_name);
        N64Recomp::Function &func = context.functions[index];
        if (patch.vram < (int32_t)func.vram || patch.vram >= (int32_t)(func.vram + func.words.size() * 4))
        {
            return fail("instruction patch outside its function", patch.func_name);
        }
        uint32_t value = (uint32_t)patch.value;
        func.words[(patch.vram - (int32_t)func.vram) / 4] =
            (value >> 24) | ((value >> 8) & 0xFF00u) | ((value << 8) & 0xFF0000u) | (value << 24);
    }
    if (!config.function_hooks.empty())
    {
        /* [[patches.hook]] entries are C text, for the C output: there is no compiler here */
        return fail("[[patches.hook]] needs the precompiled Recomp mode (it is C source)", config.function_hooks[0].func_name);
    }
    return true;
}

/* Recompiles every function; statics a pass discovers are added and the whole set compiled
 * again, until a pass finds none (a static's code runs from its address up to the next known
 * function, as the tool cuts them). */
bool compile(N64Recomp::Context &context, const N64Recomp::LiveGeneratorInputs &base_inputs, N64Recomp::LiveGeneratorOutput &output,
             std::vector<std::unique_ptr<N64Recomp::ShimFunction>> &shims)
{
    const auto &runtime = runtime_functions();
    for (int pass = 0; pass < 16; pass++)
    {
        N64Recomp::LiveGeneratorInputs inputs = base_inputs;
        for (size_t i = 0; i < context.functions.size(); i++)
        {
            const N64Recomp::Function &func = context.functions[i];
            if (!func.ignored)
            {
                continue;
            }
            std::string name = func.name;
            if (name.size() > 7 && name.compare(name.size() - 7, 7, "_recomp") == 0)
            {
                name.resize(name.size() - 7);
            }
            auto it = runtime.find(name);
            if (it != runtime.end())
            {
                inputs.external_functions[i] = it->second;
            }
            else
            {
                sMissingNames.push_back(name);
                shims.push_back(std::make_unique<N64Recomp::ShimFunction>(missing_function, sMissingNames.size() - 1));
                inputs.external_functions[i] = shims.back()->get_func();
            }
        }

        N64Recomp::LiveGenerator generator{context.functions.size(), inputs};
        std::vector<std::vector<uint32_t>> statics(context.sections.size());
        for (size_t i = 0; i < context.functions.size(); i++)
        {
            const N64Recomp::Function &func = context.functions[i];
            if (func.ignored || func.words.empty())
            {
                continue;
            }
            std::ostringstream unused;
            if (!N64Recomp::recompile_function_live(generator, context, i, unused, statics, false))
            {
                char where[16];
                snprintf(where, sizeof(where), "0x%08X", func.vram);
                return fail("could not recompile", func.name + " (" + where + ")");
            }
        }

        size_t found = 0;
        for (size_t s = 0; s < statics.size(); s++)
        {
            N64Recomp::Section &section = context.sections[s];
            std::set<uint32_t> fresh;
            for (uint32_t vram : statics[s])
            {
                if (context.find_function_by_vram_section(vram, s) == (size_t)-1)
                {
                    fresh.insert(vram);
                }
            }
            for (uint32_t vram : fresh)
            {
                uint32_t end = section.ram_addr + section.size;
                for (uint32_t a : section.function_addrs)
                {
                    if (a > vram && a < end) end = a;
                }
                for (uint32_t a : fresh)
                {
                    if (a > vram && a < end) end = a;
                }
                uint32_t rom_addr = vram - section.ram_addr + section.rom_addr;
                const uint32_t *words = reinterpret_cast<const uint32_t *>(context.rom.data() + rom_addr);
                char name[64];
                snprintf(name, sizeof(name), "static_%zu_%08X", s, vram);
                size_t index = context.functions.size();
                context.functions.emplace_back(vram, rom_addr, std::vector<uint32_t>(words, words + (end - vram) / 4), name,
                                               (uint16_t)s, false);
                context.functions_by_vram[vram].push_back(index);
                context.functions_by_name[name] = index;
                context.section_functions[s].push_back(index);
                section.function_addrs.push_back(vram);
                found++;
            }
        }
        if (found == 0)
        {
            output = generator.finish();
            return output.good ? true : fail("sljit could not finish the code");
        }
        shims.clear();
        sMissingNames.clear();
    }
    return fail("statics kept appearing after 16 passes");
}

/* The section table the runtime looks functions up in (as recomp_overlays.inl has it) */
void build_sections(const N64Recomp::Context &context, LiveGame &game)
{
    const auto &runtime = runtime_functions();
    game.funcs.resize(context.sections.size());
    for (size_t s = 0; s < context.sections.size(); s++)
    {
        const N64Recomp::Section &section = context.sections[s];
        const auto &indices = context.section_functions[s];
        if (indices.empty() && !section.has_mips32_relocs)
        {
            continue;
        }
        std::vector<FuncEntry> &entries = game.funcs[s];
        for (size_t index : indices)
        {
            const N64Recomp::Function &func = context.functions[index];
            recomp_func_t *code = nullptr;
            if (func.reimplemented)
            {
                std::string name = func.name.substr(0, func.name.size() - 7); /* <name>_recomp */
                auto it = runtime.find(name);
                code = (it != runtime.end()) ? it->second : nullptr;
            }
            else if (!func.ignored && !func.words.empty())
            {
                code = game.output.functions[index];
            }
            if (code != nullptr)
            {
                entries.push_back({code, func.rom - section.rom_addr, func.reimplemented ? 0u : (uint32_t)(func.words.size() * 4)});
            }
        }
        /* the runtime looks a call's address up by binary search */
        std::stable_sort(entries.begin(), entries.end(), [](const FuncEntry &a, const FuncEntry &b) { return a.offset < b.offset; });
        SectionTableEntry entry{};
        entry.rom_addr = section.rom_addr;
        entry.ram_addr = section.ram_addr;
        entry.size = section.size;
        entry.funcs = entries.data();
        entry.num_funcs = entries.size();
        entry.index = s;
        game.sections.push_back(entry);
    }
}
} // namespace

/* n64_boot, once the ROM is loaded: recompiles it (once per ROM; a reset reuses the code) */
extern "C" int recomp_live_load(void)
{
    const auto start = std::chrono::steady_clock::now();
    const std::string dir = port_recomp_dir();
    if (dir.empty())
    {
        return fail("no recompiler data: the player must call n64_set_recomp_dir() before n64_boot()");
    }
    const std::filesystem::path base = utf8_path(dir);
    std::string json;
    {
        std::ifstream in(base / "game.json", std::ios::binary);
        if (!in) return fail("cannot read", path_utf8(base / "game.json"));
        std::stringstream text;
        text << in.rdbuf();
        json = text.str();
    }
    const std::string config_name = json_string(json, "config");
    const std::string want_sha1 = json_string(json, "sha1");
    if (config_name.empty()) return fail("game.json names no \"config\"");

    std::vector<uint8_t> rom(port_rom_size());
    port_rom_read(0, rom.data(), (unsigned int)rom.size());
    const std::string sha1 = sha1_hex(rom);
    if (!want_sha1.empty() && sha1 != want_sha1)
    {
        port_log("recomp (live): this ROM (sha1 %s) is not the one the game's symbols describe (%s)", sha1.c_str(), want_sha1.c_str());
        return 0;
    }
    if (sGame && sGame->rom_sha1 == sha1)
    {
        return 1;
    }
    sGame.reset();

    const std::string config_path = (base / utf8_path(config_name)).string();
    N64Recomp::Config config(config_path.c_str());
    if (!config.good()) return fail("bad config", config_path);
    N64Recomp::Context context{};
    if (!build_context(config, std::move(rom), context))
    {
        return 0;
    }

    auto game = std::make_unique<LiveGame>();
    game->rom_sha1 = sha1;
    game->entrypoint = config.entrypoint;
    for (const N64Recomp::Section &section : context.sections)
    {
        game->section_addresses.push_back((int32_t)section.ram_addr);
    }
    N64Recomp::live_recompiler_init();
    N64Recomp::LiveGeneratorInputs inputs{};
    inputs.cop0_status_write = cop0_status_write;
    inputs.cop0_status_read = cop0_status_read;
    inputs.switch_error = switch_error;
    inputs.do_break = do_break;
    inputs.get_function = get_function;
    inputs.syscall_handler = recomp_syscall_handler;
    inputs.pause_self = pause_self;
    /* relocations resolve against the runtime's table (recomp_rt.c fills it from the sections) */
    inputs.local_section_addresses = section_addresses;
    inputs.loop_budget = &gRecompLoopBudget;
    inputs.loop_preempt = recomp_loop_preempt;
    if (!compile(context, inputs, game->output, game->shims))
    {
        return 0;
    }
    build_sections(context, *game);
    const long long ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    port_log("recomp (live): recompiled %zu functions from the ROM (%zu KB of code) in %lld ms", context.functions.size(),
             game->output.code_size >> 10, ms);
    sGame = std::move(game);
    return 1;
}

extern "C" const void *recomp_section_table(size_t *count)
{
    *count = sGame ? sGame->sections.size() : 0;
    return sGame ? sGame->sections.data() : nullptr;
}

extern "C" gpr get_entrypoint_address(void)
{
    return (gpr)(int32_t)(sGame ? sGame->entrypoint : 0x80000400u);
}
