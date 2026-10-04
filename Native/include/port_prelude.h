/*
 * Force-included before every decomp translation unit in the native build.
 * Establishes the host type model before the decomp's own freestanding libc
 * headers get a chance to define N64-sized types.
 */
#ifndef PORT_PRELUDE_H
#define PORT_PRELUDE_H

#ifndef PORT
#define PORT 1
#endif

/* ultratypes.h / string.h would otherwise typedef a 32-bit size_t. */
#define _SIZE_T
#define _SIZE_T_
#define _SIZE_T_DEF
typedef __SIZE_TYPE__ size_t;

/*
 * Host properties the PORT code paths depend on. PORT alone means "native build"; these say
 * what the native machine is like:
 *   PORT_64BIT          pointers are wider than the N64's (structs holding pointers grow)
 *   PORT_BIG_ENDIAN     same byte order as the N64 (GameCube, Wii); otherwise little-endian
 */
#if __SIZEOF_POINTER__ == 8
#define PORT_64BIT 1
#endif
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define PORT_BIG_ENDIAN 1
#else
#define PORT_LITTLE_ENDIAN 1
#endif

/* The symbol the linker puts at the start of the module image (64-bit hosts only). */
#ifdef _WIN32
#define PORT_IMAGE_BASE __ImageBase
#else
#define PORT_IMAGE_BASE __ehdr_start
#endif

/*
 * Pointer tokens.
 *
 * Several N64 data formats are streams of 32-bit words that embed pointers
 * (fighter motion scripts, AObjEvent32 animation scripts, colanim scripts,
 * pointer tables typed as u32[]). They keep their 4-byte stride in the native
 * build; a pointer word holds a 32-bit *token*: the target's offset from the
 * module image base, with 0 meaning NULL.
 *
 * A token cannot be a C constant expression, so translation units whose static
 * data needs one are compiled as C++ (see tools/gen_cxx_wrappers.py), where
 * PortTok's converting constructor computes it during static initialisation.
 * Game code turns a token back into a pointer with PORT_PTR().
 *
 * Everything the game can point at (module data and the port's memory arena,
 * which port_arena.c reserves just above the image) lies within 4 GB of the
 * image base, so runtime pointers can be tokenised as well (PORT_TOK_RT).
 *
 * On a 32-bit host a pointer fits the word, as it did on the N64: the token is
 * the pointer itself and the same macros reduce to casts.
 */
#ifdef PORT_64BIT
#define PORT_TOK_OF(p) ((unsigned int)((unsigned long long)(p) - (unsigned long long)&PORT_IMAGE_BASE))
#define PORT_TOK_ADDR(tok) ((void *)((char *)&PORT_IMAGE_BASE + (unsigned int)(tok)))
#ifdef __cplusplus
extern "C" char PORT_IMAGE_BASE;
#else
extern char PORT_IMAGE_BASE;
#endif
#else
#define PORT_TOK_OF(p) ((unsigned int)(p))
#define PORT_TOK_ADDR(tok) ((void *)(unsigned int)(tok))
#endif

#ifdef __cplusplus

struct PortTok
{
    unsigned int v;

    constexpr PortTok(int x) : v((unsigned int)x) {}
    constexpr PortTok(unsigned int x) : v(x) {}
    constexpr PortTok(long x) : v((unsigned int)x) {}
    constexpr PortTok(unsigned long x) : v((unsigned int)x) {}
    constexpr PortTok(long long x) : v((unsigned int)x) {}
    constexpr PortTok(unsigned long long x) : v((unsigned int)x) {}
    template <class T>
    PortTok(T *p)
        : v(p ? PORT_TOK_OF(p) : 0u) {}

    constexpr operator unsigned int() const { return v; }
};
#else
typedef unsigned int PortTok;
#endif

#define PORT_TOK(x) ((PortTok)(x))

/* Token -> pointer. Usable from C and C++. */
#define PORT_PTR(type, tok) \
    ((type)((tok) != 0 ? PORT_TOK_ADDR(tok) : (void *)0))

/* Runtime pointer -> token (C code; the C++ PortTok constructor covers static data). */
#define PORT_TOK_RT(p) \
    ((p) != 0 ? PORT_TOK_OF(p) : 0u)

#endif /* PORT_PRELUDE_H */
