/*
 * Null GPU backend: accepts everything and draws nothing. It lets the GPU path of the display
 * list interpreter (texture decoding, state reduction, vertex output) run on hosts that have no
 * real backend yet, under their debuggers and soak tests.
 */
#if defined(PORT_GFX_GPU) && !defined(GEKKO)
#include "port_host.h"
#include "port_gpu.h"

static unsigned int sTextures, sDraws;

void port_gpu_frame_begin(void)
{
}

void port_gpu_frame_end(void)
{
    if (gPortVerbose)
    {
        port_log("gpu(null): frame end, %u draws, %u textures so far", sDraws, sTextures);
    }
    sDraws = 0;
}

void port_gpu_host_idle(void)
{
}

void port_gpu_viewport(float x, float y, float width, float height)
{
}

void port_gpu_scissor(int x0, int y0, int x1, int y1)
{
}

int port_gpu_texture_valid(unsigned int handle, unsigned int hash)
{
    return 0;
}

unsigned int port_gpu_texture_find(unsigned int hash)
{
    return 0; /* nothing is kept: every texture is decoded again, which exercises the decoder */
}

unsigned int port_gpu_texture_create(unsigned int hash, int width, int height, const unsigned char *rgba)
{
    sTextures++;
    return 1;
}

void port_gpu_draw(const PortGpuState *state, const PortGpuVtx *vtx, int count)
{
    sDraws++;
}

void port_gpu_clear_depth(int x0, int y0, int x1, int y1)
{
}

#endif /* PORT_GFX_GPU && !GEKKO */
