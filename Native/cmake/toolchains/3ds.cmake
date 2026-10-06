# Nintendo 3DS (devkitARM + libctru + citro3d), library builds and the standalone test runner.
# devkitPro's own CMake files insist on msys2's cmake; this one works with any cmake.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(N64PORT_CONSOLE "3DS")

if(DEFINED ENV{DEVKITPRO})
    file(TO_CMAKE_PATH "$ENV{DEVKITPRO}" DEVKITPRO)
else()
    set(DEVKITPRO "C:/devkitPro")
endif()
set(DEVKITARM "${DEVKITPRO}/devkitARM")
set(N64PORT_CTRULIB "${DEVKITPRO}/libctru")
if(CMAKE_HOST_WIN32)
    set(_exe ".exe")
endif()

set(CMAKE_C_COMPILER "${DEVKITARM}/bin/arm-none-eabi-gcc${_exe}")
set(CMAKE_CXX_COMPILER "${DEVKITARM}/bin/arm-none-eabi-g++${_exe}")
set(CMAKE_AR "${DEVKITARM}/bin/arm-none-eabi-gcc-ar${_exe}" CACHE FILEPATH "")
set(CMAKE_RANLIB "${DEVKITARM}/bin/arm-none-eabi-gcc-ranlib${_exe}" CACHE FILEPATH "")
set(CMAKE_OBJCOPY "${DEVKITARM}/bin/arm-none-eabi-objcopy${_exe}" CACHE FILEPATH "")
set(N64PORT_3DSXTOOL "${DEVKITPRO}/tools/bin/3dsxtool${_exe}")
set(N64PORT_PICASSO "${DEVKITPRO}/tools/bin/picasso${_exe}")
# Compiler checks build a static library (linking an executable needs libctru on the link line).
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# The flags of the Polyphase engine's Makefile_3DS (the library is linked into its executable).
set(_flags "-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -mword-relocations -D__3DS__ -isystem \"${N64PORT_CTRULIB}/include\"")
set(CMAKE_C_FLAGS_INIT "${_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_flags}")
