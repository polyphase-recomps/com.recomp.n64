/* Internal interface between the guest-side port modules. */
#ifndef PORT_GUEST_H
#define PORT_GUEST_H

#include <port_types.h>
#include <PR/os.h>
#include <PR/sptask.h>

/* port_os.c */
void port_os_run(void);
void port_os_shutdown(void);
void port_os_post_event(s32 event);
void port_os_post_vi_retrace(void);

/* port_io.c */
void port_io_frame_begin(void);
void port_io_load_save(void);
extern u32 gPortFrameCount;

/* port_gfx.c: run a graphics task's display list */
void port_gfx_run_task(OSTask *task);
void port_gfx_set_framebuffer(void *fb);
/* Level of detail of models that choose it by distance (gSPBranchLessZ): */
enum { PORT_LOD_DYNAMIC, PORT_LOD_FULL, PORT_LOD_LOW }; /* by distance (as on the N64) / always the most / least detailed */
void port_gfx_set_lod(s32 mode);
s32 port_gfx_lod(void);

/* port_audio.c (or port_audio_stub.c): the audio microcode, the DAC and audio heap addresses */
void port_audio_run_task(OSTask *task);
void port_audio_submit(void *samples, u32 size);
void port_audio_frame_begin(void);
u32 port_audio_ai_length(void);
void port_audio_set_rate(u32 rate); /* osAiSetFrequency */
sb32 port_audio_is_heap(const void *ptr);
u32 port_audio_addr(const void *ptr);

#endif /* PORT_GUEST_H */
