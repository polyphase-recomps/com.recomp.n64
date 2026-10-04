/*
 * What a platform backend (port_host_<platform>.c) provides to the common host code, besides
 * the coroutine API and port_set_fault_containment() declared in port_host.h.
 */
#ifndef PORT_HOST_PLAT_H
#define PORT_HOST_PLAT_H

/* Shared state (port_host.c). */
extern int gPortContainFaults; /* port_set_fault_containment() */
extern int gPortFaulted;       /* a contained fault stopped the game */

/* Address space for the arena: `size` bytes that pointer tokens can reach. NULL on failure. */
unsigned char *port_plat_arena_reserve(unsigned long long size);
/* Make part of the reservation usable (zero-filled). Returns 0 on failure. */
int port_plat_arena_commit(unsigned char *addr, unsigned long long size);

/* End the process after a fatal error, in the way a debugger / crash handler sees best. */
void port_plat_abort(void);
/* Give up on the running coroutine and return to its resumer; marks the runtime as faulted. */
void port_plat_coro_abandon(void);

#endif /* PORT_HOST_PLAT_H */
