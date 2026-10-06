/**
 * @file N64GameApi.h
 * @brief C API of an N64 game library (Lib/<game>.lib: the decomp build or the recompiled one,
 *        they export the same functions), as a game package's addon calls it.
 *
 * Mirrors com.recomp.n64/Native/include/port_host.h and port_bridge.h, kept separate so nothing
 * under an addon's Source/ depends on the Native/ trees' include paths. Shared by every game
 * package made from com.recomp.n64's template (Templates/game); see Game/N64GamePlayer.h.
 */
#pragma once

// Platforms a game library is built for (Native/build.*, the recomp build, and the
// nativePerPlatform entries in package.json). Elsewhere Game/N64GameStub.h stands in for it.
#if PLATFORM_WINDOWS || PLATFORM_LINUX || PLATFORM_WII || PLATFORM_GAMECUBE || PLATFORM_3DS
#define N64_HAS_NATIVE_LIB 1
#else
#define N64_HAS_NATIVE_LIB 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PortPad
{
    unsigned short buttons; // N64 button mask (N64_BTN_*)
    signed char stick_x, stick_y; // -80..80
    unsigned char connected;
} PortPad;

// N64 controller buttons
#define N64_BTN_A       0x8000
#define N64_BTN_B       0x4000
#define N64_BTN_Z       0x2000
#define N64_BTN_START   0x1000
#define N64_BTN_DUP     0x0800
#define N64_BTN_DDOWN   0x0400
#define N64_BTN_DLEFT   0x0200
#define N64_BTN_DRIGHT  0x0100
#define N64_BTN_L       0x0020
#define N64_BTN_R       0x0010
#define N64_BTN_CUP     0x0008
#define N64_BTN_CDOWN   0x0004
#define N64_BTN_CLEFT   0x0002
#define N64_BTN_CRIGHT  0x0001

void port_set_log_sink(void (*sink)(const char* line));
void port_set_fault_containment(int enable);
// In the editor: development fallbacks apply (a recompiled game boots the ROM it was built from).
void port_set_development(int on);

void n64_set_save_path(const char* path);
// Recomp (live) builds: the folder with the game's recompiler data (game.json, the N64Recomp
// config, the symbols); n64_boot recompiles the ROM from it. Other builds ignore it.
void n64_set_recomp_dir(const char* path);
int n64_boot(const char* rom_path);
int n64_is_running(void);
void n64_set_pad(int port, const PortPad* pad);
void n64_run_frame(void);
void n64_shutdown(void);
const unsigned char* n64_framebuffer(int* width, int* height);
int n64_draws_to_screen(void);
// While set, display lists are not drawn (logic still runs): all but the last of several game
// frames run in one engine frame.
void n64_set_skip_draw(int skip);
const short* n64_audio(int* frames, int* sample_rate);

#if PLATFORM_3DS
// citro3d backend: the game's draws are recorded while it runs and replayed by this from inside
// the engine's render pass, onto the target being drawn (top screen: 400 x 240).
void port_gpu_c3d_render(int screen_w, int screen_h);
#endif

// Script bridge (com.recomp.n64/Native/include/port_bridge.h): variables, requests and events
// the game and its mods publish. Main thread, between frames.
typedef struct PortBridgeVar
{
    const char* name;
    void* addr;
    int type; // PB_*
    int count;
    int stride;
    const char* help;
} PortBridgeVar;

typedef struct PortBridgeRequest
{
    const char* name;
    int (*fn)(const int* args, int nargs);
    const char* help;
} PortBridgeRequest;

enum { PB_U8 = 1, PB_S8, PB_U16, PB_S16, PB_U32, PB_S32, PB_STR, PB_F32 };
#define PB_MAX_ARGS 8

int n64_bridge_var_count(void);
const PortBridgeVar* n64_bridge_var(int index);
int n64_bridge_request_count(void);
const PortBridgeRequest* n64_bridge_request_info(int index);
int n64_bridge_get(const char* name, int index, double* value, char* text, unsigned text_cap);
int n64_bridge_request(const char* name, const int* args, int nargs);
int n64_bridge_result(int id, int* result);
int n64_bridge_poll_event(char* name, unsigned name_cap, int* args, int max_args, int* nargs);

#ifdef __cplusplus
}
#endif
