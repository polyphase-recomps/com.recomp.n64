# Wii (devkitPPC + libogc), library builds.
set(OGC_MACHINE_FLAG "-mrvl")
set(OGC_HW_DEFINE "-DHW_RVL")
set(OGC_USE_LIBOGC2 OFF) # what the engine's Makefile for this console includes
set(OGC_SUBDIR "wii")
set(N64PORT_CONSOLE "Wii")
include("${CMAKE_CURRENT_LIST_DIR}/ogc-common.cmake")
