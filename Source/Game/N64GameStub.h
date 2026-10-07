/**
 * @file N64GameStub.h
 * @brief Stand-in for a game library on platforms it has not been built for: no-op versions
 *        of N64GameApi.h, so the project links and the player node shows nothing.
 *
 * Included by one .cpp of a game package (the template's Source/N64GameStub.cpp).
 */
#pragma once

#include "Game/N64GameApi.h"

#if !N64_HAS_NATIVE_LIB

extern "C" {

void port_set_log_sink(void (*)(const char*)) {}
void port_set_fault_containment(int) {}
void port_set_development(int) {}

void n64_set_save_path(const char*) {}
void n64_set_recomp_dir(const char*) {}
int n64_boot(const char*) { return 0; }
int n64_is_running(void) { return 0; }
void n64_set_pad(int, const PortPad*) {}
void n64_run_frame(void) {}
void n64_shutdown(void) {}

int n64_draws_to_screen(void) { return 0; }
void n64_set_render_scale(int) {}
int n64_render_scale(void) { return 1; }
int n64_max_render_scale(void) { return 1; }
unsigned long long n64_frame_signature(void) { return 0; }
void n64_set_skip_draw(int) {}

int n64_bridge_var_count(void) { return 0; }
const PortBridgeVar* n64_bridge_var(int) { return nullptr; }
int n64_bridge_request_count(void) { return 0; }
const PortBridgeRequest* n64_bridge_request_info(int) { return nullptr; }
int n64_bridge_get(const char*, int, double*, char*, unsigned) { return 0; }
int n64_bridge_request(const char*, const int*, int) { return 0; }
int n64_bridge_result(int, int*) { return 0; }
int n64_bridge_poll_event(char*, unsigned, int*, int, int*) { return 0; }

const unsigned char* n64_framebuffer(int* width, int* height)
{
    *width = 0;
    *height = 0;
    return nullptr;
}

const short* n64_audio(int* frames, int* sample_rate)
{
    *frames = 0;
    *sample_rate = 32000;
    return nullptr;
}

} // extern "C"

#endif // !N64_HAS_NATIVE_LIB
