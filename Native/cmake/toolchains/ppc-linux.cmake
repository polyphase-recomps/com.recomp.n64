# 32-bit big-endian PowerPC Linux (run with qemu-ppc): the closest thing to a GameCube / Wii
# that can be tested headlessly. Not a shipping target.
#   cmake -DCMAKE_TOOLCHAIN_FILE=<this file> ...
#   qemu-ppc -L /usr/powerpc-linux-gnu ./ssb64_host ...
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR ppc)
set(CMAKE_C_COMPILER powerpc-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER powerpc-linux-gnu-g++)
set(CMAKE_FIND_ROOT_PATH /usr/powerpc-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
