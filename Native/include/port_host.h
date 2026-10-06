/*
 * Boundary between the guest side (decomp code + libultra shim, freestanding,
 * decomp headers) and the host side (hosted C, Windows APIs). Only plain C
 * types cross it, so it can be included from both worlds.
 */
#ifndef PORT_HOST_H
#define PORT_HOST_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- logging / fatal errors ------------------------------------------- */
void port_log(const char *fmt, ...);
void port_fatal(const char *fmt, ...);
/* args: pointer to the caller's va_list (both sides use the platform va_list). */
void port_logv(const char *prefix, const char *fmt, void *args);

/* Route log lines somewhere other than stderr (the addon sends them to the engine log). */
void port_set_log_sink(void (*sink)(const char *line));
/* Contain game crashes instead of taking the process down (see port_host.c). */
void port_set_fault_containment(int enable);
int port_faulted(void);
/* Non-zero while code that handles its own hardware faults is running (the fault filters stand down). */
extern int gPortFaultGuard;
/* Bring-up aid: while non-zero, backends log their individual steps. */
extern int gPortVerbose;
/* Call counters of runtime entry points (0 recv, 1 send, 2 DMA, 3 yield, 4 gfx task,
 * 5 audio task, 6 thread create, 7 retrace wait); see the hang report in host/main_ogc.c. */
extern unsigned int gPortProgress[8];

int port_env_int(const char *name);

/* The game runs inside the editor, on the developer's machine: runtimes may use development-only
 * fallbacks then (a recompiled game boots the ROM it was built from when it finds no other).
 * Off by default, so packaged games only use what they ship with. */
void port_set_development(int on);
int port_development(void);

/* Bring-up aid: a fast free-running counter (CPU ticks where the platform has one, else 0) and
 * the display list interpreter's time split, filled while gPortVerbose is set:
 * [0] vertices, [1] texture loads, [2] texture lookups that hashed, [3] decodes; [4..7] counts. */
unsigned long long port_ticks(void);
extern unsigned long long gPortGfxProfile[8];

/* ---- memory ------------------------------------------------------------ */
/* Zeroed memory within 4 GB above the module image (tokenisable, see port_prelude.h). */
void *port_arena_alloc(unsigned long long size, unsigned long long align);
void port_arena_reset(void);
unsigned long long port_arena_used(void); /* bytes handed out so far */
/*
 * Whether an address is the port's own memory (the arena) or the module's data rather than an
 * N64 segmented address (segment << 24 | offset). Only needed on 32-bit hosts whose memory
 * starts below 0x10000000 (3DS, ARM Linux), where the two can look alike.
 */
int port_addr_is_native(const void *p);

void port_memcpy(void *dst, const void *src, unsigned long long size);
void port_memset(void *dst, int value, unsigned long long size);

/* ---- overlays ------------------------------------------------------------ */
/* Variables of one N64 overlay (see tools/gen_overlays.py). */
typedef struct PortOverlay
{
    const char *rom_start; /* SYOverlay.rom_start: address of the <name>_ROM_START symbol */
    const char *name;
    char *data_start, *data_end;
    char *bss_start, *bss_end;
} PortOverlay;

extern const PortOverlay gPortOverlays[];
extern const unsigned int gPortOverlaysNum;

/* First call: remember every overlay's initial data. Later calls: put all of them back. */
void port_overlays_reset_all(void);
/* What loading an overlay did on the N64: its variables are back in their initial state. */
void port_overlay_load(unsigned long long rom_start);

/* ---- save data ----------------------------------------------------------- */
/* Whole-file helpers for the save RAM image. Read returns the number of bytes read. */
unsigned int port_file_read(const char *path, void *data, unsigned int size);
int port_file_write(const char *path, const void *data, unsigned int size);

/* ---- ROM ---------------------------------------------------------------- */
/* An asset pack (tools/make_rom_pack.py) or a whole ROM; .v64 / .n64 dumps are put in .z64 order
 * as they load (not when streaming, PORT_ROM_STREAM). */
int port_rom_load(const char *path);
/* Copy a piece of the ROM. */
void port_rom_read(unsigned int offset, void *dst, unsigned int size);
/* A piece of the ROM to look at; valid until the next port_rom_view() call. */
const unsigned char *port_rom_view(unsigned int offset, unsigned int size);
unsigned int port_rom_size(void);

/* ---- coroutines (one per N64 OSThread) ---------------------------------- */
typedef struct PortCoro PortCoro;
PortCoro *port_coro_create(void (*entry)(void *), void *arg, unsigned long long stack_size);
void port_coro_destroy(PortCoro *coro);
void port_coro_resume(PortCoro *coro); /* run until it yields or finishes */
void port_coro_yield(void);            /* from inside a coroutine, back to the resumer */
int port_coro_finished(PortCoro *coro);

/* ---- embedding API (what the addon / host exe calls) --------------------- */
typedef struct PortPad
{
    unsigned short buttons; /* N64 button mask */
    signed char stick_x, stick_y;
    unsigned char connected;
} PortPad;

/* Where the cartridge's save RAM is kept between runs; set before n64_boot(). Without a path
 * (the default) save data lives in memory only. */
void n64_set_save_path(const char *path);
int n64_boot(const char *rom_path);
/* 0 until booted; stays 1 after a contained fault, when n64_run_frame() becomes a no-op. */
int n64_is_running(void);
void n64_set_pad(int port, const PortPad *pad);
void n64_run_frame(void);
void n64_shutdown(void);
/* RGBA8 framebuffer produced by the last frame (software renderer). */
const unsigned char *n64_framebuffer(int *width, int *height);
/* Non-zero when a GPU backend draws the game straight into the host's render target instead:
 * there is no framebuffer to show, and each host frame needs at least one game frame. */
int n64_draws_to_screen(void);
/* While set, the game's display lists are not drawn (its logic still runs): a host that runs
 * several game frames before showing one skips drawing all but the last. */
void n64_set_skip_draw(int skip);
/* GPU backends: where the game's picture goes in the host's render target, in its pixels
 * (resolution scaler). width <= 0 = the whole target. No effect with the software renderer. */
void n64_set_display_rect(float x, float y, float width, float height);
/* Interleaved stereo S16 samples produced by the last frame. */
const short *n64_audio(int *frames, int *sample_rate);

#ifdef __cplusplus
}
#endif

#endif /* PORT_HOST_H */
