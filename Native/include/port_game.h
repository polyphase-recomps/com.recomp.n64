/*
 * What a game has to supply to the shared N64 runtime. Each game package implements these
 * (see com.recomp.ssmb64/Native/port/guest/port_game.c) next to its own patches; everything
 * else in the runtime is game-independent.
 *
 * The game also provides <port_types.h>, which pulls in its basic types (u8..u64, f32, sb32).
 */
#ifndef PORT_GAME_H
#define PORT_GAME_H

#include <port_types.h>

/* The game's entry point after the boot code: creates its first thread(s) and returns. */
void port_game_main(void);

/* The buffer the game's audio heap lives in. Audio command lists address it by offset. */
u8 *port_game_audio_heap(u32 *size);

/*
 * Asset data, for games whose assets are compiled natively rather than loaded from the ROM.
 *
 * port_asset_elem_size: integer element size (1, 2 or 4) the asset source declares for the
 * object `ptr` points into; 0 for anything else. Pixel and palette data is big-endian bytes on
 * the N64, so on a little-endian host the renderer has to know whether it reads bytes or
 * host-order words.
 *
 * port_asset_describe: debug aid, names the asset file / N64 offset behind a pointer
 * (FALSE if it is not asset data).
 */
u32 port_asset_elem_size(const void *ptr);
sb32 port_asset_describe(const void *ptr, u32 *file_id, u32 *offset);

#endif /* PORT_GAME_H */
