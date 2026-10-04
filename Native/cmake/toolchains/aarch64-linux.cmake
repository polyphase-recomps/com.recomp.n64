# 64-bit little-endian ARM Linux (run with qemu-aarch64): the same CPU and data model as
# Android arm64, testable headlessly.
#   cmake -DCMAKE_TOOLCHAIN_FILE=<this file> ...
#   qemu-aarch64 -L /usr/aarch64-linux-gnu ./ssb64_host ...
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
