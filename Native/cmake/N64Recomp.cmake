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
set(N64RECOMP_THREAD_STACK_MB 4 CACHE STRING "native stack per N64 thread (MB)")
# The ROM the game was recompiled from: n64_boot falls back to it when the path it is given is
# not a whole ROM (a player still pointing at the decomp build's asset pack). Development only:
# a packaged game asks the player for their ROM.
set(N64RECOMP_ROM "" CACHE FILEPATH "ROM n64_boot falls back to (development builds)")

#   n64port_recomp_game(<name> GENERATED <dir> GBI_INCLUDES <dirs> GBI_DEFS <defines>)
#     GBI_INCLUDES / GBI_DEFS: what the display list interpreter (runtime/guest/port_gfx.c) is
#     compiled with: the game's GBI headers (PR/gbi.h, a port_types.h) and its microcode
#     defines (F3DEX_GBI_2, ...).
function(n64port_recomp_game name)
    cmake_parse_arguments(ARG "" "GENERATED" "GBI_INCLUDES;GBI_DEFS" ${ARGN})
    if(NOT ARG_GENERATED OR NOT EXISTS "${ARG_GENERATED}/funcs.h")
        message(FATAL_ERROR "n64port_recomp_game: GENERATED must be N64Recomp's output folder (funcs.h missing)")
    endif()
    file(GLOB generated_c CONFIGURE_DEPENDS "${ARG_GENERATED}/funcs_*.c")
    set(rt "${N64PORT_DIR}/runtime/recomp")

    # lookup.cpp (entrypoint, ROM name) is plain C despite its name
    set_source_files_properties("${ARG_GENERATED}/lookup.cpp" PROPERTIES LANGUAGE C)
    add_library(${name}_recomp STATIC
        ${generated_c}
        "${ARG_GENERATED}/lookup.cpp"
        "${rt}/recomp_sections.cpp"
        "${rt}/recomp_rt.c"
        "${rt}/recomp_os.c"
        "${rt}/recomp_io.c"
        "${N64PORT_DIR}/runtime/host/port_host.c")
    if(WIN32)
        target_sources(${name}_recomp PRIVATE "${N64PORT_DIR}/runtime/host/port_host_win32.c")
    else()
        target_sources(${name}_recomp PRIVATE "${N64PORT_DIR}/runtime/host/port_host_posix.c")
    endif()
    # Our sections.h (librecomp/) and N64Recomp's recomp.h; the game's generated folder first.
    target_include_directories(${name}_recomp PUBLIC "${N64PORT_INCLUDE_DIR}")
    target_include_directories(${name}_recomp PRIVATE "${ARG_GENERATED}" "${rt}" "${rt}/include"
        "${N64RECOMP_DIR}/include" "${N64PORT_DIR}/runtime/host")
    target_compile_definitions(${name}_recomp PRIVATE
        "RECOMP_THREAD_STACK=(${N64RECOMP_THREAD_STACK_MB}u<<20)" "PORT_ARENA_SIZE=(64ull<<20)")
    if(N64RECOMP_ROM)
        set_property(SOURCE "${rt}/recomp_io.c" APPEND PROPERTY COMPILE_DEFINITIONS "RECOMP_DEFAULT_ROM=\"${N64RECOMP_ROM}\"")
    endif()
    # recomp_hooks.h: runtime functions the game's [[patches.hook]] text may call
    set_source_files_properties(${generated_c} PROPERTIES COMPILE_OPTIONS
        "$<$<C_COMPILER_ID:GNU,Clang>:-w;-fno-strict-aliasing;-include;recomp_hooks.h>$<$<C_COMPILER_ID:MSVC>:/FIrecomp_hooks.h>")
    set_target_properties(${name}_recomp PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON C_STANDARD 11)
    if(WIN32)
        target_link_libraries(${name}_recomp PUBLIC winmm dbghelp)
    else()
        target_link_libraries(${name}_recomp PUBLIC pthread m)
    endif()

    # The display list interpreter and the audio microcodes (n_aspMain, aspMain), as native code
    # over the recompiled game's RDRAM: the same host mode the wasm build uses (PORT_RSP_HOST),
    # with RDRAM's word-swapped layout.
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
        COMPILE_DEFINITIONS "${ARG_GBI_DEFS};PORT_RSP_HOST=1;PORT_RSP_RECOMP=1;_SIZE_T_DEF"
        COMPILE_OPTIONS "${gfx_options}"
        INCLUDE_DIRECTORIES "${N64PORT_DIR}/runtime/host/include;${N64PORT_DIR}/runtime/guest;${N64PORT_INCLUDE_DIR}")

    add_executable(${name}_recomp_host "${N64PORT_DIR}/host/main.c")
    target_link_libraries(${name}_recomp_host PRIVATE ${name}_recomp)
endfunction()
