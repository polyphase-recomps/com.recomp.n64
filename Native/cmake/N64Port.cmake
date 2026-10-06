# Shared build logic for games ported on the native N64 runtime (com.recomp.n64).
#
# A game's CMakeLists.txt includes this file, then:
#   n64port_decomp_objects(<name> <sources...>)   decomp / guest-side objects (freestanding, C89)
#   n64port_host_objects(<name> <sources...>)     hosted C objects (runtime backends, generated tables)
#   n64port_overlay_sections(<targets...>)        per-overlay data sections for those targets
#   n64port_add_host_exe(<name> <library>)        the headless runner linked against the game
#
# Inputs the game sets before calling n64port_decomp_objects():
#   N64PORT_DECOMP_DEFS       compile definitions for decomp code
#   N64PORT_DECOMP_INCLUDES   include directories for decomp code
#   N64PORT_FORCE_INCLUDES    headers force-included after port_prelude.h
#
# Supported compilers: clang targeting Windows (GNU driver), and GCC / clang targeting ELF
# (Linux, Android, console toolchains).

get_filename_component(N64PORT_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(N64PORT_INCLUDE_DIR "${N64PORT_DIR}/include")

find_package(Python3 REQUIRED COMPONENTS Interpreter)

if(WIN32)
    set(N64PORT_COFF TRUE)
else()
    set(N64PORT_COFF FALSE)
endif()

# ---------------------------------------------------------------------------
# Runtime sources
# ---------------------------------------------------------------------------
option(N64PORT_AUDIO "Build the audio microcode / output layer (port_audio.c)" ON)

file(GLOB N64PORT_GUEST_SOURCES CONFIGURE_DEPENDS "${N64PORT_DIR}/runtime/guest/*.c")
if(NOT N64PORT_AUDIO)
    list(REMOVE_ITEM N64PORT_GUEST_SOURCES "${N64PORT_DIR}/runtime/guest/port_audio.c"
        "${N64PORT_DIR}/runtime/guest/port_audio_abi1.c")
endif()
# Every backend is compiled everywhere; each one is wrapped in its own platform guard.
file(GLOB N64PORT_HOST_SOURCES CONFIGURE_DEPENDS "${N64PORT_DIR}/runtime/host/*.c")
if(N64PORT_CONSOLE STREQUAL "3DS")
    # The citro3d backend's vertex shader, assembled with picasso and embedded as C data.
    set(_shbin "${CMAKE_BINARY_DIR}/gen/port_gpu_c3d.shbin")
    set(_shbin_c "${CMAKE_BINARY_DIR}/gen/port_gpu_c3d_shbin.c")
    add_custom_command(OUTPUT "${_shbin_c}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/gen"
        COMMAND "${N64PORT_PICASSO}" -o "${_shbin}" "${N64PORT_DIR}/runtime/host/port_gpu_c3d.v.pica"
        COMMAND "${Python3_EXECUTABLE}" "${N64PORT_DIR}/tools/bin2c.py" "${_shbin}" "${_shbin_c}" n64port_c3d_shbin
        DEPENDS "${N64PORT_DIR}/runtime/host/port_gpu_c3d.v.pica" "${N64PORT_DIR}/tools/bin2c.py"
        VERBATIM)
    list(APPEND N64PORT_HOST_SOURCES "${_shbin_c}")
endif()

# The renderer and the audio mixer are inner loops; optimise them whatever the game code uses.
set_source_files_properties("${N64PORT_DIR}/runtime/guest/port_gfx.c" "${N64PORT_DIR}/runtime/guest/port_audio.c"
    "${N64PORT_DIR}/runtime/guest/port_audio_abi1.c" PROPERTIES COMPILE_OPTIONS "-O2")
# Debug aid (GCC / clang on ELF): report undefined behaviour in the renderer and the mixer.
option(N64PORT_SANITIZE "Build port_gfx.c / port_audio.c with the undefined behaviour sanitizer" OFF)
if(N64PORT_SANITIZE)
    set_source_files_properties("${N64PORT_DIR}/runtime/guest/port_gfx.c" "${N64PORT_DIR}/runtime/guest/port_audio.c"
        PROPERTIES COMPILE_OPTIONS "-O2;-fsanitize=undefined;-fsanitize=float-cast-overflow")
endif()

# ---------------------------------------------------------------------------
# Flags
# ---------------------------------------------------------------------------
set(N64PORT_DECOMP_FLAGS -ffreestanding -funsigned-char -fno-strict-aliasing -fwrapv
    -ffp-contract=off -fno-builtin -fno-common -g -fno-omit-frame-pointer)
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    list(APPEND N64PORT_DECOMP_FLAGS -nostdlibinc -Wno-everything)
else()
    # GCC 14+ turned several C89-isms into errors; the decomp relies on them.
    # Game data relies on consecutive declarations being adjacent in memory (a display list loads
    # vertices across two arrays, for example); GCC reorders top-level variables when optimising.
    # -fno-data-sections: console toolchains (devkitPPC) give every variable its own
    # .data.<name> / .bss.<name> section by default, which both breaks that adjacency and hides
    # the variables from the overlay section renaming (tools/cc_overlay.py matches .data / .bss).
    list(APPEND N64PORT_DECOMP_FLAGS -w -fno-toplevel-reorder -fno-data-sections)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i.86")
        # ...and on x86 GCC pads large objects to 32 / 64 bytes unless told to stick to the ABI.
        list(APPEND N64PORT_DECOMP_FLAGS -malign-data=abi)
    endif()
    set(N64PORT_DECOMP_C_FLAGS -fpermissive)
    set(N64PORT_DECOMP_CXX_FLAGS -fpermissive)
endif()
if(N64PORT_COFF)
    list(APPEND N64PORT_DECOMP_FLAGS -gcodeview -funwind-tables -fasynchronous-unwind-tables)
elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
    # 64-bit ELF: position independent, so the image loads above 4 GB. The renderer tells native
    # pointers from N64 segmented addresses by that (see gfx_is_segmented in port_gfx.c).
    list(APPEND N64PORT_DECOMP_FLAGS -fPIC)
    set(N64PORT_PIC TRUE)
endif()

# Memory budget. The arena backs everything the port allocates (including the scene heap handed
# to the game); 64-bit hosts are generous because structures holding pointers grow, 32-bit
# hosts are close to the N64's own numbers.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_n64port_arena_mb 256)
    set(_n64port_scene_mb 48)
else()
    set(_n64port_arena_mb 32)
    set(_n64port_scene_mb 12)
endif()
set(N64PORT_ARENA_MB ${_n64port_arena_mb} CACHE STRING "Size of the port's memory arena in MB")
set(N64PORT_SCENE_ARENA_MB ${_n64port_scene_mb} CACHE STRING "Size of the game's scene heap in MB (part of the arena)")
set(N64PORT_THREAD_STACK_KB 512 CACHE STRING "Host stack per N64 thread in KB")

if(N64PORT_CONSOLE MATCHES "Wii|GameCube")
    # Overlay variables are moved to their own sections (cc_overlay.py), which the small data
    # area's register-relative addressing cannot reach.
    list(APPEND N64PORT_DECOMP_FLAGS -msdata=none)
    # The real CPUs (Gekko / Broadway) raise an alignment exception on a floating-point load
    # or store at an address that is not a multiple of 4; emulators do not. Without this GCC
    # turns a byte-wise float read from a byte stream (lbParticleReadFloatBigEnd) into `lfs`.
    list(APPEND N64PORT_DECOMP_FLAGS -mstrict-align)
endif()
if(N64PORT_CONSOLE STREQUAL "3DS")
    # Only aligned accesses (the ARM11 faults on unaligned multi-word and VFP loads).
    list(APPEND N64PORT_DECOMP_FLAGS -mno-unaligned-access)
endif()

function(n64port_decomp_objects name)
    add_library(${name} OBJECT ${ARGN})
    target_compile_definitions(${name} PRIVATE PORT=1 ${N64PORT_DECOMP_DEFS}
        "PORT_SCENE_ARENA_SIZE=(${N64PORT_SCENE_ARENA_MB}*1024*1024)"
        "PORT_THREAD_STACK=(${N64PORT_THREAD_STACK_KB}*1024)")
    if(N64PORT_GFX STREQUAL "gpu")
        target_compile_definitions(${name} PRIVATE PORT_GFX_GPU=1)
    endif()
    target_include_directories(${name} PRIVATE "${N64PORT_INCLUDE_DIR}" ${N64PORT_DECOMP_INCLUDES})
    # SHELL: keeps CMake from de-duplicating the repeated -include option.
    set(force "SHELL:-include \"${N64PORT_INCLUDE_DIR}/port_prelude.h\"")
    foreach(header ${N64PORT_FORCE_INCLUDES})
        list(APPEND force "SHELL:-include \"${header}\"")
    endforeach()
    target_compile_options(${name} PRIVATE ${N64PORT_DECOMP_FLAGS} ${force}
        $<$<COMPILE_LANGUAGE:C>:-std=gnu89 ${N64PORT_DECOMP_C_FLAGS}>
        $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17 -fno-exceptions -fno-rtti -Wno-narrowing ${N64PORT_DECOMP_CXX_FLAGS}>)
    set_target_properties(${name} PROPERTIES MSVC_RUNTIME_LIBRARY "")
endfunction()

# How the display list is drawn: "soft" = software rasteriser into an RGBA buffer (any host),
# "gpu" = through a GPU backend (port_gpu.h; GX on GameCube / Wii).
set(N64PORT_GFX "soft" CACHE STRING "Renderer: soft or gpu")

option(N64PORT_ROM_STREAM "Read the ROM from storage on demand instead of holding it in memory" OFF)

function(n64port_host_objects name)
    add_library(${name} OBJECT ${ARGN})
    if(N64PORT_ROM_STREAM)
        target_compile_definitions(${name} PRIVATE PORT_ROM_STREAM=1)
    endif()
    if(N64PORT_GFX STREQUAL "gpu")
        target_compile_definitions(${name} PRIVATE PORT_GFX_GPU=1)
    endif()
    if(N64PORT_CONSOLE STREQUAL "GameCube")
        target_compile_definitions(${name} PRIVATE "PORT_GPU_TEX_BUDGET=(2u<<20)")
    endif()
    target_include_directories(${name} PRIVATE "${N64PORT_INCLUDE_DIR}" "${N64PORT_DIR}/runtime/host")
    target_compile_options(${name} PRIVATE -Wall -Wno-unused-function)
    target_compile_definitions(${name} PRIVATE "PORT_ARENA_SIZE=(${N64PORT_ARENA_MB}ull<<20)")
    if(N64PORT_PIC)
        target_compile_options(${name} PRIVATE -fPIC)
    endif()
    set_target_properties(${name} PROPERTIES MSVC_RUNTIME_LIBRARY "")
endfunction()

# ---------------------------------------------------------------------------
# Overlay sections
# ---------------------------------------------------------------------------
# Sources of an overlay are compiled with `-include ovl_<n>.h` (tools/gen_overlays.py). On COFF
# that header's #pragma data_seg / bss_seg puts the overlay's variables in their own sections.
# GCC has no such pragma, so on ELF a compiler launcher renames .data / .bss in the finished
# object instead (tools/cc_overlay.py).
function(n64port_overlay_sections)
    if(N64PORT_COFF)
        return()
    endif()
    set(launcher "${Python3_EXECUTABLE}" "${N64PORT_DIR}/tools/cc_overlay.py" "${CMAKE_OBJCOPY}")
    if(N64PORT_CONSOLE MATCHES "Wii|GameCube|3DS")
        # libogc's linker script puts unknown no-load sections after .bss, inside the heap;
        # 3dsxtool only carries what lies inside the code / rodata / data segments.
        list(APPEND launcher "--bss-as-data")
    endif()
    list(APPEND launcher "--")
    foreach(target ${ARGN})
        set_target_properties(${target} PROPERTIES
            C_COMPILER_LAUNCHER "${launcher}" CXX_COMPILER_LAUNCHER "${launcher}")
    endforeach()
endfunction()

# ---------------------------------------------------------------------------
# Guest model: "native" compiles the game for the host itself; "wasm-be" compiles it to a
# big-endian WebAssembly guest translated back to C (for little-endian hosts and games that
# read big-endian ROM data directly; see cmake/N64Wasm.cmake).
# ---------------------------------------------------------------------------
set(N64PORT_GUEST "native" CACHE STRING "Guest model: native or wasm-be")
include("${CMAKE_CURRENT_LIST_DIR}/N64Wasm.cmake")

# ---------------------------------------------------------------------------
# Console libraries: keep the game's C library to itself
# ---------------------------------------------------------------------------
# libultra brings its own sprintf / memcpy / strlen / guPerspective ... and the runtime a few
# more (bzero, bcopy). Linked into a console executable they clash with newlib and libogc, or
# the engine's calls bind to them. After the library is built, every symbol it shares with
# those libraries is renamed n64_<name> inside it (tools/isolate_symbols.py).
function(n64port_isolate_system_symbols target)
    if(NOT N64PORT_CONSOLE MATCHES "Wii|GameCube|3DS")
        return()
    endif()
    execute_process(COMMAND "${CMAKE_C_COMPILER}" -print-file-name=libc.a OUTPUT_VARIABLE libc OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND "${CMAKE_C_COMPILER}" -print-file-name=libm.a OUTPUT_VARIABLE libm OUTPUT_STRIP_TRAILING_WHITESPACE)
    # <triplet>-gcc -> <triplet>-nm, next to it
    get_filename_component(nm_dir "${CMAKE_C_COMPILER}" DIRECTORY)
    get_filename_component(cc_name "${CMAKE_C_COMPILER}" NAME_WE)
    string(REGEX REPLACE "gcc$" "nm" nm_name "${cc_name}")
    set(nm "${nm_dir}/${nm_name}${CMAKE_EXECUTABLE_SUFFIX}")
    if(CMAKE_HOST_WIN32)
        set(nm "${nm_dir}/${nm_name}.exe")
    endif()
    if(N64PORT_CONSOLE STREQUAL "3DS")
        # libctru brings its own os* functions (osGetTime ...) that share libultra's names
        file(GLOB system_libs "${N64PORT_CTRULIB}/lib/*.a")
    else()
        # both libogc flavours: the engine links libogc on Wii and libogc2 on GameCube
        file(GLOB system_libs "${DEVKITPRO}/libogc/lib/wii/*.a" "${DEVKITPRO}/libogc/lib/cube/*.a"
                              "${DEVKITPRO}/libogc2/*/lib/*.a" "${N64PORT_OGC_LIBDIR}/*.a")
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${Python3_EXECUTABLE}" "${N64PORT_DIR}/tools/isolate_symbols.py" "${nm}" "${CMAKE_OBJCOPY}"
                "$<TARGET_FILE:${target}>" "${libc}" "${libm}" ${system_libs}
        VERBATIM)
endfunction()

# ---------------------------------------------------------------------------
# Headless runner
# ---------------------------------------------------------------------------
function(n64port_add_host_exe name library)
    if(N64PORT_CONSOLE STREQUAL "3DS")
        # Standalone 3DS runner (no engine); see host/main_ctr.c.
        add_executable(${name} "${N64PORT_DIR}/host/main_ctr.c")
        set_target_properties(${name} PROPERTIES SUFFIX ".elf")
        target_include_directories(${name} PRIVATE "${N64PORT_INCLUDE_DIR}")
        target_link_directories(${name} PRIVATE "${N64PORT_CTRULIB}/lib")
        target_link_options(${name} PRIVATE -specs=3dsx.specs "-Wl,-Map,${CMAKE_BINARY_DIR}/${name}.map")
        target_link_libraries(${name} PRIVATE ${library} citro3d ctru m)
        add_custom_command(TARGET ${name} POST_BUILD
            COMMAND "${N64PORT_3DSXTOOL}" "$<TARGET_FILE:${name}>" "${CMAKE_BINARY_DIR}/${name}.3dsx"
            VERBATIM)
        return()
    endif()
    if(N64PORT_CONSOLE MATCHES "Wii|GameCube")
        # Standalone console runner (no engine); see host/main_ogc.c.
        add_executable(${name} "${N64PORT_DIR}/host/main_ogc.c")
        set_target_properties(${name} PROPERTIES SUFFIX ".elf")
        target_include_directories(${name} PRIVATE "${N64PORT_INCLUDE_DIR}")
        target_link_directories(${name} PRIVATE "${N64PORT_OGC_LIBDIR}")
        target_link_libraries(${name} PRIVATE ${library} fat ogc m)
        if(N64PORT_GFX STREQUAL "gpu")
            target_compile_definitions(${name} PRIVATE PORT_GFX_GPU=1)
        endif()
        return()
    endif()
    add_executable(${name} "${N64PORT_DIR}/host/main.c")
    target_include_directories(${name} PRIVATE "${N64PORT_INCLUDE_DIR}")
    target_link_libraries(${name} PRIVATE ${library})
    if(N64PORT_COFF)
        target_link_libraries(${name} PRIVATE winmm dbghelp)
        target_link_options(${name} PRIVATE -fuse-ld=lld -Wl,/errorlimit:0 -Wl,/debug)
    else()
        target_link_libraries(${name} PRIVATE m pthread)
        if(N64PORT_PIC)
            target_compile_options(${name} PRIVATE -fPIE)
            target_link_options(${name} PRIVATE -pie)
        endif()
        if(N64PORT_SANITIZE)
            target_link_options(${name} PRIVATE -fsanitize=undefined)
        endif()
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            target_link_options(${name} PRIVATE -rdynamic) # symbol names in crash backtraces
        endif()
    endif()
endfunction()
