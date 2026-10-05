# Big-endian wasm guest build (included by N64Port.cmake when N64PORT_GUEST is "wasm-be").
#
# For games that read big-endian data straight from the ROM, on little-endian hosts: the game
# and the runtime's guest code are compiled to WebAssembly that sees big-endian memory
# (tools/wasm_be_cc.py), linked into one module (wasm-ld, wasi-sdk), translated to C (wasm2c,
# WABT; tools/wasm_to_c.py) and compiled for the host together with runtime/wasm/n64w_host.c,
# which offers the usual port_host.h embedding API. See runtime/wasm/n64w.h for the memory.
#
#   n64port_wasm_guest(<name> SOURCES <guest sources...> [DEFS ...] [INCLUDES ...] [FORCE ...])
#     -> static library <name> (host code) and the headless runner <name>_host

# Finds <pattern> in a Tools/ folder above the project; <probe> is a program inside it, so a
# folder of tools for another OS (Windows .exe files seen from WSL) is passed over.
function(_n64port_find_tool var pattern probe)
    if(${var} AND EXISTS "${${var}}")
        return()
    endif()
    set(dir "${CMAKE_SOURCE_DIR}")
    foreach(i RANGE 12)
        file(GLOB found LIST_DIRECTORIES true "${dir}/Tools/${pattern}")
        list(SORT found)
        list(REVERSE found)
        foreach(candidate ${found})
            if(EXISTS "${candidate}/${probe}${CMAKE_HOST_EXECUTABLE_SUFFIX}")
                set(${var} "${candidate}" CACHE PATH "${pattern}" FORCE)
                return()
            endif()
        endforeach()
        get_filename_component(dir "${dir}" DIRECTORY)
    endforeach()
    message(FATAL_ERROR "${pattern} (with ${probe}${CMAKE_HOST_EXECUTABLE_SUFFIX}) not found in a Tools/ folder "
        "above the project: download wasi-sdk and WABT for this OS into one, or set ${var}")
endfunction()

function(n64port_wasm_guest name)
    cmake_parse_arguments(G "" "" "SOURCES;DEFS;INCLUDES;FORCE" ${ARGN})
    _n64port_find_tool(N64PORT_WASI_SDK "wasi-sdk-*" bin/clang)
    _n64port_find_tool(N64PORT_WABT "wabt-*" bin/wasm2c)
    set(wasm_cc "${N64PORT_WASI_SDK}/bin/clang${CMAKE_HOST_EXECUTABLE_SUFFIX}")
    set(wasm_ld "${N64PORT_WASI_SDK}/bin/wasm-ld${CMAKE_HOST_EXECUTABLE_SUFFIX}")
    file(GLOB wasm_builtins "${N64PORT_WASI_SDK}/lib/clang/*/lib/wasm32-unknown-wasip1/libclang_rt.builtins.a")
    set(wasm2c "${N64PORT_WABT}/bin/wasm2c${CMAKE_HOST_EXECUTABLE_SUFFIX}")
    set(tools "${N64PORT_DIR}/tools")
    set(wdir "${CMAKE_BINARY_DIR}/wasm")
    file(MAKE_DIRECTORY "${wdir}/obj")

    set(flags -ffreestanding -funsigned-char -fno-strict-aliasing -fwrapv -ffp-contract=off -fno-builtin -fno-math-errno
        -fno-common -nostdlibinc -Wno-everything -std=gnu89 -O2 -DPORT=1 -DPORT_WASM=1)
    foreach(d ${G_DEFS})
        list(APPEND flags "-D${d}")
    endforeach()
    foreach(d "${N64PORT_INCLUDE_DIR}" "${N64PORT_DIR}/runtime/wasm" ${G_INCLUDES})
        list(APPEND flags "-I${d}")
    endforeach()
    list(APPEND flags -include "${N64PORT_INCLUDE_DIR}/port_prelude.h")
    foreach(h ${G_FORCE})
        list(APPEND flags -include "${h}")
    endforeach()

    set(objs)
    set(index 0)
    foreach(src ${G_SOURCES} "${N64PORT_DIR}/runtime/wasm/n64w_guest.c")
        get_filename_component(stem "${src}" NAME_WE)
        set(obj "${wdir}/obj/${index}_${stem}.o")
        math(EXPR index "${index} + 1")
        # per-source extras: the directory of a generated copy, overlay section headers
        set(extra)
        get_source_file_property(src_includes "${src}" INCLUDE_DIRECTORIES)
        if(src_includes)
            foreach(d ${src_includes})
                list(APPEND extra "-I${d}")
            endforeach()
        endif()
        get_source_file_property(src_flags "${src}" COMPILE_FLAGS)
        if(src_flags)
            separate_arguments(src_flags NATIVE_COMMAND "${src_flags}")
            list(APPEND extra ${src_flags})
        endif()
        add_custom_command(OUTPUT "${obj}"
            COMMAND "${Python3_EXECUTABLE}" "${tools}/wasm_be_cc.py" "${wasm_cc}" ${flags} ${extra} -c "${src}" -o "${obj}"
            DEPENDS "${src}" "${tools}/wasm_be_cc.py"
            IMPLICIT_DEPENDS C "${src}"
            COMMENT "wasm-be ${stem}.c"
            VERBATIM)
        list(APPEND objs "${obj}")
    endforeach()
    # the start-up byte swap reads its tables as they are
    set(fix_obj "${wdir}/obj/be_fixup.o")
    add_custom_command(OUTPUT "${fix_obj}"
        COMMAND "${Python3_EXECUTABLE}" "${tools}/wasm_be_cc.py" --native "${wasm_cc}" -O2 -nostdlibinc -ffreestanding
            -c "${N64PORT_DIR}/runtime/wasm/be_fixup.c" -o "${fix_obj}"
        DEPENDS "${N64PORT_DIR}/runtime/wasm/be_fixup.c" "${tools}/wasm_be_cc.py"
        COMMENT "wasm be_fixup.c" VERBATIM)
    list(APPEND objs "${fix_obj}")

    # The module's entry points (runtime/wasm/n64w_guest.c); export_name attributes are lost
    # on the way through the MIPS front end, so they are named here.
    set(exports)
    foreach(e __wasm_call_ctors n64w_boot n64w_run_frame n64w_is_running n64w_set_pad n64w_path_buffer
              n64w_set_save_path n64w_framebuffer n64w_audio n64w_out_a n64w_out_b n64w_draws_to_screen
              n64w_coro_entry n64w_scratch n64w_args n64w_value n64w_arena_used n64w_bridge_var_count
              n64w_bridge_request_count n64w_bridge_var n64w_bridge_request_info n64w_bridge_get
              n64w_bridge_request n64w_bridge_result n64w_bridge_poll_event)
        list(APPEND exports "--export=${e}")
    endforeach()

    # Memory: data from 0x80000400 (N64 KSEG0), 64 MB window (runtime/wasm/n64w.h).
    set(wasm "${wdir}/${name}.wasm")
    add_custom_command(OUTPUT "${wasm}"
        COMMAND "${Python3_EXECUTABLE}" "${tools}/wasm_link.py" "${wasm_ld}" --no-entry --allow-undefined ${exports}
            --no-stack-first -z stack-size=1048576 --global-base=2147484672
            --initial-memory=2214592512 --max-memory=2214592512 --error-limit=0
            ${objs} ${wasm_builtins} -o "${wasm}"
        DEPENDS ${objs} "${tools}/wasm_link.py"
        COMMENT "wasm-ld ${name}.wasm"
        VERBATIM)

    set(w2c_dir "${CMAKE_BINARY_DIR}/w2c")
    set(w2c_count 8)
    set(w2c_c)
    math(EXPR last "${w2c_count} - 1")
    foreach(i RANGE ${last})
        list(APPEND w2c_c "${w2c_dir}/${name}_guest_${i}.c")
    endforeach()
    add_custom_command(OUTPUT ${w2c_c} "${w2c_dir}/${name}_guest.h" "${w2c_dir}/${name}_guest-impl.h"
        COMMAND "${Python3_EXECUTABLE}" "${tools}/wasm_to_c.py" "${wasm2c}" "${wasm}" "${w2c_dir}" ${name} ${w2c_count}
        DEPENDS "${wasm}" "${tools}/wasm_to_c.py"
        COMMENT "wasm2c ${name}.wasm"
        VERBATIM)

    # Host side: the translated module, its runtime and the platform host.
    add_library(${name} STATIC ${w2c_c} "${N64PORT_DIR}/runtime/wasm/n64w_host.c" "${N64PORT_DIR}/runtime/wasm/n64w_rt.c"
        "${N64PORT_DIR}/runtime/host/port_host.c" "${N64PORT_DIR}/runtime/host/port_host_win32.c"
        "${N64PORT_DIR}/runtime/host/port_host_posix.c")
    target_include_directories(${name} PRIVATE "${w2c_dir}" "${N64PORT_DIR}/runtime/wasm" "${N64PORT_INCLUDE_DIR}"
        "${N64PORT_DIR}/runtime/host")
    target_compile_definitions(${name} PRIVATE N64W_MODULE=${name} PORT_WASM_HOST=1)
    target_compile_options(${name} PRIVATE -O2 -w)
    set_target_properties(${name} PROPERTIES MSVC_RUNTIME_LIBRARY "")
    if(CMAKE_C_COMPILER_ID MATCHES "Clang" AND WIN32)
        # no default C runtime named in the objects: the addon links whichever it uses (/MD or /MDd)
        target_compile_options(${name} PRIVATE -fms-omit-default-lib)
    endif()

    add_executable(${name}_host "${N64PORT_DIR}/host/main.c")
    target_include_directories(${name}_host PRIVATE "${N64PORT_INCLUDE_DIR}")
    target_link_libraries(${name}_host PRIVATE ${name})
    if(WIN32)
        target_link_libraries(${name}_host PRIVATE winmm dbghelp)
        target_link_options(${name}_host PRIVATE -fuse-ld=lld -Wl,/debug)
    else()
        target_link_libraries(${name}_host PRIVATE m pthread)
    endif()
endfunction()
