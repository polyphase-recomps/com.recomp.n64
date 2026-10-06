/*
 * The little of the N64 OS interface the shared runtime's renderer and audio refer to in recomp
 * mode. Written for com.recomp.n64 (the N64 SDK's headers are not used): the recompiled game
 * brings its own OS code, and the runtime's replacements (runtime/recomp/recomp_os.c) work on
 * guest memory by offset.
 */
#ifndef _OS_H_
#define _OS_H_

#include <port_types.h>

/* KSEG0 / KSEG1 address -> physical address */
#ifndef K0_TO_PHYS
#define K0_TO_PHYS(addr) ((u32)(addr) & 0x1FFFFFFF)
#endif

#endif /* _OS_H_ */
