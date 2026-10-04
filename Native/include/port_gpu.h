/*
 * GPU backend interface of the display list interpreter (port_gfx.c built with PORT_GFX_GPU).
 *
 * The interpreter keeps doing what is the same on every machine: walking the display list,
 * the matrix stack, transform and lighting, decoding N64 textures. What it hands over is
 * what a GPU is good at: clip-space triangles with a colour and a texture coordinate, plus
 * the N64 pixel pipeline state reduced to a small description. One backend per graphics API
 * implements these functions (runtime/host/port_gpu_gx.c for GameCube / Wii).
 *
 * Only plain C types cross this boundary (it sits between guest and host code).
 */
#ifndef PORT_GPU_H
#define PORT_GPU_H

#ifdef __cplusplus
extern "C" {
#endif

/* Inputs of the colour combiner, (a - b) * c + d per cycle. */
enum
{
    PORT_GPU_IN_ZERO,
    PORT_GPU_IN_ONE,
    PORT_GPU_IN_COMBINED, /* result of the previous cycle */
    PORT_GPU_IN_TEXEL,
    PORT_GPU_IN_PRIM,
    PORT_GPU_IN_SHADE,    /* interpolated vertex colour */
    PORT_GPU_IN_ENV,
    /* alpha of the above used as a colour (colour `c` input only) */
    PORT_GPU_IN_COMBINED_A,
    PORT_GPU_IN_TEXEL_A,
    PORT_GPU_IN_PRIM_A,
    PORT_GPU_IN_SHADE_A,
    PORT_GPU_IN_ENV_A
};

enum { PORT_GPU_WRAP_REPEAT, PORT_GPU_WRAP_MIRROR, PORT_GPU_WRAP_CLAMP };
enum { PORT_GPU_CULL_NONE, PORT_GPU_CULL_BACK, PORT_GPU_CULL_FRONT }; /* front faces are counter-clockwise */

typedef struct PortGpuCycle
{
    unsigned char a, b, c, d;     /* colour */
    unsigned char aa, ab, ac, ad; /* alpha (only ZERO..ENV) */
} PortGpuCycle;

typedef struct PortGpuState
{
    unsigned char cycles; /* 1 or 2 */
    PortGpuCycle cycle[2];
    unsigned char prim[4], env[4]; /* RGBA */
    unsigned char fog[4];          /* fog_blend: fog RGB laid over the result by fog alpha */
    unsigned char fog_blend;

    unsigned int texture;          /* handle from port_gpu_texture_*; 0 = none */
    unsigned char wrap_s, wrap_t;
    unsigned char filter;          /* 0 = nearest, 1 = bilinear */

    unsigned char cull;
    unsigned char z_test, z_write;
    unsigned char blend;           /* 0 = opaque, 1 = source alpha over destination */
    unsigned char alpha_test;      /* discard pixels whose alpha is below 8 / 255 */

    /* Depth of the vertices this state is drawn with: `ortho` when w is constant, otherwise
     * clip z = depth_k1 * w + depth_k2 (true for any perspective projection). Backends whose
     * API takes full clip-space positions can ignore these. */
    unsigned char ortho;
    float depth_k1, depth_k2;
} PortGpuState;

typedef struct PortGpuVtx
{
    float x, y, z, w;       /* clip space, N64 conventions: y up, z in -w..w */
    float s, t;             /* 0..1 across the texture */
    unsigned char r, g, b, a;
} PortGpuVtx;

/* Frame bracket: everything between is drawn to the backend's render target. */
void port_gpu_frame_begin(void);
void port_gpu_frame_end(void);
/* Called on the host's own thread after the game's threads have run for a frame. Drawing
 * happens on a game thread; a backend whose API is tied to "the thread that draws" hands it
 * back to the host here. */
void port_gpu_host_idle(void);

/* In N64 screen pixels (320 x 240, y down); scissor max is exclusive. */
void port_gpu_viewport(float x, float y, float width, float height);
void port_gpu_scissor(int x0, int y0, int x1, int y1);

/* Textures are identified by a hash of their N64 source data. Find returns 0 when the backend
 * does not hold it (any more); Create takes width * height RGBA8 pixels, row-major. */
unsigned int port_gpu_texture_find(unsigned int hash);
/* Whether a handle from Find / Create still refers to the texture with this hash (the backend
 * may drop textures at any time to make room). */
int port_gpu_texture_valid(unsigned int handle, unsigned int hash);
unsigned int port_gpu_texture_create(unsigned int hash, int width, int height, const unsigned char *rgba);

/* count is 3 (triangle) or 4 (triangle fan). */
void port_gpu_draw(const PortGpuState *state, const PortGpuVtx *vtx, int count);
/* Set the depth buffer to "farthest" inside the rectangle (N64 screen pixels). */
void port_gpu_clear_depth(int x0, int y0, int x1, int y1);

#ifdef __cplusplus
}
#endif

#endif /* PORT_GPU_H */
