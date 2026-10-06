# Third-party code in com.recomp.n64

## N64Recomp (`ThirdParty/N64Recomp`)

A trimmed snapshot of [N64Recomp](https://github.com/N64Recomp/N64Recomp) and the libraries it
builds with. The exact commits are in `N64Recomp/VERSION.txt`, and `update_n64recomp.ps1` refreshes it.
Only the sources its CMake uses are kept (no tests, docs, bindings or examples).

| Component | Path | License | Notice file |
|---|---|---|---|
| N64Recomp | `N64Recomp/` | MIT | `N64Recomp/LICENSE` |
| rabbitizer (MIPS disassembler) | `N64Recomp/lib/rabbitizer` | MIT | `lib/rabbitizer/LICENSE` |
| fmt | `N64Recomp/lib/fmt` | MIT, with an optional exception for binaries | `lib/fmt/LICENSE` |
| toml++ | `N64Recomp/lib/tomlplusplus` | MIT | `lib/tomlplusplus/LICENSE` |
| ELFIO | `N64Recomp/lib/ELFIO` | MIT | `lib/ELFIO/LICENSE.txt` |
| sljit | `N64Recomp/lib/sljit` | BSD-2-Clause | `lib/sljit/LICENSE` |

**What uses what:**
- **Editor / developer tooling:** the N64Recomp library and CLI tools, to recompile a game from the
  developer's own ROM.
- **PC runtime (live mode):** LiveRecomp and sljit, linked into the game.
- **Console builds:** link none of it.

**Binary distributions** that include any of these (the editor addon, PC games in live mode) must
reproduce the notices above; sljit's BSD-2 license requires it. Ship this file plus the listed
license files as the game's third-party notices.

### Local changes to N64Recomp

`n64recomp-local.patch` (re-applied by `update_n64recomp.ps1`) changes LiveRecomp, for the
recomp runtime's live mode (`Native/runtime/recomp/recomp_live.cpp`):
- `LiveGeneratorInputs::external_functions`: calls to the given function indices go to host
  functions (the runtime's libultra, `<name>_recomp`), as the C output calls them by name.
- `LiveGeneratorInputs::loop_budget` / `loop_preempt`: every jump back to an earlier label
  decrements the budget and calls the preempt hook when it runs out. These are the runtime's
  preemption points, which `tools/recomp/add_loop_checks.py` adds to the C output.

Both are additive: without them set, LiveRecomp behaves as upstream. The patch is MIT like the
code it changes.

## Not used

N64ModernRuntime (librecomp / ultramodern) is GPL-3.0 and is **not** part of this package, nor is any of
its code. The recomp runtime here is our own and implements N64Recomp's generated-code contract
independently. RT64 (MIT) is not used either: rendering is `runtime/guest/port_gfx.c`.
