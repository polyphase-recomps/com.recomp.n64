/*
 * Compiled per game, next to N64Recomp's output: hands the generated section table to the
 * runtime (recomp_rt.c). The include path has the game's generated folder first.
 */
#include "recomp.h"
#include "funcs.h"
#include "librecomp/sections.h"

#include "recomp_overlays.inl"

extern "C" const void *recomp_section_table(size_t *count)
{
    *count = num_sections;
    return section_table;
}
