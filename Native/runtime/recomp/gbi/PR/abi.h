/*
 * The audio binary interface: command opcodes and flags of the audio microcodes (aspMain and
 * n_aspMain share the numbering), as the runtime's interpreters (runtime/guest/port_audio.c,
 * port_audio_abi1.c) read them. Written for com.recomp.n64; the N64 SDK's headers are not used.
 * The values are checked against a decomp's headers by tools/recomp/check_gbi.py.
 */
#ifndef _ABI_H_
#define _ABI_H_

#include <port_types.h>

/* commands */
#define A_SPNOOP 0
#define A_ADPCM 1
#define A_CLEARBUFF 2
#define A_ENVMIXER 3
#define A_LOADBUFF 4
#define A_RESAMPLE 5
#define A_SAVEBUFF 6
#define A_SEGMENT 7
#define A_SETBUFF 8
#define A_SETVOL 9
#define A_DMEMMOVE 10
#define A_LOADADPCM 11
#define A_MIXER 12
#define A_INTERLEAVE 13
#define A_POLEF 14
#define A_SETLOOP 15

/* flags */
#define A_INIT 0x01
#define A_CONTINUE 0x00
#define A_LOOP 0x02
#define A_OUT 0x02
#define A_LEFT 0x02
#define A_RIGHT 0x00
#define A_VOL 0x04
#define A_RATE 0x00
#define A_AUX 0x08
#define A_NOAUX 0x00
#define A_MAIN 0x00
#define A_MIX 0x10

/* a command: two 32-bit words */
typedef struct
{
    u32 w0;
    u32 w1;
} Awords;

typedef union
{
    Awords words;
    long long force_union_align;
} Acmd;

#endif /* _ABI_H_ */
