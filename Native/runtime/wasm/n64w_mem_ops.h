/*
 * Included by tools/wasm_to_c.py into the wasm2c -impl.h, just before the load and store
 * helpers are instantiated: masked addresses (n64w.h), no bounds checks.
 */
#include "n64w.h"

#undef MEM_ADDR
#define MEM_ADDR(mem, addr, n) (&(mem)->data[N64W_OFFSET((u32)(addr))])
#undef MEM_ADDR_MEMOP
#define MEM_ADDR_MEMOP(mem, addr, n) MEM_ADDR(mem, addr, n)
#undef RANGE_CHECK
#define RANGE_CHECK(mem, offset, len)
#undef MEMCHECK_DEFAULT32
#define MEMCHECK_DEFAULT32(mem, local_memory_size, a, t)
#undef MEMCHECK_GENERAL
#define MEMCHECK_GENERAL(mem, a, t)
#undef LOAD_DATA
#define LOAD_DATA(m, o, i, s) wasm_rt_memcpy(MEM_ADDR(&(m), o, s), i, s)
