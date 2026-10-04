/* Embedding API: boots the game and steps it one video frame at a time. */
#include <port_types.h>
#include <PR/os.h>
#include <port_host.h>
#include <port_game.h>
#include "port_guest.h"

extern u32 gPortGfxTraceFrame;

static sb32 sIsBooted;

/* General heap handed to every scene (see syTaskmanInitGeneralHeap). */
#ifndef PORT_SCENE_ARENA_SIZE
#define PORT_SCENE_ARENA_SIZE (48 * 1024 * 1024)
#endif

void *port_scene_arena(u32 *size)
{
    static void *sArena;

    if (sArena == NULL)
    {
        sArena = port_arena_alloc(PORT_SCENE_ARENA_SIZE, 16);
    }
    *size = PORT_SCENE_ARENA_SIZE;
    return sArena;
}

int n64_boot(const char *rom_path)
{
    PortPad pad = { 0, 0, 0, 1 };

    if (!port_rom_load(rom_path))
    {
        return 0;
    }
    port_log("boot: game data loaded");
    port_overlays_reset_all();
    port_io_load_save();
    gPortGfxTraceFrame = port_env_int("SSB64_GFX_TRACE"); /* (name kept from the first game ported) */
    n64_set_pad(0, &pad);

    port_game_main();
    port_os_run();
    sIsBooted = TRUE;
    port_log("boot: threads started");
    return 1;
}

void n64_run_frame(void)
{
    if (!sIsBooted || port_faulted())
    {
        return;
    }
    port_io_frame_begin();
    port_audio_frame_begin();
    port_os_post_vi_retrace();
    port_os_run();
#ifdef PORT_GFX_GPU
    {
        extern void port_gpu_host_idle(void);

        port_gpu_host_idle();
    }
#endif
}

int n64_is_running(void)
{
    return sIsBooted && !port_faulted();
}

void n64_shutdown(void)
{
    port_os_shutdown();
    sIsBooted = FALSE;
}


