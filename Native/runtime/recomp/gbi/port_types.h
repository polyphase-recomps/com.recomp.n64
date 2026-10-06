/*
 * Basic types for the shared runtime's renderer and audio when they run over a recompiled game
 * (recomp mode): what a decomp's port_types.h provides otherwise. Written for com.recomp.n64;
 * the N64 SDK's headers are not used.
 */
#ifndef PORT_TYPES_H
#define PORT_TYPES_H

#include <PR/ultratypes.h> /* (runtime/host/include: exact-width u8..u64, f32, f64) */
#include <stdint.h>

/* booleans of a given width, as the runtime's interfaces use them */
typedef s8 sb8;
typedef s16 sb16;
typedef s32 sb32;
typedef u8 ub8;
typedef u16 ub16;
typedef u32 ub32;

#endif /* PORT_TYPES_H */
