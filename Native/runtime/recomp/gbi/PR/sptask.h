/*
 * The RSP task header, as a recompiled game lays it out in RDRAM (64 bytes, 32-bit words), for
 * the runtime's prototypes; recomp mode reads tasks by offset (runtime/recomp/recomp_io.c).
 * Written for com.recomp.n64; the N64 SDK's headers are not used.
 */
#ifndef _SPTASK_H_
#define _SPTASK_H_

#include <port_types.h>

/* task types (OSTask_t.type) */
#define M_GFXTASK 1
#define M_AUDTASK 2
#define M_VIDTASK 3

typedef struct
{
    u32 type;
    u32 flags;
    u32 ucode_boot;
    u32 ucode_boot_size;
    u32 ucode;
    u32 ucode_size;
    u32 ucode_data;
    u32 ucode_data_size;
    u32 dram_stack;
    u32 dram_stack_size;
    u32 output_buff;
    u32 output_buff_size;
    u32 data_ptr;  /* the display list / audio command list (offset 48) */
    u32 data_size; /* its size in bytes (offset 52) */
    u32 yield_data_ptr;
    u32 yield_data_size;
} OSTask_t;

typedef union
{
    OSTask_t t;
    long long force_structure_alignment;
} OSTask;

#endif /* _SPTASK_H_ */
