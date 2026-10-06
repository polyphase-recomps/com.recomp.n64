# Recomp mode: build a game recompiled from its ROM by N64Recomp against com.recomp.n64.
#
#   include(<com.recomp.n64>/Native/cmake/N64Recomp.cmake)
#   n64port_recomp_game(<name> GENERATED <dir with funcs_*.c, funcs.h, recomp_overlays.inl>)
#
# Makes:
#   <name>_recomp       static library: the recompiled game + the recomp runtime (the embedding
#                       API n64_boot / n64_run_frame / ..., as in a decomp build)
#   <name>_recomp_host  headless runner (host/main.c): --rom --frames --dump --fuzz --wav
#
# The generated C is game code made from your own ROM: it is built locally and never committed.
cmake_minimum_required(VERSION 3.20)

set(N64PORT_DIR "${CMAKE_CURRENT_LIST_DIR}/.." CACHE PATH "com.recomp.n64/Native")
set(N64PORT_INCLUDE_DIR "${N64PORT_DIR}/include")
set(N64RECOMP_DIR "${N64PORT_DIR}/../ThirdParty/N64Recomp" CACHE PATH "vendored N64Recomp")
# Native stack per N64 thread and the runtime's arena: generous on PCs, small where memory is
# (GameCube, Wii: the toolchain files set N64PORT_CONSOLE)
if(N64PORT_CONSOLE)
    set(_n64recomp_stack_kb 256)
    set(_n64recomp_arena_mb 1)
else()
    set(_n64recomp_stack_kb 4096)
    set(_n64recomp_arena_mb 64)
endif()
set(N64RECOMP_THREAD_STACK_KB ${_n64recomp_stack_kb} CACHE STRING "native stack per N64 thread (KB)")
set(N64RECOMP_ARENA_MB ${_n64recomp_arena_mb} CACHE STRING "runtime arena (MB)")
# RDRAM: 8 MB (Expansion Pak); 4 for a game that needs no more, on hosts short on memory
set(N64RECOMP_RDRAM_MB 8 CACHE STRING "RDRAM size (MB)")
if(N64PORT_CONSOLE)
    # the console pieces the decomp build has too: keeping the game's C library names to
    # itself (n64port_isolate_system_symbols) and the standalone runner (host/main_ogc.c)
    include("${CMAKE_CURRENT_LIST_DIR}/N64Port.cmake")
endif()
# The ROM the game was recompiled from: in the editor (port_set_development) n64_boot falls back
# to it when the path it is given is not a whole ROM (a player still pointing at the decomp
# build's asset pack). Packaged games boot the ROM they ship with or ask the player for theirs.
set(N64RECOMP_ROM "" CACHE FILEPATH "ROM n64_boot falls back to in the editor")
# Its cartridge header checksums (16 hex digits, header bytes 0x10-0x17): n64_boot refuses any
# other ROM, which would run the recompiled code on the wrong data.
set(N64RECOMP_ROM_CRC "" CACHE STRING "header checksums of the ROM the game was recompiled from (hex)")

# Live recompilation (N64RECOMP_LIVE, Recomp (live) mode: 64-bit Windows / Linux / macOS):
# the library holds no game code. n64_boot recompiles the player's ROM with N64Recomp's
# LiveRecomp (sljit) from the game's recompiler data (n64_set_recomp_dir: game.json, the toml,
# the symbol files), so GENERATED is not used. <name>_recomp_all is the library plus everything
# it needs (LiveRecomp, N64Recomp, rabbitizer, fmt) merged into one, for the game's addon.
option(N64RECOMP_LIVE "recompile the ROM at boot (LiveRecomp) instead of building N64Recomp's C" OFF)

#   n64port_recomp_game(<name> GENERATED <dir> [GBI_INCLUDES <dirs>] [GBI_DEFS <defines>])
#     The display list interpreter and the audio interpreters (runtime/guest) are compiled with
#     the runtime's own GBI / ABI headers (runtime/recomp/gbi: F3DEX2, written for it; checked
#     against a decomp's by tools/recomp/check_gbi.py), so a game needs no decomp or SDK headers.
#     GBI_INCLUDES: further header folders, searched after those (rarely needed);
#     GBI_DEFS: extra defines for those files.
function(n64port_recomp_game name)
    cmake_parse_arguments(ARG "" "GENERATED" "GBI_INCLUDES;GBI_DEFS" ${ARGN})
    set(rt "${N64PORT_DIR}/runtime/recomp")
    if(N64RECOMP_LIVE)
        if(N64PORT_CONSOLE OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
            message(FATAL_ERROR "N64RECOMP_LIVE: LiveRecomp makes x86-64 / ARM64 code on 64-bit PCs only")
        endif()
        # the recompiler itself (vendored; its tools are not built)
        if(NOT TARGET LiveRecomp)
            add_subdirectory("${N64RECOMP_DIR}" "${CMAKE_BINARY_DIR}/n64recomp" EXCLUDE_FROM_ALL)
        endif()
        set(game_sources "${rt}/recomp_live.cpp" "${N64RECOMP_DIR}/src/config.cpp")
    else()
        if(NOT ARG_GENERATED OR NOT EXISTS "${ARG_GENERATED}/funcs.h")
            message(FATAL_ERROR "n64port_recomp_game: GENERATED must be N64Recomp's output folder (funcs.h missing)")
        endif()
        file(GLOB generated_c CONFIGURE_DEPENDS "${ARG_GENERATED}/funcs_*.c")
        # lookup.cpp (entrypoint, ROM name) is plain C despite its name
        set_source_files_properties("${ARG_GENERATED}/lookup.cpp" PROPERTIES LANGUAGE C)
        set(game_sources ${generated_c} "${ARG_GENERATED}/lookup.cpp" "${rt}/recomp_sections.cpp")
    endif()

    add_library(${name}_recomp STATIC
        ${game_sources}
        "${rt}/recomp_rt.c"
        "${rt}/recomp_os.c"
        "${rt}/recomp_io.c"
        "${N64PORT_DIR}/runtime/host/port_host.c"
        # the platform backends (each compiles only on its platform)
        "${N64PORT_DIR}/runtime/host/port_host_win32.c"
        "${N64PORT_DIR}/runtime/host/port_host_posix.c"
        "${N64PORT_DIR}/runtime/host/port_host_ogc.c")
    # Our sections.h (librecomp/) and recomp.h (include/portable: N64Recomp's own on 64-bit
    # little-endian hosts, a portable version elsewhere); the game's generated folder first.
    target_include_directories(${name}_recomp PUBLIC "${N64PORT_INCLUDE_DIR}")
    target_include_directories(${name}_recomp PRIVATE "${ARG_GENERATED}" "${rt}" "${rt}/include"
        "${rt}/include/portable" "${N64RECOMP_DIR}/include" "${N64PORT_DIR}/runtime/host")
    target_compile_definitions(${name}_recomp PRIVATE
        "RECOMP_THREAD_STACK=(${N64RECOMP_THREAD_STACK_KB}u<<10)" "PORT_ARENA_SIZE=(${N64RECOMP_ARENA_MB}ull<<20)"
        "RECOMP_RDRAM_MB=${N64RECOMP_RDRAM_MB}")
    if(N64RECOMP_ROM)
        set_property(SOURCE "${rt}/recomp_io.c" APPEND PROPERTY COMPILE_DEFINITIONS "RECOMP_DEFAULT_ROM=\"${N64RECOMP_ROM}\"")
    endif()
    if(N64RECOMP_LIVE)
        target_compile_definitions(${name}_recomp PRIVATE RECOMP_LIVE=1)
        target_include_directories(${name}_recomp PRIVATE "${N64RECOMP_DIR}/src")
        target_link_libraries(${name}_recomp PUBLIC LiveRecomp N64Recomp SymbolLists tomlplusplus::tomlplusplus)
        # one library for the addon: the runtime and the recompiler's libraries merged
        set(merged "${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}${name}_recomp_all${CMAKE_STATIC_LIBRARY_SUFFIX}")
        set(parts $<TARGET_FILE:${name}_recomp> $<TARGET_FILE:LiveRecomp> $<TARGET_FILE:N64Recomp>
            $<TARGET_FILE:SymbolLists> $<TARGET_FILE:rabbitizer> $<TARGET_FILE:fmt>)
        if(MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
            get_filename_component(tool_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
            find_program(N64RECOMP_LIB_TOOL NAMES llvm-lib lib HINTS "${tool_dir}" REQUIRED)
            set(merge_command "${N64RECOMP_LIB_TOOL}" /nologo "/out:${merged}" ${parts})
        else()
            # GNU ar: an MRI script adds every member of each part
            set(merge_command ${CMAKE_COMMAND} "-DOUT=${merged}" "-DMRI=${CMAKE_CURRENT_BINARY_DIR}/${name}_recomp_all.mri"
                "-DPARTS=$<JOIN:${parts},\;>" "-DAR=${CMAKE_AR}" -P "${N64PORT_DIR}/cmake/N64RecompMerge.cmake")
        endif()
        add_custom_command(OUTPUT "${merged}" COMMAND ${merge_command}
            DEPENDS ${name}_recomp LiveRecomp N64Recomp SymbolLists rabbitizer fmt
            COMMENT "merging ${name}_recomp_all" VERBATIM)
        add_custom_target(${name}_recomp_all ALL DEPENDS "${merged}")
    endif()
    if(N64RECOMP_ROM_CRC MATCHES "^[0-9A-Fa-f]+$" AND NOT N64RECOMP_LIVE)
        set_property(SOURCE "${rt}/recomp_io.c" APPEND PROPERTY COMPILE_DEFINITIONS "RECOMP_ROM_CRC=0x${N64RECOMP_ROM_CRC}ull")
    endif()
    # recomp_hooks.h: runtime functions the game's [[patches.hook]] text may call
    if(generated_c)
        # -ffp-contract=off: no fused multiply-adds (ARM64 compilers fuse by default), so the game
        # computes the same floats on every machine (netplay between PCs, Macs and consoles)
        set_source_files_properties(${generated_c} PROPERTIES COMPILE_OPTIONS
            "$<$<C_COMPILER_ID:GNU,Clang>:-w;-fno-strict-aliasing;-ffp-contract=off;-include;recomp_hooks.h>$<$<C_COMPILER_ID:MSVC>:/FIrecomp_hooks.h>")
    endif()
    set_target_properties(${name}_recomp PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON C_STANDARD 11)
    if(WIN32)
        target_link_libraries(${name}_recomp PUBLIC winmm dbghelp)
    elseif(NOT N64PORT_CONSOLE)
        target_link_libraries(${name}_recomp PUBLIC pthread m)
    endif()

    # The display list interpreter and the audio microcodes (n_aspMain, aspMain), as native code
    # over the recompiled game's RDRAM: the same host mode the wasm build uses (PORT_RSP_HOST),
    # with RDRAM's layout on this host (recomp_layout.h).
    set(gfx "${N64PORT_DIR}/runtime/guest/port_gfx.c"
        "${N64PORT_DIR}/runtime/guest/port_audio.c"
        "${N64PORT_DIR}/runtime/guest/port_audio_abi1.c")
    target_sources(${name}_recomp PRIVATE ${gfx})
    set(gfx_options -funsigned-char -fwrapv -fno-strict-aliasing -ffp-contract=off -w)
    foreach(d ${ARG_GBI_INCLUDES})
        list(APPEND gfx_options "-idirafter" "${d}")
    endforeach()
    # No PORT: that tells a decomp's patched headers (gbi.h, os.h, ...) to use the native port's
    # layouts (pointer-sized display list words, ...); here the data is the game's own.
    set_source_files_properties(${gfx} PROPERTIES
        COMPILE_DEFINITIONS "${ARG_GBI_DEFS};F3DEX_GBI_2;PORT_RSP_HOST=1;PORT_RSP_RECOMP=1;_SIZE_T_DEF;RECOMP_RDRAM_MB=${N64RECOMP_RDRAM_MB}"
        COMPILE_OPTIONS "${gfx_options}"
        INCLUDE_DIRECTORIES "${rt}/gbi;${rt}/include;${N64PORT_DIR}/runtime/host/include;${N64PORT_DIR}/runtime/guest;${N64PORT_INCLUDE_DIR}")

    if(N64PORT_CONSOLE)
        # the game's names that newlib / libogc also define become n64_<name> inside the library
        n64port_isolate_system_symbols(${name}_recomp)
        # standalone runner (no engine): <name>_recomp_host.elf, see tools/run_dolphin.ps1
        n64port_add_host_exe(${name}_recomp_host ${name}_recomp)
    else()
        # the headless runner
        add_executable(${name}_recomp_host "${N64PORT_DIR}/host/main.c")
        target_link_libraries(${name}_recomp_host PRIVATE ${name}_recomp)
    endif()
endfunction()
