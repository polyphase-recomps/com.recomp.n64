# devkitPPC + libogc toolchain for the native N64 runtime (library builds). Included by
# wii.cmake / gamecube.cmake, which set OGC_MACHINE_FLAG, OGC_HW_DEFINE and N64PORT_CONSOLE.
# devkitPro's own CMake files insist on msys2's cmake; this one works with any cmake.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR powerpc)

if(DEFINED ENV{DEVKITPRO})
    file(TO_CMAKE_PATH "$ENV{DEVKITPRO}" DEVKITPRO)
else()
    set(DEVKITPRO "C:/devkitPro")
endif()
set(DEVKITPPC "${DEVKITPRO}/devkitPPC")
if(CMAKE_HOST_WIN32)
    set(_exe ".exe")
endif()

set(CMAKE_C_COMPILER "${DEVKITPPC}/bin/powerpc-eabi-gcc${_exe}")
set(CMAKE_CXX_COMPILER "${DEVKITPPC}/bin/powerpc-eabi-g++${_exe}")
set(CMAKE_AR "${DEVKITPPC}/bin/powerpc-eabi-gcc-ar${_exe}" CACHE FILEPATH "")
set(CMAKE_RANLIB "${DEVKITPPC}/bin/powerpc-eabi-gcc-ranlib${_exe}" CACHE FILEPATH "")
set(CMAKE_OBJCOPY "${DEVKITPPC}/bin/powerpc-eabi-objcopy${_exe}" CACHE FILEPATH "")
# Compiler checks build a static library (linking an executable needs libogc on the link line).
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# The library must be built against the same libogc flavour the final executable links
# (they differ in inline functions and structure layouts). The Polyphase engine uses libogc2
# for GameCube and the original libogc for Wii; the console file sets OGC_USE_LIBOGC2.
if(OGC_USE_LIBOGC2 AND EXISTS "${DEVKITPRO}/libogc2/${OGC_SUBDIR}/include")
    set(_ogc_include "${DEVKITPRO}/libogc2/${OGC_SUBDIR}/include")
    set(N64PORT_OGC_LIBDIR "${DEVKITPRO}/libogc2/${OGC_SUBDIR}/lib")
elseif(OGC_SUBDIR STREQUAL "gamecube")
    set(_ogc_include "${DEVKITPRO}/libogc/include")
    set(N64PORT_OGC_LIBDIR "${DEVKITPRO}/libogc/lib/cube")
else()
    set(_ogc_include "${DEVKITPRO}/libogc/include")
    set(N64PORT_OGC_LIBDIR "${DEVKITPRO}/libogc/lib/wii")
endif()
set(_flags "${OGC_MACHINE_FLAG} -mcpu=750 -meabi -mhard-float -DGEKKO ${OGC_HW_DEFINE} -isystem \"${_ogc_include}\"")
set(CMAKE_C_FLAGS_INIT "${_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_flags}")
