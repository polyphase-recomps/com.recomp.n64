# {{TITLE}} in recomp mode: the game recompiled from your ROM by N64Recomp (recomp.{{REGION}}.toml +
# {{ID}}.{{REGION}}.syms.toml), on com.recomp.n64's recomp runtime. No decomp needed.
#
# Built by com.recomp.n64 (Tools > Recomp > N64 > Set Up Game, or Setup Dependencies), which
# generates the C, builds this and publishes the library as Lib/{{ID}}.lib.
cmake_minimum_required(VERSION 3.20)
project({{ID}}_recomp C CXX)

set(N64PORT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../com.recomp.n64/Native" CACHE PATH
    "Path to the shared N64 runtime (com.recomp.n64/Native)")
set(N64RECOMP_GENERATED "${CMAKE_CURRENT_SOURCE_DIR}/../Native/build/recomp/funcs" CACHE PATH
    "N64Recomp's output for the game (funcs_*.c, funcs.h, recomp_overlays.inl)")

include("${N64PORT_ROOT}/cmake/N64Recomp.cmake")
n64port_recomp_game({{ID}} GENERATED "${N64RECOMP_GENERATED}")
