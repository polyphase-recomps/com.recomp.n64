/*
 * GPU path of the display list interpreter. Included by port_gfx.c when it is built with
 * PORT_GFX_GPU: triangles and rectangles go to a GPU backend (port_gpu.h) instead of the
 * software rasteriser, with the N64 pixel pipeline state reduced to a PortGpuState. The
 * state is derived exactly the way the software rasteriser reads it (gfx_combine, gfx_plot),
 * so both paths are meant to produce the same picture.
 */
#include <port_gpu.h>

#define GPU_TEX_MAX 1024       /* largest decoded texture edge (wide sprites: a 300 x 16 line of text) */
#define GPU_TEX_TEXELS 0x10000 /* largest decoded texture area */
#define GPU_BAKE_MAX 256       /* largest extent decoded with its repeats baked in */

/* Bumped whenever texture memory or a tile descriptor changes. */
static u32 sTexGen = 1; /* (tentatively declared in port_gfx.c) */

typedef struct GpuTileTex
{
    u32 gen;      /* sTexGen this entry was made for */
    u32 handle;
    u32 hash;
    s32 width, height;
    u8 wrap_s, wrap_t;
} GpuTileTex;

static GpuTileTex sGpuTileTex[8];

#define GPU_ABS(v) (((v) < 0.0F) ? -(v) : (v))

/*
 * Depth description of the vertices in sVtx (see PortGpuState), captured when they are loaded.
 * A perspective projection gives clip z = k1 * w + k2. A matrix the game has written elements
 * into (gSPInsertMatrix: billboards, screen-facing effects) need not have that form; such
 * vertices are drawn pre-divided (sVtxDivide), which is exact for any matrix as long as the
 * triangle is in front of the eye.
 */
static u8 sVtxOrtho = TRUE, sVtxDivide = FALSE;
static f32 sVtxDepthK1, sVtxDepthK2;

static void gpu_vertices_loaded(void)
{
    s32 i, major = 0;
    f32 k1;

    sVtxDivide = FALSE;
    if (sMP[0][3] == 0.0F && sMP[1][3] == 0.0F && sMP[2][3] == 0.0F)
    {
        sVtxOrtho = TRUE;
        sVtxDepthK1 = sVtxDepthK2 = 0.0F;
        return;
    }
    for (i = 1; i < 3; i++)
    {
        if (GPU_ABS(sMP[i][3]) > GPU_ABS(sMP[major][3])) major = i;
    }
    k1 = sMP[major][2] / sMP[major][3];
    for (i = 0; sMPInserted && i < 3; i++)
    {
        f32 want = k1 * sMP[i][3], diff = sMP[i][2] - want;

        if (GPU_ABS(diff) > 0.002F * (GPU_ABS(sMP[major][2]) + GPU_ABS(sMP[major][3])) + 1.0e-6F)
        {
            sVtxDivide = TRUE;
        }
    }
    sVtxOrtho = FALSE;
    sVtxDepthK1 = k1;
    sVtxDepthK2 = sMP[3][2] - k1 * sMP[3][3];
}

/* FNV-style hash, a 32-bit word at a time where the data allows (texture memory is 8-byte
 * aligned): every texture use that is not cached hashes its whole source, so this is hot. */
static u32 gpu_hash(u32 hash, const void *data, u32 bytes)
{
    const u8 *p = data;
    u32 i = 0;

    if (((uintptr_t)p & 3) == 0)
    {
        const u32 *w = (const u32 *)p;

        for (; i + 4 <= bytes; i += 4)
        {
            hash = (hash ^ *w++) * 16777619u;
        }
    }
    for (; i < bytes; i++)
    {
        hash = (hash ^ p[i]) * 16777619u;
    }
    return hash;
}

/*
 * Texture size and wrapping of a tile, the way gfx_sample() / tex_coord() address it: the
 * coordinate is clamped to the tile extent first (when clamping is on), then wrapped or
 * mirrored by the mask. So a clamped tile whose extent is larger than its mask still repeats
 * inside the extent - a face whose second eye is the mirror image of the first, an emblem made
 * of a half and its mirror image - and only clamps beyond it. Such a tile is decoded at its
 * full extent (the repeats baked in) and clamped. Only when the extent is too large for a
 * texture is it given to the backend as a repeating / mirrored texture of mask size, which
 * drops the far clamp (a floor texture tiled across a huge tile).
 */
static void gpu_tile_extent(const PortTile *tile, s32 *size, u8 *wrap, u32 mask, u32 mode, f32 lo, f32 hi)
{
    s32 span = (s32)(hi - lo) + 1;

    if (mask != 0 && (!(mode & G_TX_CLAMP) || span > GPU_BAKE_MAX))
    {
        *size = 1 << mask;
        *wrap = (mode & G_TX_MIRROR) ? PORT_GPU_WRAP_MIRROR : PORT_GPU_WRAP_REPEAT;
    }
    else
    {
        *size = span;
        *wrap = PORT_GPU_WRAP_CLAMP;
    }
    if (*size < 1) *size = 1;
    if (*size > GPU_TEX_MAX) *size = GPU_TEX_MAX;
}

/* The backend texture for what tile `tile_id` currently shows; decoded on first use. */
static const GpuTileTex *gpu_tile_texture(s32 tile_id)
{
    static u8 sPixels[GPU_TEX_TEXELS * 4];
    PortTile *tile = &sTiles[tile_id & 7];
    GpuTileTex *tex = &sGpuTileTex[tile_id & 7];
    u32 hash, base, bytes, stride;
    s32 s, t;

    unsigned long long t0;

    if (tex->gen == sTexGen && port_gpu_texture_valid(tex->handle, tex->hash))
    {
        return tex;
    }
    t0 = gPortVerbose ? port_ticks() : 0;
    gpu_tile_extent(tile, &tex->width, &tex->wrap_s, tile->masks, tile->cms, tile->sl, tile->sh);
    gpu_tile_extent(tile, &tex->height, &tex->wrap_t, tile->maskt, tile->cmt, tile->tl, tile->th);
    if (tex->width * tex->height > GPU_TEX_TEXELS)
    {
        tex->height = GPU_TEX_TEXELS / tex->width;
    }

    /* Identify the texture by the bytes it is decoded from: its TMEM rows, the palette for
     * colour-indexed formats, and the tile fields that change the decoding. */
    stride = tile->line * 8 * ((tile->siz == G_IM_SIZ_32b) ? 2 : 1);
    base = tile->tmem * 8;
    bytes = stride * (u32)((tile->maskt != 0 && tex->height > (1 << tile->maskt)) ? (1 << tile->maskt) : tex->height);
    if (base > TMEM_SIZE) base = TMEM_SIZE;
    if (bytes > TMEM_SIZE - base) bytes = TMEM_SIZE - base;
    if (!tmem_region_sig(base, base + bytes, &hash))
    {
        hash = gpu_hash(2166136261u, &sTmem[base], bytes);
    }
    if (tile->fmt == G_IM_FMT_CI)
    {
        u32 pal = (tile->siz == G_IM_SIZ_4b) ? 0x800 + tile->palette * 32 : 0x800;
        u32 pal_bytes = (tile->siz == G_IM_SIZ_4b) ? 32 : 512, pal_sig;

        if (tmem_region_sig(pal, pal + pal_bytes, &pal_sig))
        {
            hash = tmem_mix(hash, pal_sig);
        }
        else
        {
            hash = gpu_hash(hash, &sTmem[pal], pal_bytes);
        }
    }
    {
        u8 key[10];

        key[0] = tile->fmt; key[1] = tile->siz; key[2] = (u8)tex->width; key[3] = (u8)tex->height;
        key[4] = (u8)(tex->width >> 8) | (u8)((tex->height >> 8) << 4);
        key[5] = sTmemShuffled[tile->tmem & 511]; key[6] = tile->masks; key[7] = tile->maskt;
        key[8] = (u8)(tex->width >> 8); key[9] = (u8)(tex->height >> 8);
        hash = gpu_hash(hash, key, sizeof(key));
    }
    if (hash == 0)
    {
        hash = 1;
    }
    tex->gen = sTexGen;
    tex->hash = hash;
    tex->handle = port_gpu_texture_find(hash);
    if (gPortVerbose)
    {
        gPortGfxProfile[2] += port_ticks() - t0;
        gPortGfxProfile[6]++;
    }
    if (tex->handle != 0)
    {
        return tex;
    }
    for (t = 0; t < tex->height; t++)
    {
        for (s = 0; s < tex->width; s++)
        {
            PortColor c;
            u8 *dst = &sPixels[(t * tex->width + s) * 4];

            gfx_sample_tile(&c, tile_id, s, t);
            dst[0] = (u8)c.r; dst[1] = (u8)c.g; dst[2] = (u8)c.b; dst[3] = (u8)c.a;
        }
    }
    tex->handle = port_gpu_texture_create(hash, tex->width, tex->height, sPixels);
    return tex;
}

static u8 gpu_color_u8(f32 v)
{
    return (v <= 0.0F) ? 0 : (v >= 255.0F) ? 255 : (u8)v;
}

static void gpu_color4(u8 *out, const PortColor *c)
{
    out[0] = gpu_color_u8(c->r); out[1] = gpu_color_u8(c->g); out[2] = gpu_color_u8(c->b); out[3] = gpu_color_u8(c->a);
}

/* N64 combiner selectors -> backend inputs, as combine_cycle() reads them. */
static u8 gpu_cc_abd(u32 sel, u32 limit, sb32 has_tex)
{
    static const u8 map[7] = { PORT_GPU_IN_COMBINED, PORT_GPU_IN_TEXEL, PORT_GPU_IN_TEXEL, PORT_GPU_IN_PRIM,
                               PORT_GPU_IN_SHADE, PORT_GPU_IN_ENV, PORT_GPU_IN_ONE };
    u8 in = (sel < limit) ? map[sel] : PORT_GPU_IN_ZERO;

    return (in == PORT_GPU_IN_TEXEL && !has_tex) ? PORT_GPU_IN_ONE : in;
}

static u8 gpu_cc_c(u32 sel, sb32 has_tex)
{
    switch (sel)
    {
    case 0: return PORT_GPU_IN_COMBINED;
    case 1: case 2: return has_tex ? PORT_GPU_IN_TEXEL : PORT_GPU_IN_ONE;
    case 3: return PORT_GPU_IN_PRIM;
    case 4: return PORT_GPU_IN_SHADE;
    case 5: return PORT_GPU_IN_ENV;
    case 7: return PORT_GPU_IN_COMBINED_A;
    case 8: case 9: return has_tex ? PORT_GPU_IN_TEXEL_A : PORT_GPU_IN_ONE;
    case 10: return PORT_GPU_IN_PRIM_A;
    case 11: return PORT_GPU_IN_SHADE_A;
    case 12: return PORT_GPU_IN_ENV_A;
    default: return PORT_GPU_IN_ZERO;
    }
}

static u8 gpu_ca(u32 sel, sb32 is_c, sb32 has_tex)
{
    static const u8 map[8] = { PORT_GPU_IN_COMBINED, PORT_GPU_IN_TEXEL, PORT_GPU_IN_TEXEL, PORT_GPU_IN_PRIM,
                               PORT_GPU_IN_SHADE, PORT_GPU_IN_ENV, PORT_GPU_IN_ONE, PORT_GPU_IN_ZERO };
    u8 in;

    if (is_c && (sel == 0 || sel == 6 || sel == 7))
    {
        return PORT_GPU_IN_ZERO;
    }
    in = map[sel & 7];
    return (in == PORT_GPU_IN_TEXEL && !has_tex) ? PORT_GPU_IN_ONE : in;
}

static void gpu_cycle(PortGpuCycle *out, sb32 has_tex, sb32 first, u32 a, u32 b, u32 c, u32 d, u32 aa, u32 ab, u32 ac, u32 ad)
{
    u8 *in = &out->a;
    s32 i;

    out->a = gpu_cc_abd(a, 7, has_tex);
    out->b = gpu_cc_abd(b, 6, has_tex);
    out->c = gpu_cc_c(c, has_tex);
    out->d = gpu_cc_abd(d, 7, has_tex);
    out->aa = gpu_ca(aa, FALSE, has_tex);
    out->ab = gpu_ca(ab, FALSE, has_tex);
    out->ac = gpu_ca(ac, TRUE, has_tex);
    out->ad = gpu_ca(ad, FALSE, has_tex);
    if (first)
    {
        /* Nothing has been combined yet in the first cycle. */
        for (i = 0; i < 8; i++)
        {
            if (in[i] == PORT_GPU_IN_COMBINED || in[i] == PORT_GPU_IN_COMBINED_A)
            {
                in[i] = PORT_GPU_IN_ZERO;
            }
        }
    }
}

/* Pixel pipeline state for geometry drawn through the combiner (gfx_combine + gfx_plot). */
static void gpu_state_combined(PortGpuState *st, const GpuTileTex *tex)
{
    u32 w0 = sCombineW0, w1 = sCombineW1;
    sb32 two = (sOtherModeH & (3 << G_MDSFT_CYCLETYPE)) == G_CYC_2CYCLE;
    sb32 has_tex = (tex != NULL);

    port_memset(st, 0, sizeof(*st));
    st->cycles = two ? 2 : 1;
    gpu_cycle(&st->cycle[0], has_tex, TRUE,
              (w0 >> 20) & 0xF, (w1 >> 28) & 0xF, (w0 >> 15) & 0x1F, (w1 >> 15) & 7,
              (w0 >> 12) & 7, (w1 >> 12) & 7, (w0 >> 9) & 7, (w1 >> 9) & 7);
    if (two)
    {
        gpu_cycle(&st->cycle[1], has_tex, FALSE,
                  (w0 >> 5) & 0xF, (w1 >> 24) & 0xF, w0 & 0x1F, (w1 >> 6) & 7,
                  (w1 >> 21) & 7, (w1 >> 3) & 7, (w1 >> 18) & 7, w1 & 7);
    }
    gpu_color4(st->prim, &sPrim);
    gpu_color4(st->env, &sEnv);
    gpu_color4(st->fog, &sFog);
    st->fog_blend = two && (sOtherModeL & 0xCCCC0000) == 0xC4000000 && sFog.a > 0.0F;
    if (has_tex)
    {
        st->texture = tex->handle;
        st->wrap_s = tex->wrap_s;
        st->wrap_t = tex->wrap_t;
        st->filter = ((sOtherModeH & (3 << G_MDSFT_TEXTFILT)) != G_TF_POINT);
    }
    st->alpha_test = (sOtherModeL & (CVG_X_ALPHA | ALPHA_CVG_SEL | 3)) != 0;
    st->blend = (sOtherModeL & FORCE_BL) != 0;
    st->ortho = TRUE;
}

static void gpu_set_scissor(void)
{
    port_gpu_scissor(sScissor[0], sScissor[1], sScissor[2], sScissor[3]);
}

/*
 * Everything a triangle's state depends on. Triangles come in long runs with the same state,
 * and building a PortGpuState (combiner decoding, colour conversion) for each one costs more
 * than the rest of the triangle on slow CPUs, so it is rebuilt only when this changes.
 */
typedef struct GpuStateKey
{
    u32 combine0, combine1, mode_h, mode_l, geometry, texture, tex_gen, tex_handle;
    PortColor prim, env, fog;
    u32 vtx_ortho;
    f32 k1, k2;
} GpuStateKey;

static GpuStateKey sGpuKey;
static PortGpuState sGpuState;
static sb32 sGpuStateOk;
static f32 sGpuTexMul[2], sGpuTexAdd[2]; /* s = v->s * mul + add (tile shift, origin, size) */

static f32 gpu_shift_scale(u32 shift)
{
    if (shift == 0) return 1.0F;
    return (shift <= 10) ? 1.0F / (f32)(1 << shift) : (f32)(1 << (16 - shift));
}

static void gpu_triangle(s32 i0, s32 i1, s32 i2)
{
    const GpuTileTex *tex = sTextureOn ? gpu_tile_texture(sTextureTile) : NULL;
    const s32 index[3] = { i0, i1, i2 };
    GpuStateKey key;
    PortGpuVtx out[3];
    const u32 *ka, *kb;
    u32 n;
    s32 i;

    if ((sGeometryMode & G_CULL_BACK) && (sGeometryMode & G_CULL_FRONT))
    {
        return;
    }
    key.combine0 = sCombineW0; key.combine1 = sCombineW1;
    key.mode_h = sOtherModeH; key.mode_l = sOtherModeL;
    key.geometry = sGeometryMode;
    key.texture = (sTextureOn << 8) | sTextureTile;
    key.tex_gen = (tex != NULL) ? sTexGen : 0;
    key.tex_handle = (tex != NULL) ? tex->handle : 0;
    key.prim = sPrim; key.env = sEnv; key.fog = sFog;
    key.vtx_ortho = sVtxOrtho | (sVtxDivide << 1);
    key.k1 = sVtxDepthK1; key.k2 = sVtxDepthK2;
    ka = (const u32 *)&key;
    kb = (const u32 *)&sGpuKey;
    for (n = 0; n < sizeof(key) / 4 && ka[n] == kb[n]; n++)
    {
    }
    if (!sGpuStateOk || n != sizeof(key) / 4)
    {
        PortGpuState *st = &sGpuState;

        gpu_state_combined(st, tex);
        st->cull = (sGeometryMode & G_CULL_BACK) ? PORT_GPU_CULL_BACK : (sGeometryMode & G_CULL_FRONT) ? PORT_GPU_CULL_FRONT : PORT_GPU_CULL_NONE;
        if (sGeometryMode & G_ZBUFFER)
        {
            st->z_test = (sOtherModeL & Z_CMP) != 0;
            st->z_write = (sOtherModeL & Z_UPD) != 0;
            st->decal = st->z_test && (sOtherModeL & ZMODE_DEC) == ZMODE_DEC;
        }
        st->ortho = sVtxDivide ? TRUE : sVtxOrtho;
        st->depth_k1 = sVtxDepthK1;
        st->depth_k2 = sVtxDepthK2;
        if (tex != NULL)
        {
            const PortTile *tile = &sTiles[sTextureTile & 7];

            sGpuTexMul[0] = gpu_shift_scale(tile->shifts) / (f32)tex->width;
            sGpuTexAdd[0] = -tile->sl / (f32)tex->width;
            sGpuTexMul[1] = gpu_shift_scale(tile->shiftt) / (f32)tex->height;
            sGpuTexAdd[1] = -tile->tl / (f32)tex->height;
        }
        sGpuKey = key;
        sGpuStateOk = TRUE;
    }
    if (sVtxDivide && (sVtx[i0].w <= 0.0F || sVtx[i1].w <= 0.0F || sVtx[i2].w <= 0.0F))
    {
        return; /* would need clipping against the eye plane */
    }

    for (i = 0; i < 3; i++)
    {
        const PortVtx *v = &sVtx[index[i]];

        out[i].x = v->x; out[i].y = v->y; out[i].z = v->z; out[i].w = v->w;
        if (tex != NULL)
        {
            out[i].s = v->s * sGpuTexMul[0] + sGpuTexAdd[0];
            out[i].t = v->t * sGpuTexMul[1] + sGpuTexAdd[1];
        }
        else
        {
            out[i].s = out[i].t = 0.0F;
        }
        out[i].r = gpu_color_u8(v->r); out[i].g = gpu_color_u8(v->g);
        out[i].b = gpu_color_u8(v->b); out[i].a = gpu_color_u8(v->a);
    }
    port_gpu_viewport(sVpTrans[0] - sVpScale[0], sVpTrans[1] - sVpScale[1], sVpScale[0] * 2.0F, sVpScale[1] * 2.0F);
    gpu_set_scissor();
    port_gpu_draw(&sGpuState, out, 3);
    sStatTris++;
}

/* A screen rectangle (N64 pixels) as a quad in clip space, texture coordinates per corner. */
static void gpu_rect(PortGpuState *st, f32 x0, f32 y0, f32 x1, f32 y1, f32 s0, f32 t0, f32 s1, f32 t1, sb32 is_flip,
                     const PortColor *color)
{
    PortGpuVtx v[4];
    s32 i;

    for (i = 0; i < 4; i++)
    {
        sb32 right = (i == 1 || i == 2), bottom = (i >= 2);
        f32 s = right ? s1 : s0, t = bottom ? t1 : t0;

        v[i].x = (right ? x1 : x0) / (FB_W / 2.0F) - 1.0F;
        v[i].y = 1.0F - (bottom ? y1 : y0) / (FB_H / 2.0F);
        v[i].z = 0.0F;
        v[i].w = 1.0F;
        if (is_flip)
        {
            /* s runs down the screen and t across it */
            s = bottom ? s1 : s0;
            t = right ? t1 : t0;
        }
        v[i].s = s; v[i].t = t;
        gpu_color4(&v[i].r, color);
    }
    st->cull = PORT_GPU_CULL_NONE;
    st->z_test = st->z_write = 0;
    st->ortho = TRUE;
    port_gpu_viewport(0.0F, 0.0F, (f32)FB_W, (f32)FB_H);
    gpu_set_scissor();
    port_gpu_draw(st, v, 4);
}

static void gpu_fill_rect(u32 w0, u32 w1)
{
    static const PortColor white = { 255.0F, 255.0F, 255.0F, 255.0F };
    s32 x1 = ((w0 >> 12) & 0xFFF) >> 2, y1 = (w0 & 0xFFF) >> 2;
    s32 x0 = ((w1 >> 12) & 0xFFF) >> 2, y0 = (w1 & 0xFFF) >> 2;
    u32 cycle = sOtherModeH & (3 << G_MDSFT_CYCLETYPE);
    PortGpuState st;

    if (cycle == G_CYC_FILL || cycle == G_CYC_COPY)
    {
        x1++; y1++;
    }
    if (gfx_is_offscreen())
    {
        /* Filling the depth image: clear the depth buffer. */
        if (x0 < sScissor[0]) x0 = sScissor[0]; if (y0 < sScissor[1]) y0 = sScissor[1];
        if (x1 > sScissor[2]) x1 = sScissor[2]; if (y1 > sScissor[3]) y1 = sScissor[3];
        port_gpu_clear_depth(x0, y0, x1, y1);
        return;
    }
    if (cycle == G_CYC_FILL)
    {
        PortColor fill = sFill;

        port_memset(&st, 0, sizeof(st));
        st.cycles = 1;
        st.cycle[0].d = PORT_GPU_IN_SHADE;
        st.cycle[0].ad = PORT_GPU_IN_ONE;
        fill.a = 255.0F;
        gpu_rect(&st, (f32)x0, (f32)y0, (f32)x1, (f32)y1, 0.0F, 0.0F, 0.0F, 0.0F, FALSE, &fill);
    }
    else
    {
        gpu_state_combined(&st, NULL);
        gpu_rect(&st, (f32)x0, (f32)y0, (f32)x1, (f32)y1, 0.0F, 0.0F, 0.0F, 0.0F, FALSE, &white);
    }
    sStatRects++;
}

static void gpu_tex_rect(u32 w0, u32 w1, sb32 is_flip)
{
    static const PortColor white = { 255.0F, 255.0F, 255.0F, 255.0F };
    f32 x1 = ((w0 >> 12) & 0xFFF) / 4.0F, y1 = (w0 & 0xFFF) / 4.0F;
    f32 x0 = ((w1 >> 12) & 0xFFF) / 4.0F, y0 = (w1 & 0xFFF) / 4.0F;
    s32 tile_id = (w1 >> 24) & 7;
    const PortTile *tile = &sTiles[tile_id];
    f32 s0 = (s16)(sRdpHalf1 >> 16) / 32.0F, t0 = (s16)(sRdpHalf1 & 0xFFFF) / 32.0F;
    f32 dsdx = (s16)(sRdpHalf2 >> 16) / 1024.0F, dtdy = (s16)(sRdpHalf2 & 0xFFFF) / 1024.0F;
    u32 cycle = sOtherModeH & (3 << G_MDSFT_CYCLETYPE);
    const GpuTileTex *tex;
    PortGpuState st;
    f32 s1, t1;

    if (gfx_is_offscreen())
    {
        return;
    }
    tex = gpu_tile_texture(tile_id);
    if (cycle == G_CYC_COPY)
    {
        dsdx /= 4.0F;
        x1 += 1.0F; y1 += 1.0F;
        /* Copy mode: the texel as it is, cut out by its alpha when alpha compare is on. */
        port_memset(&st, 0, sizeof(st));
        st.cycles = 1;
        st.cycle[0].d = PORT_GPU_IN_TEXEL;
        st.alpha_test = (sOtherModeL & 3) != 0;
        st.cycle[0].ad = st.alpha_test ? PORT_GPU_IN_TEXEL : PORT_GPU_IN_ONE;
        st.texture = tex->handle;
        st.wrap_s = tex->wrap_s;
        st.wrap_t = tex->wrap_t;
    }
    else
    {
        gpu_state_combined(&st, tex);
    }
    /* The software rasteriser samples at whole pixels starting at the truncated corner. */
    x0 = (f32)(s32)x0; y0 = (f32)(s32)y0; x1 = (f32)(s32)x1; y1 = (f32)(s32)y1;
    if (is_flip)
    {
        f32 s_start = t0, t_start = s0;

        s0 = s_start; t0 = t_start;
        s1 = s0 + (y1 - y0) * dsdx;
        t1 = t0 + (x1 - x0) * dtdy;
    }
    else
    {
        s1 = s0 + (x1 - x0) * dsdx;
        t1 = t0 + (y1 - y0) * dtdy;
    }
    gpu_rect(&st, x0, y0, x1, y1,
             (tex_shift(s0, tile->shifts) - tile->sl) / (f32)tex->width, (tex_shift(t0, tile->shiftt) - tile->tl) / (f32)tex->height,
             (tex_shift(s1, tile->shifts) - tile->sl) / (f32)tex->width, (tex_shift(t1, tile->shiftt) - tile->tl) / (f32)tex->height,
             is_flip, &white);
    sStatRects++;
}
