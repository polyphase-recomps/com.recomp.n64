# GameCube (devkitPPC + libogc), library builds.
set(OGC_MACHINE_FLAG "-mogc")
set(OGC_HW_DEFINE "-DHW_DOL")
set(OGC_USE_LIBOGC2 ON) # what the engine's Makefile for this console includes
set(OGC_SUBDIR "gamecube")
set(N64PORT_CONSOLE "GameCube")
include("${CMAKE_CURRENT_LIST_DIR}/ogc-common.cmake")
