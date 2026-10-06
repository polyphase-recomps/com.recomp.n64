/*
 * N64 basic types for code that runs natively on the host and reads game memory (the display
 * list interpreter and RSP audio in host mode: PORT_RSP_HOST). Searched before a game's own
 * headers so it takes the place of the SDK's ultratypes.h: that one declares u32 / s32 as
 * `long`, which is 64 bits on LP64 hosts (Linux, macOS) and breaks every 32-bit read of game
 * memory there. These are exact-width on every host.
 *
 * Same include guard as the SDK header, so whichever comes first wins and the other is skipped.
 */
#ifndef _ULTRATYPES_H_
#define _ULTRATYPES_H_

#if defined(_LANGUAGE_C) || defined(_LANGUAGE_C_PLUS_PLUS) || !defined(_LANGUAGE_ASSEMBLY)
#include <stddef.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

typedef volatile uint8_t vu8;
typedef volatile uint16_t vu16;
typedef volatile uint32_t vu32;
typedef volatile uint64_t vu64;

typedef volatile int8_t vs8;
typedef volatile int16_t vs16;
typedef volatile int32_t vs32;
typedef volatile int64_t vs64;

typedef float f32;
typedef double f64;
#endif

#ifndef TRUE
#define TRUE 1
#endif

#ifndef FALSE
#define FALSE 0
#endif

#ifndef NULL
#define NULL 0
#endif

#endif /* _ULTRATYPES_H_ */
