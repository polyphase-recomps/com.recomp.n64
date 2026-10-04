/*
 * F3DEX2 display list interpreter with a software rasteriser.
 *
 * Replaces the RSP graphics microcode and the RDP: walks the display list of a
 * graphics task, transforms and lights vertices, emulates TMEM texture loads,
 * and rasterises into a 320x240 RGBA8 buffer. On osViSwapBuffer the finished
 * frame is latched as the one n64_framebuffer() returns.
 *
 * First-pass accuracy: point sampling, one or two combiner cycles, alpha
 * blending and alpha test, depth buffer. No fog, LOD, coverage or dither.
 */
#include <port_types.h>
#include <PR/mbi.h>
#include <PR/gbi.h>
#include <PR/sptask.h>
#include <port_host.h>
#include <port_game.h>
#include "port_guest.h"

#define FB_W 320
#define FB_H 240
#define TMEM_SIZE 4096
#define MTX_STACK_MAX 18
#define VTX_MAX 64
#define DL_STACK_MAX 18

typedef struct PortVtx
{
    f32 x, y, z, w; /* clip space */
    f32 s, t;       /* texel units (already scaled) */
    f32 r, g, b, a; /* 0..255 */
} PortVtx;

typedef struct PortTile
{
    u8 fmt, siz, palette, cms, cmt, masks, maskt, shifts, shiftt;
    u16 line, tmem;
    f32 sl, tl, sh, th;
} PortTile;

typedef struct PortColor
{
    f32 r, g, b, a;
} PortColor;

#ifndef PORT_GFX_GPU
/* Software rasteriser targets (a GPU backend draws to its own). */
static u8 sBack[FB_W * FB_H * 4];
static u8 sFront[FB_W * FB_H * 4];
static f32 sDepth[FB_W * FB_H];
#endif
static sb32 sHasFrame;

static uintptr_t sSegments[16];
static f32 sModelview[MTX_STACK_MAX][4][4];
static s32 sModelviewTop;
static f32 sProjection[4][4];
static f32 sMP[4][4];
static u8 sMPInserted; /* the game wrote elements into sMP since it was last computed (gSPInsertMatrix) */
static PortVtx sVtx[VTX_MAX];

static f32 sVpScale[3], sVpTrans[3];
static u32 sGeometryMode, sOtherModeL, sOtherModeH;
static s32 sScissor[4]; /* x0 y0 x1 y1, exclusive max */

static s32 sLightsNum;
static f32 sLightColor[9][3];
static f32 sLightDir[9][3];

static PortColor sPrim, sEnv, sBlend, sFill, sFog;
static u32 sCombineW0, sCombineW1;

static u8 sTmem[TMEM_SIZE + 8];
/* Set per TMEM word address by a LoadBlock with dxt == 0: the RDP always swaps the two 32-bit
 * halves of each 64-bit word on odd rows when sampling. Loads normally pre-swap those rows
 * (so the two cancel), but a dxt == 0 block load does not, and such textures are stored
 * pre-shuffled in the ROM. Sampling has to undo it for those loads only. */
static u8 sTmemShuffled[512];
static PortTile sTiles[8];
static uintptr_t sTimgAddr;
static u32 sTimgFmt, sTimgSiz, sTimgWidth;
static sb32 sTextureOn;
static s32 sTextureTile;
static f32 sTextureScaleS, sTextureScaleT;

static uintptr_t sColorImage, sDepthImage;
static u32 sRdpHalf1, sRdpHalf2;

/* Debug aid: log every display list command of this frame (0 = off). */
u32 gPortGfxTraceFrame;

static u32 sStatTris, sStatRects, sStatUnknown;
static u8 sUnknownSeen[256];

extern f32 sqrtf(f32);

static s32 gfx_floor(f32 v)
{
    s32 i = (s32)v;

    return (v < (f32)i) ? i - 1 : i;
}

/* ---- addresses ------------------------------------------------------------ */
static Gfx *sCurDl; /* command being interpreted, for diagnostics */

/*
 * Display list arguments are either native pointers or N64 segmented addresses (0x0Snnnnnn).
 * On 64-bit hosts the image and the arena are placed above 4 GB, so anything smaller is
 * segmented. On 32-bit hosts the two share one range: an address is taken as segmented when
 * its segment has been set (segment 0 is the identity on the N64 too). That relies on the
 * platform not placing memory in a segment range the game uses; consoles map RAM at
 * 0x80000000 (GameCube, Wii) or 0x8C000000 (Dreamcast), well clear of it.
 */
static sb32 gfx_is_segmented(uintptr_t addr)
{
#ifdef PORT_64BIT
    return (addr >> 32) == 0;
#else
    return (addr >> 28) == 0 && sSegments[(addr >> 24) & 0xF] != 0;
#endif
}

static void *gfx_addr(uintptr_t addr)
{
    if (gfx_is_segmented(addr))
    {
        uintptr_t base = sSegments[(addr >> 24) & 0xF];

        if (base == 0)
        {
            return NULL;
        }
        return (void *)(base + (addr & 0x00FFFFFF));
    }
    return (void *)addr;
}

/* ---- matrices ----------------------------------------------------------------- */
/* In a traced frame: a checksum of the data a command read, so that two builds (other compiler,
 * other platform) can be compared input by input. */
static void gfx_trace_data(const char *what, const void *data, u32 bytes)
{
    if (gPortGfxTraceFrame != 0 && gPortFrameCount == gPortGfxTraceFrame)
    {
        const u8 *p = data;
        u32 sum = 2166136261u, i;

        for (i = 0; i < bytes; i++)
        {
            sum = (sum ^ p[i]) * 16777619u;
        }
        port_log("  %s: %u bytes, sum %08X", what, bytes, sum);
        for (i = 0; i + 16 <= bytes && bytes <= 64; i += 16)
        {
            port_log("    %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X", p[i], p[i + 1], p[i + 2],
                     p[i + 3], p[i + 4], p[i + 5], p[i + 6], p[i + 7], p[i + 8], p[i + 9], p[i + 10], p[i + 11],
                     p[i + 12], p[i + 13], p[i + 14], p[i + 15]);
        }
    }
}

static void mtx_from_fixed(f32 out[4][4], const Mtx *mtx)
{
    const u32 *words = (const u32 *)mtx;
    s32 i;

    for (i = 0; i < 16; i++)
    {
        u32 hi = words[i / 2], lo = words[8 + i / 2];
        s32 ip = (i & 1) ? (s16)(hi & 0xFFFF) : (s16)(hi >> 16);
        u32 fp = (i & 1) ? (lo & 0xFFFF) : (lo >> 16);

        out[i / 4][i % 4] = (f32)((ip * 65536) + (s32)fp) / 65536.0F;
    }
}

static void mtx_mul(f32 out[4][4], f32 a[4][4], f32 b[4][4])
{
    f32 tmp[4][4];
    s32 i, j;

    for (i = 0; i < 4; i++)
    {
        for (j = 0; j < 4; j++)
        {
            tmp[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
        }
    }
    port_memcpy(out, tmp, sizeof(tmp));
}

static void gfx_matrix(u32 w0, uintptr_t w1)
{
    u32 params = (w0 & 0xFF) ^ G_MTX_PUSH;
    Mtx *src = gfx_addr(w1);
    f32 m[4][4];

    if (src == NULL)
    {
        return;
    }
    gfx_trace_data("mtx", src, sizeof(*src));
    mtx_from_fixed(m, src);
    if (params & G_MTX_PROJECTION)
    {
        if (params & G_MTX_LOAD)
        {
            port_memcpy(sProjection, m, sizeof(m));
        }
        else
        {
            mtx_mul(sProjection, m, sProjection);
        }
    }
    else
    {
        if ((params & G_MTX_PUSH) && sModelviewTop < MTX_STACK_MAX - 1)
        {
            port_memcpy(sModelview[sModelviewTop + 1], sModelview[sModelviewTop], sizeof(m));
            sModelviewTop++;
        }
        if (params & G_MTX_LOAD)
        {
            port_memcpy(sModelview[sModelviewTop], m, sizeof(m));
        }
        else
        {
            mtx_mul(sModelview[sModelviewTop], m, sModelview[sModelviewTop]);
        }
    }
    mtx_mul(sMP, sModelview[sModelviewTop], sProjection); sMPInserted = FALSE;
}

/* ---- vertices ------------------------------------------------------------------- */
#ifdef PORT_GFX_GPU
static void gpu_vertices_loaded(void);
static u32 sTexGen;
#endif
static void gfx_vertices(u32 w0, uintptr_t w1)
{
    s32 count = (w0 >> 12) & 0xFF;
    s32 first = ((w0 >> 1) & 0x7F) - count;
    Vtx *src = gfx_addr(w1);
    f32 (*mv)[4] = sModelview[sModelviewTop];
    s32 i, l;

    if (src == NULL || first < 0 || first + count > VTX_MAX)
    {
        return;
    }
#ifdef PORT_64BIT
    if (((uintptr_t)src >> 32) == 0)
    {
        u32 file_id = 0, offset = 0;

        port_asset_describe(sCurDl, &file_id, &offset);
        port_log("gfx: bad vertex address %p (w1 %p) in dl %p (file %u offset 0x%X) frame %u", src, (void *)w1, sCurDl,
                 file_id, offset / 2, gPortFrameCount);
        return;
    }
#endif
    gfx_trace_data("vtx", src, (u32)count * sizeof(*src));
    for (i = 0; i < count; i++, src++)
    {
        PortVtx *v = &sVtx[first + i];
        f32 x = src->v.ob[0], y = src->v.ob[1], z = src->v.ob[2];

        v->x = x * sMP[0][0] + y * sMP[1][0] + z * sMP[2][0] + sMP[3][0];
        v->y = x * sMP[0][1] + y * sMP[1][1] + z * sMP[2][1] + sMP[3][1];
        v->z = x * sMP[0][2] + y * sMP[1][2] + z * sMP[2][2] + sMP[3][2];
        v->w = x * sMP[0][3] + y * sMP[1][3] + z * sMP[2][3] + sMP[3][3];
        v->s = (f32)src->v.tc[0] * sTextureScaleS / 32.0F;
        v->t = (f32)src->v.tc[1] * sTextureScaleT / 32.0F;
        v->a = src->v.cn[3];

        if (sGeometryMode & G_LIGHTING)
        {
            f32 nx = src->n.n[0], ny = src->n.n[1], nz = src->n.n[2];
            f32 tx = nx * mv[0][0] + ny * mv[1][0] + nz * mv[2][0];
            f32 ty = nx * mv[0][1] + ny * mv[1][1] + nz * mv[2][1];
            f32 tz = nx * mv[0][2] + ny * mv[1][2] + nz * mv[2][2];
            f32 len = sqrtf(tx * tx + ty * ty + tz * tz);
            f32 r = sLightColor[sLightsNum][0], g = sLightColor[sLightsNum][1], b = sLightColor[sLightsNum][2];

            if (len > 0.0F)
            {
                tx /= len; ty /= len; tz /= len;
            }
            for (l = 0; l < sLightsNum; l++)
            {
                f32 dot = tx * sLightDir[l][0] + ty * sLightDir[l][1] + tz * sLightDir[l][2];

                if (dot > 0.0F)
                {
                    r += sLightColor[l][0] * dot;
                    g += sLightColor[l][1] * dot;
                    b += sLightColor[l][2] * dot;
                }
            }
            v->r = (r > 255.0F) ? 255.0F : r;
            v->g = (g > 255.0F) ? 255.0F : g;
            v->b = (b > 255.0F) ? 255.0F : b;
            if (sGeometryMode & G_TEXTURE_GEN)
            {
                v->s = (tx * 0.5F + 0.5F) * sTextureScaleS * 2048.0F / 32.0F;
                v->t = (ty * 0.5F + 0.5F) * sTextureScaleT * 2048.0F / 32.0F;
            }
        }
        else
        {
            v->r = src->v.cn[0];
            v->g = src->v.cn[1];
            v->b = src->v.cn[2];
        }
    }
#ifdef PORT_GFX_GPU
    gpu_vertices_loaded();
#endif
}

/* ---- textures --------------------------------------------------------------------- */
/*
 * Copy texture data into TMEM, which holds it in the big-endian byte order the N64 uses.
 * Asset arrays declared u8 already are in that order; arrays declared u16 hold host-order
 * values natively and are swapped here when the texture really is 16 bits per texel.
 */
static void gfx_copy_texels(u8 *dst, const u8 *src, u32 bytes)
{
#ifdef PORT_LITTLE_ENDIAN
    if (sTimgSiz == G_IM_SIZ_16b && port_asset_elem_size(src) == 2)
    {
        u32 i;

        for (i = 0; i + 1 < bytes; i += 2)
        {
            dst[i] = src[i + 1];
            dst[i + 1] = src[i];
        }
        return;
    }
#endif
    port_memcpy(dst, src, bytes);
}

static void gfx_load_block(u32 w0, u32 w1)
{
    PortTile *tile = &sTiles[(w1 >> 24) & 7];
    u32 texels = ((w1 >> 12) & 0xFFF) + 1;
    u32 bytes = (texels << sTimgSiz) >> 1;
    u8 *src = gfx_addr(sTimgAddr);
    u32 dst = tile->tmem * 8;

    if (src == NULL || dst >= TMEM_SIZE)
    {
        return;
    }
    if (dst + bytes > TMEM_SIZE)
    {
        bytes = TMEM_SIZE - dst;
    }
    gfx_copy_texels(&sTmem[dst], src, bytes);
#ifdef PORT_GFX_GPU
    sTexGen++;
#endif
    gfx_trace_data("load block", &sTmem[dst], bytes);
    sTmemShuffled[tile->tmem & 511] = ((w1 & 0xFFF) == 0);
}

static void gfx_load_tile(u32 w0, u32 w1)
{
    PortTile *tile = &sTiles[(w1 >> 24) & 7];
    u32 uls = (w0 >> 14) & 0x3FF, ult = (w0 >> 2) & 0x3FF;
    u32 lrs = (w1 >> 14) & 0x3FF, lrt = (w1 >> 2) & 0x3FF;
    u8 *src = gfx_addr(sTimgAddr);
    u32 bpp_shift = sTimgSiz; /* bytes per texel = (1 << siz) / 2 */
    u32 row_bytes = (((lrs - uls + 1) << bpp_shift) + 1) >> 1;
    /* RGBA32 is split over both TMEM halves on hardware, so `line` counts half its bytes (see gfx_sample) */
    u32 stride = tile->line * 8 * ((tile->siz == G_IM_SIZ_32b) ? 2 : 1);
    u32 y;

    if (src == NULL)
    {
        return;
    }
    sTmemShuffled[tile->tmem & 511] = FALSE;
#ifdef PORT_GFX_GPU
    sTexGen++;
#endif
    tile->sl = uls; tile->tl = ult; tile->sh = lrs; tile->th = lrt;
    for (y = ult; y <= lrt; y++)
    {
        u32 dst = tile->tmem * 8 + (y - ult) * stride;
        u32 off = ((y * sTimgWidth + uls) << bpp_shift) >> 1;

        if (dst + row_bytes > TMEM_SIZE)
        {
            break;
        }
        gfx_copy_texels(&sTmem[dst], src + off, row_bytes);
        gfx_trace_data("load tile row", &sTmem[dst], row_bytes);
    }
}

static void gfx_load_tlut(u32 w1)
{
    PortTile *tile = &sTiles[(w1 >> 24) & 7];
    u32 count = ((w1 >> 14) & 0x3FF) + 1;
    u8 *src = gfx_addr(sTimgAddr);
    /* On hardware each palette entry takes a whole TMEM word in the upper half; here the
     * palette area is kept compact: entry n lives at 0x800 + n * 2. */
    u32 dst = 0x800 + ((tile->tmem >= 256) ? (tile->tmem - 256) * 2 : 0);
    sb32 is_bytes;
    u32 i;

    if (src == NULL)
    {
        return;
    }
    /* Palettes typed u16 in the asset data (and anything the game builds at run time) hold
     * host-order values; palettes embedded in byte arrays are big-endian already. */
#ifdef PORT_GFX_GPU
    sTexGen++;
#endif
    is_bytes = (port_asset_elem_size(src) == 1);
    for (i = 0; i < count && dst + 1 < TMEM_SIZE; i++, dst += 2)
    {
        if (is_bytes)
        {
            sTmem[dst] = src[i * 2];
            sTmem[dst + 1] = src[i * 2 + 1];
        }
        else
        {
            u16 value = ((u16 *)src)[i];

            sTmem[dst] = value >> 8;
            sTmem[dst + 1] = value & 0xFF;
        }
    }
    gfx_trace_data("load tlut", &sTmem[dst - i * 2], i * 2);
}

static void color_from_5551(PortColor *c, u32 v)
{
    c->r = (f32)(((v >> 11) & 0x1F) * 255 / 31);
    c->g = (f32)(((v >> 6) & 0x1F) * 255 / 31);
    c->b = (f32)(((v >> 1) & 0x1F) * 255 / 31);
    c->a = (v & 1) ? 255.0F : 0.0F;
}

static s32 tex_coord(s32 c, u32 mask, u32 mode, s32 size)
{
    /* As on the RDP: clamp to the tile extent first, then mirror / wrap with the mask. A clamped
     * tile with a mask still repeats inside its extent (e.g. a 16x16 texture over a 224x160 tile). */
    if ((mode & G_TX_CLAMP) || mask == 0)
    {
        if (c < 0) c = 0;
        if (c > size) c = size;
    }
    if (mask != 0)
    {
        s32 span = 1 << mask;

        if ((mode & G_TX_MIRROR) && (c & span))
        {
            c = ~c;
        }
        c &= span - 1;
    }
    return c;
}

/* The tile's coordinate shift: 1..10 divide the coordinate by 2^n, 11..15 multiply it by 2^(16-n). */
static f32 tex_shift(f32 c, u32 shift)
{
    if (shift == 0) return c;
    return (shift <= 10) ? c / (f32)(1 << shift) : c * (f32)(1 << (16 - shift));
}

/* Texel of a tile at tile coordinates (already shifted, relative to the tile's origin). */
static void gfx_sample_tile(PortColor *out, s32 tile_id, s32 s, s32 t);

static void gfx_sample(PortColor *out, s32 tile_id, f32 fs, f32 ft)
{
    PortTile *tile = &sTiles[tile_id & 7];

    gfx_sample_tile(out, tile_id, gfx_floor(tex_shift(fs, tile->shifts) - tile->sl),
                    gfx_floor(tex_shift(ft, tile->shiftt) - tile->tl));
}

static void gfx_sample_tile(PortColor *out, s32 tile_id, s32 s, s32 t)
{
    PortTile *tile = &sTiles[tile_id & 7];
    u32 base = tile->tmem * 8, stride = tile->line * 8, off, swap;
    u32 v;

    s = tex_coord(s, tile->masks, tile->cms, (s32)(tile->sh - tile->sl));
    t = tex_coord(t, tile->maskt, tile->cmt, (s32)(tile->th - tile->tl));
    if (tile->siz == G_IM_SIZ_32b)
    {
        stride *= 2; /* hardware splits RGBA32 over both TMEM halves, so `line` counts half the bytes */
    }
    off = base + t * stride;
    swap = ((t & 1) && sTmemShuffled[tile->tmem & 511]) ? 4 : 0;

    switch (tile->siz)
    {
    case G_IM_SIZ_4b:
        off += (s >> 1) ^ swap;
        v = (off < TMEM_SIZE) ? ((sTmem[off] >> ((s & 1) ? 0 : 4)) & 0xF) : 0;
        if (tile->fmt == G_IM_FMT_CI)
        {
            u32 pal = 0x800 + (tile->palette * 16 + v) * 2;
            color_from_5551(out, (sTmem[pal] << 8) | sTmem[pal + 1]);
        }
        else if (tile->fmt == G_IM_FMT_IA)
        {
            out->r = out->g = out->b = (f32)((v >> 1) * 255 / 7);
            out->a = (v & 1) ? 255.0F : 0.0F;
        }
        else
        {
            out->r = out->g = out->b = out->a = (f32)(v * 17);
        }
        break;

    case G_IM_SIZ_8b:
        off += s ^ swap;
        v = (off < TMEM_SIZE) ? sTmem[off] : 0;
        if (tile->fmt == G_IM_FMT_CI)
        {
            u32 pal = 0x800 + v * 2;
            color_from_5551(out, (sTmem[pal] << 8) | sTmem[pal + 1]);
        }
        else if (tile->fmt == G_IM_FMT_IA)
        {
            out->r = out->g = out->b = (f32)((v >> 4) * 17);
            out->a = (f32)((v & 0xF) * 17);
        }
        else
        {
            out->r = out->g = out->b = out->a = (f32)v;
        }
        break;

    case G_IM_SIZ_16b:
        off += (s * 2) ^ swap;
        v = (off + 1 < TMEM_SIZE) ? ((sTmem[off] << 8) | sTmem[off + 1]) : 0;
        if (tile->fmt == G_IM_FMT_IA)
        {
            out->r = out->g = out->b = (f32)(v >> 8);
            out->a = (f32)(v & 0xFF);
        }
        else
        {
            color_from_5551(out, v);
        }
        break;

    default: /* 32-bit RGBA */
        off += (s * 4) ^ (swap * 2); /* 32-bit texels swap in pairs too: 8 bytes */
        if (off + 3 < TMEM_SIZE)
        {
            out->r = sTmem[off]; out->g = sTmem[off + 1]; out->b = sTmem[off + 2]; out->a = sTmem[off + 3];
        }
        else
        {
            out->r = out->g = out->b = out->a = 0.0F;
        }
        break;
    }
}

/* ---- colour combiner ------------------------------------------------------------------ */
static void combine_cycle(PortColor *out, const PortColor *combined, const PortColor *tex, const PortColor *shade,
                          u32 a, u32 b, u32 c, u32 d, u32 aa, u32 ab, u32 ac, u32 ad)
{
    static const PortColor one = { 255.0F, 255.0F, 255.0F, 255.0F };
    static const PortColor zero = { 0.0F, 0.0F, 0.0F, 0.0F };
    const PortColor *rgb[8];
    const PortColor *pa, *pb, *pd;
    PortColor cc;
    f32 alpha[8], ca;

    rgb[0] = combined; rgb[1] = tex; rgb[2] = tex; rgb[3] = &sPrim; rgb[4] = shade; rgb[5] = &sEnv; rgb[6] = &one; rgb[7] = &zero;
    pa = (a < 7) ? rgb[a] : &zero;
    pb = (b < 6) ? rgb[b] : &zero;
    pd = (d < 7) ? rgb[d] : &zero;
    switch (c)
    {
    case 0: case 1: case 2: case 3: case 4: case 5: cc = *rgb[c]; break;
    case 7: cc.r = cc.g = cc.b = combined->a; break;
    case 8: case 9: cc.r = cc.g = cc.b = tex->a; break;
    case 10: cc.r = cc.g = cc.b = sPrim.a; break;
    case 11: cc.r = cc.g = cc.b = shade->a; break;
    case 12: cc.r = cc.g = cc.b = sEnv.a; break;
    default: cc.r = cc.g = cc.b = 0.0F; break;
    }
    out->r = (pa->r - pb->r) * cc.r / 255.0F + pd->r;
    out->g = (pa->g - pb->g) * cc.g / 255.0F + pd->g;
    out->b = (pa->b - pb->b) * cc.b / 255.0F + pd->b;

    alpha[0] = combined->a; alpha[1] = tex->a; alpha[2] = tex->a; alpha[3] = sPrim.a;
    alpha[4] = shade->a; alpha[5] = sEnv.a; alpha[6] = 255.0F; alpha[7] = 0.0F;
    ca = (ac == 0 || ac == 6 || ac == 7) ? 0.0F : alpha[ac];
    out->a = (alpha[aa] - alpha[ab]) * ca / 255.0F + alpha[ad];
}

static void gfx_combine(PortColor *out, const PortColor *tex, const PortColor *shade)
{
    static const PortColor zero = { 0.0F, 0.0F, 0.0F, 0.0F };
    u32 w0 = sCombineW0, w1 = sCombineW1;
    PortColor c0;

    combine_cycle(&c0, &zero, tex, shade,
                  (w0 >> 20) & 0xF, (w1 >> 28) & 0xF, (w0 >> 15) & 0x1F, (w1 >> 15) & 7,
                  (w0 >> 12) & 7, (w1 >> 12) & 7, (w0 >> 9) & 7, (w1 >> 9) & 7);
    if ((sOtherModeH & (3 << G_MDSFT_CYCLETYPE)) == G_CYC_2CYCLE)
    {
        combine_cycle(out, &c0, tex, shade,
                      (w0 >> 5) & 0xF, (w1 >> 24) & 0xF, w0 & 0x1F, (w1 >> 6) & 7,
                      (w1 >> 21) & 7, (w1 >> 3) & 7, (w1 >> 18) & 7, w1 & 7);
    }
    else
    {
        *out = c0;
    }
    /* First blender cycle of G_RM_FOG_PRIM_A (fog colour over the pixel by the fog alpha): how
     * fighters get their colour overlays (hit flashes, charging, the 1P team tints). */
    if ((sOtherModeH & (3 << G_MDSFT_CYCLETYPE)) == G_CYC_2CYCLE && (sOtherModeL & 0xCCCC0000) == 0xC4000000 &&
        sFog.a > 0.0F)
    {
        f32 k = sFog.a / 255.0F;

        out->r = sFog.r * k + out->r * (1.0F - k);
        out->g = sFog.g * k + out->g * (1.0F - k);
        out->b = sFog.b * k + out->b * (1.0F - k);
    }
}

/* ---- pixel output ---------------------------------------------------------------------- */
static sb32 gfx_is_offscreen(void)
{
    return (sColorImage != 0) && (sColorImage == sDepthImage);
}

#ifdef PORT_GFX_GPU
#include "port_gfx_gpu.h"

static void gfx_triangle(s32 i0, s32 i1, s32 i2)
{
    if (i0 >= VTX_MAX || i1 >= VTX_MAX || i2 >= VTX_MAX || gfx_is_offscreen())
    {
        return;
    }
    gpu_triangle(i0, i1, i2);
}

static void gfx_fill_rect(u32 w0, u32 w1)
{
    gpu_fill_rect(w0, w1);
}

static void gfx_tex_rect(u32 w0, u32 w1, sb32 is_flip)
{
    gpu_tex_rect(w0, w1, is_flip);
}
#else /* software rasteriser */
static u8 clamp_u8(f32 v)
{
    return (v <= 0.0F) ? 0 : (v >= 255.0F) ? 255 : (u8)v;
}

static void gfx_plot(s32 x, s32 y, const PortColor *c)
{
    u8 *dst = &sBack[(y * FB_W + x) * 4];
    f32 a = c->a;

    if ((sOtherModeL & (CVG_X_ALPHA | ALPHA_CVG_SEL | 3)) && a < 8.0F)
    {
        return; /* alpha test / coverage-from-alpha cutout */
    }
    if ((sOtherModeL & FORCE_BL) && a < 255.0F)
    {
        f32 k = a / 255.0F;

        dst[0] = clamp_u8(c->r * k + dst[0] * (1.0F - k));
        dst[1] = clamp_u8(c->g * k + dst[1] * (1.0F - k));
        dst[2] = clamp_u8(c->b * k + dst[2] * (1.0F - k));
    }
    else
    {
        dst[0] = clamp_u8(c->r);
        dst[1] = clamp_u8(c->g);
        dst[2] = clamp_u8(c->b);
    }
    dst[3] = 255;
}

/* ---- triangles ------------------------------------------------------------------------- */
typedef struct PortScreenVtx
{
    f32 x, y, z, iw;
    f32 s, t, r, g, b, a; /* pre-divided by w */
} PortScreenVtx;

static void to_screen(PortScreenVtx *out, const PortVtx *v)
{
    f32 iw = 1.0F / v->w;

    out->x = v->x * iw * sVpScale[0] + sVpTrans[0];
    out->y = -v->y * iw * sVpScale[1] + sVpTrans[1];
    out->z = v->z * iw;
    out->iw = iw;
    out->s = v->s * iw; out->t = v->t * iw;
    out->r = v->r * iw; out->g = v->g * iw; out->b = v->b * iw; out->a = v->a * iw;
}

static void raster_triangle(const PortScreenVtx *v0, const PortScreenVtx *v1, const PortScreenVtx *v2)
{
    f32 area = (v1->x - v0->x) * (v2->y - v0->y) - (v2->x - v0->x) * (v1->y - v0->y);
    f32 min_x, max_x, min_y, max_y, inv;
    s32 x0, x1, y0, y1, x, y;
    sb32 use_z = (sGeometryMode & G_ZBUFFER) != 0;
    sb32 use_tex = sTextureOn;

    if (area == 0.0F)
    {
        return;
    }
    /* Screen y points down, so front faces (counter-clockwise on N64) have negative area. */
    if ((sGeometryMode & G_CULL_BACK) && area > 0.0F) return;
    if ((sGeometryMode & G_CULL_FRONT) && area < 0.0F) return;
    inv = 1.0F / area;

    min_x = v0->x; max_x = v0->x; min_y = v0->y; max_y = v0->y;
    if (v1->x < min_x) min_x = v1->x; if (v1->x > max_x) max_x = v1->x;
    if (v2->x < min_x) min_x = v2->x; if (v2->x > max_x) max_x = v2->x;
    if (v1->y < min_y) min_y = v1->y; if (v1->y > max_y) max_y = v1->y;
    if (v2->y < min_y) min_y = v2->y; if (v2->y > max_y) max_y = v2->y;
    x0 = (s32)min_x; x1 = (s32)max_x + 1; y0 = (s32)min_y; y1 = (s32)max_y + 1;
    if (x0 < sScissor[0]) x0 = sScissor[0]; if (y0 < sScissor[1]) y0 = sScissor[1];
    if (x1 > sScissor[2]) x1 = sScissor[2]; if (y1 > sScissor[3]) y1 = sScissor[3];

    for (y = y0; y < y1; y++)
    {
        for (x = x0; x < x1; x++)
        {
            f32 px = x + 0.5F, py = y + 0.5F;
            f32 w0 = ((v1->x - px) * (v2->y - py) - (v2->x - px) * (v1->y - py)) * inv;
            f32 w1 = ((v2->x - px) * (v0->y - py) - (v0->x - px) * (v2->y - py)) * inv;
            f32 w2 = 1.0F - w0 - w1;
            f32 iw, w, z;
            PortColor shade, tex, out;

            if (w0 < 0.0F || w1 < 0.0F || w2 < 0.0F)
            {
                continue;
            }
            z = w0 * v0->z + w1 * v1->z + w2 * v2->z;
            if (use_z && (sOtherModeL & Z_CMP) && z >= sDepth[y * FB_W + x])
            {
                continue;
            }
            iw = w0 * v0->iw + w1 * v1->iw + w2 * v2->iw;
            w = 1.0F / iw;
            shade.r = (w0 * v0->r + w1 * v1->r + w2 * v2->r) * w;
            shade.g = (w0 * v0->g + w1 * v1->g + w2 * v2->g) * w;
            shade.b = (w0 * v0->b + w1 * v1->b + w2 * v2->b) * w;
            shade.a = (w0 * v0->a + w1 * v1->a + w2 * v2->a) * w;
            if (use_tex)
            {
                gfx_sample(&tex, sTextureTile, (w0 * v0->s + w1 * v1->s + w2 * v2->s) * w,
                           (w0 * v0->t + w1 * v1->t + w2 * v2->t) * w);
            }
            else
            {
                tex.r = tex.g = tex.b = tex.a = 255.0F;
            }
            gfx_combine(&out, &tex, &shade);
            if ((sOtherModeL & (CVG_X_ALPHA | ALPHA_CVG_SEL | 3)) && out.a < 8.0F)
            {
                continue;
            }
            if (use_z && (sOtherModeL & Z_UPD))
            {
                sDepth[y * FB_W + x] = z;
            }
            gfx_plot(x, y, &out);
        }
    }
}

#define CLIP_W 0.05F

static void lerp_vtx(PortVtx *out, const PortVtx *a, const PortVtx *b, f32 k)
{
    out->x = a->x + (b->x - a->x) * k; out->y = a->y + (b->y - a->y) * k;
    out->z = a->z + (b->z - a->z) * k; out->w = a->w + (b->w - a->w) * k;
    out->s = a->s + (b->s - a->s) * k; out->t = a->t + (b->t - a->t) * k;
    out->r = a->r + (b->r - a->r) * k; out->g = a->g + (b->g - a->g) * k;
    out->b = a->b + (b->b - a->b) * k; out->a = a->a + (b->a - a->a) * k;
}

static void gfx_triangle(s32 i0, s32 i1, s32 i2)
{
    PortVtx in[3], poly[4];
    PortScreenVtx sv[4];
    s32 count = 0, i;

    if (i0 >= VTX_MAX || i1 >= VTX_MAX || i2 >= VTX_MAX || gfx_is_offscreen())
    {
        return;
    }
    in[0] = sVtx[i0]; in[1] = sVtx[i1]; in[2] = sVtx[i2];
    /* Clip against the near plane (w = CLIP_W). */
    for (i = 0; i < 3; i++)
    {
        const PortVtx *a = &in[i], *b = &in[(i + 1) % 3];

        if (a->w >= CLIP_W)
        {
            poly[count++] = *a;
        }
        if ((a->w >= CLIP_W) != (b->w >= CLIP_W))
        {
            lerp_vtx(&poly[count++], a, b, (CLIP_W - a->w) / (b->w - a->w));
        }
    }
    if (count < 3)
    {
        return;
    }
    for (i = 0; i < count; i++)
    {
        to_screen(&sv[i], &poly[i]);
    }
    raster_triangle(&sv[0], &sv[1], &sv[2]);
    if (count == 4)
    {
        raster_triangle(&sv[0], &sv[2], &sv[3]);
    }
    sStatTris++;
}

/* ---- rectangles ------------------------------------------------------------------------- */
static void gfx_fill_rect(u32 w0, u32 w1)
{
    s32 x1 = ((w0 >> 12) & 0xFFF) >> 2, y1 = (w0 & 0xFFF) >> 2;
    s32 x0 = ((w1 >> 12) & 0xFFF) >> 2, y0 = (w1 & 0xFFF) >> 2;
    u32 cycle = sOtherModeH & (3 << G_MDSFT_CYCLETYPE);
    s32 x, y;

    if (cycle == G_CYC_FILL || cycle == G_CYC_COPY)
    {
        x1++; y1++;
    }
    if (x0 < sScissor[0]) x0 = sScissor[0]; if (y0 < sScissor[1]) y0 = sScissor[1];
    if (x1 > sScissor[2]) x1 = sScissor[2]; if (y1 > sScissor[3]) y1 = sScissor[3];
    if (gfx_is_offscreen())
    {
        /* Filling the depth image: clear the depth buffer. */
        for (y = y0; y < y1; y++)
        {
            for (x = x0; x < x1; x++)
            {
                sDepth[y * FB_W + x] = 1.0e30F;
            }
        }
        return;
    }
    for (y = y0; y < y1; y++)
    {
        for (x = x0; x < x1; x++)
        {
            if (cycle == G_CYC_FILL)
            {
                u8 *dst = &sBack[(y * FB_W + x) * 4];

                dst[0] = (u8)sFill.r; dst[1] = (u8)sFill.g; dst[2] = (u8)sFill.b; dst[3] = 255;
            }
            else
            {
                PortColor white = { 255.0F, 255.0F, 255.0F, 255.0F }, out;

                gfx_combine(&out, &white, &white);
                gfx_plot(x, y, &out);
            }
        }
    }
    sStatRects++;
}

static void gfx_tex_rect(u32 w0, u32 w1, sb32 is_flip)
{
    f32 x1 = ((w0 >> 12) & 0xFFF) / 4.0F, y1 = (w0 & 0xFFF) / 4.0F;
    f32 x0 = ((w1 >> 12) & 0xFFF) / 4.0F, y0 = (w1 & 0xFFF) / 4.0F;
    s32 tile = (w1 >> 24) & 7;
    f32 s0 = (s16)(sRdpHalf1 >> 16) / 32.0F, t0 = (s16)(sRdpHalf1 & 0xFFFF) / 32.0F;
    f32 dsdx = (s16)(sRdpHalf2 >> 16) / 1024.0F, dtdy = (s16)(sRdpHalf2 & 0xFFFF) / 1024.0F;
    u32 cycle = sOtherModeH & (3 << G_MDSFT_CYCLETYPE);
    s32 ix0, iy0, ix1, iy1, x, y;

    if (gfx_is_offscreen())
    {
        return;
    }
    if (cycle == G_CYC_COPY)
    {
        dsdx /= 4.0F;
        x1 += 1.0F; y1 += 1.0F;
    }
    ix0 = (s32)x0; iy0 = (s32)y0; ix1 = (s32)x1; iy1 = (s32)y1;
    if (ix0 < sScissor[0]) ix0 = sScissor[0]; if (iy0 < sScissor[1]) iy0 = sScissor[1];
    if (ix1 > sScissor[2]) ix1 = sScissor[2]; if (iy1 > sScissor[3]) iy1 = sScissor[3];

    for (y = iy0; y < iy1; y++)
    {
        for (x = ix0; x < ix1; x++)
        {
            f32 s = s0 + (x - x0) * dsdx, t = t0 + (y - y0) * dtdy;
            PortColor tex, out;

            if (is_flip)
            {
                gfx_sample(&tex, tile, t0 + (y - y0) * dsdx, s0 + (x - x0) * dtdy);
            }
            else
            {
                gfx_sample(&tex, tile, s, t);
            }
            if (cycle == G_CYC_COPY)
            {
                if (tex.a < 8.0F && (sOtherModeL & 3))
                {
                    continue;
                }
                out = tex;
                out.a = 255.0F;
                {
                    u8 *dst = &sBack[(y * FB_W + x) * 4];
                    dst[0] = (u8)out.r; dst[1] = (u8)out.g; dst[2] = (u8)out.b; dst[3] = 255;
                }
            }
            else
            {
                PortColor white = { 255.0F, 255.0F, 255.0F, 255.0F };

                gfx_combine(&out, &tex, &white);
                gfx_plot(x, y, &out);
            }
        }
    }
    sStatRects++;
}

#endif /* PORT_GFX_GPU */

/* ---- display list walker ------------------------------------------------------------------ */
static void color_from_rgba32(PortColor *c, u32 v)
{
    c->r = (f32)(v >> 24); c->g = (f32)((v >> 16) & 0xFF); c->b = (f32)((v >> 8) & 0xFF); c->a = (f32)(v & 0xFF);
}

static void gfx_move_mem(u32 w0, uintptr_t w1)
{
    u32 index = w0 & 0xFF, offset = ((w0 >> 8) & 0xFF) * 8;
    void *src = gfx_addr(w1);

    if (src == NULL)
    {
        return;
    }
    if (index == G_MV_VIEWPORT)
    {
        Vp *vp = src;

        sVpScale[0] = vp->vp.vscale[0] / 4.0F; sVpScale[1] = vp->vp.vscale[1] / 4.0F; sVpScale[2] = vp->vp.vscale[2];
        sVpTrans[0] = vp->vp.vtrans[0] / 4.0F; sVpTrans[1] = vp->vp.vtrans[1] / 4.0F; sVpTrans[2] = vp->vp.vtrans[2];
    }
    else if (index == G_MV_LIGHT)
    {
        s32 slot = (s32)(offset / 24) - 2;
        Light *light = src;

        if (slot >= 0 && slot < 9)
        {
            f32 x = light->l.dir[0], y = light->l.dir[1], z = light->l.dir[2];
            f32 len = sqrtf(x * x + y * y + z * z);

            sLightColor[slot][0] = light->l.col[0];
            sLightColor[slot][1] = light->l.col[1];
            sLightColor[slot][2] = light->l.col[2];
            if (len > 0.0F)
            {
                x /= len; y /= len; z /= len;
            }
            sLightDir[slot][0] = x; sLightDir[slot][1] = y; sLightDir[slot][2] = z;
        }
    }
}

static Gfx *sDlStack[DL_STACK_MAX];
static s32 sDlDepth;

static void gfx_run(Gfx *dl)
{
    Gfx **stack = sDlStack; /* static so a fault report can show the call chain */
#define depth sDlDepth

    depth = 0;
    u32 guard = 0;

    while (dl != NULL && guard++ < 400000)
    {
        u32 w0 = (u32)dl->words.w0;
        uintptr_t p1 = dl->words.w1;
        u32 w1 = (u32)p1;

        if (gPortGfxTraceFrame != 0 && gPortFrameCount == gPortGfxTraceFrame)
        {
            u32 file_id = 0, offset = 0;

            /* Display lists in asset data are reported as file:offset so they can be looked up. */
            if (port_asset_describe(dl, &file_id, &offset))
            {
                port_log("dl %u:%05X+%X d%d: %02X %08X %p", file_id, offset, 0, depth, w0 >> 24, w0, (void *)p1);
            }
            else
            {
                port_log("dl %p d%d: %02X %08X %p", dl, depth, w0 >> 24, w0, (void *)p1);
            }
        }
        sCurDl = dl;
        dl++;
        switch (w0 >> 24)
        {
        case G_VTX: gfx_vertices(w0, p1); break;
        case G_TRI1: gfx_triangle(((w0 >> 16) & 0xFF) / 2, ((w0 >> 8) & 0xFF) / 2, (w0 & 0xFF) / 2); break;
        case G_TRI2:
        case G_QUAD:
            gfx_triangle(((w0 >> 16) & 0xFF) / 2, ((w0 >> 8) & 0xFF) / 2, (w0 & 0xFF) / 2);
            gfx_triangle(((w1 >> 16) & 0xFF) / 2, ((w1 >> 8) & 0xFF) / 2, (w1 & 0xFF) / 2);
            break;
        case G_MODIFYVTX:
        {
            u32 index = (w0 & 0xFFFF) / 2;

            if (index < VTX_MAX)
            {
                PortVtx *v = &sVtx[index];

                switch ((w0 >> 16) & 0xFF)
                {
                case G_MWO_POINT_RGBA:
                    v->r = (w1 >> 24) & 0xFF; v->g = (w1 >> 16) & 0xFF; v->b = (w1 >> 8) & 0xFF; v->a = w1 & 0xFF;
                    break;
                case G_MWO_POINT_ST:
                    v->s = (f32)(s16)(w1 >> 16) / 32.0F;
                    v->t = (f32)(s16)(w1 & 0xFFFF) / 32.0F;
                    break;
                }
            }
            break;
        }
        case G_MTX: gfx_matrix(w0, p1); break;
        case G_POPMTX:
            sModelviewTop -= w1 / 64;
            if (sModelviewTop < 0) sModelviewTop = 0;
            mtx_mul(sMP, sModelview[sModelviewTop], sProjection); sMPInserted = FALSE;
            break;
        case G_GEOMETRYMODE: sGeometryMode = (sGeometryMode & (w0 & 0x00FFFFFF)) | w1; break;
        case G_TEXTURE:
            sTextureTile = (w0 >> 8) & 7;
            sTextureOn = ((w0 >> 1) & 0x7F) != 0;
            sTextureScaleS = (w1 >> 16) / 65536.0F;
            sTextureScaleT = (w1 & 0xFFFF) / 65536.0F;
            break;
        case G_MOVEWORD:
            switch ((w0 >> 16) & 0xFF)
            {
            case G_MW_MATRIX:
            {
                sMPInserted = TRUE;
                /*
                 * gSPInsertMatrix: overwrite two adjacent elements of the combined
                 * modelview-projection matrix. The RSP keeps it as 16 integer parts followed by
                 * 16 fraction parts; the offset selects the pair. Games use this to cancel the
                 * rotation of the matrix so that an object faces the camera (billboards).
                 */
                u32 offset = w0 & 0xFFFF;
                sb32 is_frac = (offset >= 0x20);
                u32 element = (offset & 0x1F) / 2, k;

                for (k = 0; k < 2 && element + k < 16; k++)
                {
                    f32 *m = &sMP[(element + k) / 4][(element + k) % 4];
                    s32 fixed = (s32)(*m * 65536.0F);
                    u32 part = (k == 0) ? ((u32)w1 >> 16) : ((u32)w1 & 0xFFFF);

                    if (is_frac)
                    {
                        fixed = (s32)(((u32)fixed & 0xFFFF0000) | part);
                    }
                    else
                    {
                        fixed = (s32)(((u32)(s32)(s16)part << 16) | ((u32)fixed & 0xFFFF));
                    }
                    *m = (f32)fixed / 65536.0F;
                }
                break;
            }
            case G_MW_SEGMENT: sSegments[((w0 & 0xFFFF) / 4) & 0xF] = p1; break;
            case G_MW_NUMLIGHT: sLightsNum = w1 / 24; if (sLightsNum > 8) sLightsNum = 8; break;
            case G_MW_LIGHTCOL:
            {
                /* Each light holds its colour twice (offsets 0 and 4 of a 24-byte slot). Fighter
                 * materials recolour the lights this way; the slot after the last light is ambient. */
                u32 slot = (w0 & 0xFFFF) / 24;

                if (slot < 9)
                {
                    sLightColor[slot][0] = (w1 >> 24) & 0xFF;
                    sLightColor[slot][1] = (w1 >> 16) & 0xFF;
                    sLightColor[slot][2] = (w1 >> 8) & 0xFF;
                }
                break;
            }
            }
            break;
        case G_MOVEMEM: gfx_move_mem(w0, p1); break;
        case G_DL:
        {
            Gfx *target = gfx_addr(p1);

            if (gfx_is_segmented(p1) && target != NULL)
            {
                /* A segmented display list address counts in 8-byte N64 commands (models call their
                 * material lists as segment 0xE + index * 8); native commands are 16 bytes when
                 * pointers are 64-bit. */
                target = (Gfx *)(sSegments[(p1 >> 24) & 0xF] + (p1 & 0x00FFFFFF) * (sizeof(Gfx) / 8));
            }

            if (((w0 >> 16) & 0xFF) == G_DL_PUSH && depth < DL_STACK_MAX)
            {
                stack[depth++] = dl;
            }
            dl = target;
            break;
        }
        case G_ENDDL:
            dl = (depth > 0) ? stack[--depth] : NULL;
            break;
        case G_RDPHALF_1: sRdpHalf1 = w1; break;
        case G_RDPHALF_2: sRdpHalf2 = w1; break;
        case G_SETOTHERMODE_L:
        {
            u32 len = (w0 & 0xFF) + 1, shift = 32 - ((w0 >> 8) & 0xFF) - len;
            u32 mask = ((len >= 32) ? 0xFFFFFFFF : ((1u << len) - 1)) << shift;

            sOtherModeL = (sOtherModeL & ~mask) | (w1 & mask);
            break;
        }
        case G_SETOTHERMODE_H:
        {
            u32 len = (w0 & 0xFF) + 1, shift = 32 - ((w0 >> 8) & 0xFF) - len;
            u32 mask = ((len >= 32) ? 0xFFFFFFFF : ((1u << len) - 1)) << shift;

            sOtherModeH = (sOtherModeH & ~mask) | (w1 & mask);
            break;
        }
        case G_RDPSETOTHERMODE: sOtherModeH = w0 & 0x00FFFFFF; sOtherModeL = w1; break;
        case G_TEXRECT:
        case G_TEXRECTFLIP:
        {
            /* The texture coordinates follow in two G_RDPHALF commands. */
            u32 rect_w0 = w0, rect_w1 = w1;

            sRdpHalf1 = (u32)dl[0].words.w1;
            sRdpHalf2 = (u32)dl[1].words.w1;
            dl += 2;
            gfx_tex_rect(rect_w0, rect_w1, (rect_w0 >> 24) == G_TEXRECTFLIP);
            break;
        }
        case G_SETSCISSOR:
            sScissor[0] = ((w0 >> 12) & 0xFFF) >> 2; sScissor[1] = (w0 & 0xFFF) >> 2;
            sScissor[2] = ((w1 >> 12) & 0xFFF) >> 2; sScissor[3] = (w1 & 0xFFF) >> 2;
            if (sScissor[2] > FB_W) sScissor[2] = FB_W;
            if (sScissor[3] > FB_H) sScissor[3] = FB_H;
            break;
        case G_LOADTLUT: gfx_load_tlut(w1); break;
        case G_SETTILESIZE:
        {
            PortTile *tile = &sTiles[(w1 >> 24) & 7];

#ifdef PORT_GFX_GPU
            sTexGen++;
#endif

            tile->sl = ((w0 >> 12) & 0xFFF) / 4.0F; tile->tl = (w0 & 0xFFF) / 4.0F;
            tile->sh = ((w1 >> 12) & 0xFFF) / 4.0F; tile->th = (w1 & 0xFFF) / 4.0F;
            break;
        }
        case G_LOADBLOCK: gfx_load_block(w0, w1); break;
        case G_LOADTILE: gfx_load_tile(w0, w1); break;
        case G_SETTILE:
        {
            PortTile *tile = &sTiles[(w1 >> 24) & 7];

#ifdef PORT_GFX_GPU
            sTexGen++;
#endif

            tile->fmt = (w0 >> 21) & 7; tile->siz = (w0 >> 19) & 3;
            tile->line = (w0 >> 9) & 0x1FF; tile->tmem = w0 & 0x1FF;
            tile->palette = (w1 >> 20) & 0xF;
            tile->cmt = (w1 >> 18) & 3; tile->maskt = (w1 >> 14) & 0xF; tile->shiftt = (w1 >> 10) & 0xF;
            tile->cms = (w1 >> 8) & 3; tile->masks = (w1 >> 4) & 0xF; tile->shifts = w1 & 0xF;
            break;
        }
        case G_FILLRECT: gfx_fill_rect(w0, w1); break;
        case G_SETFILLCOLOR: color_from_5551(&sFill, w1 >> 16); break;
        case G_SETBLENDCOLOR: color_from_rgba32(&sBlend, w1); break;
        case G_SETPRIMCOLOR: color_from_rgba32(&sPrim, w1); break;
        case G_SETENVCOLOR: color_from_rgba32(&sEnv, w1); break;
        case G_SETFOGCOLOR: color_from_rgba32(&sFog, w1); break;
        case G_SETCOMBINE: sCombineW0 = w0 & 0x00FFFFFF; sCombineW1 = w1; break;
        case G_SETTIMG:
            sTimgFmt = (w0 >> 21) & 7; sTimgSiz = (w0 >> 19) & 3; sTimgWidth = (w0 & 0xFFF) + 1;
            sTimgAddr = p1;
            break;
        case G_SETZIMG: sDepthImage = (uintptr_t)gfx_addr(p1); break;
        case G_SETCIMG: sColorImage = (uintptr_t)gfx_addr(p1); break;

        /* Nothing to do natively. */
        case G_NOOP: case G_SPNOOP: case G_LOAD_UCODE: case G_RDPLOADSYNC: case G_RDPPIPESYNC:
        case G_RDPTILESYNC: case G_RDPFULLSYNC: case G_SETPRIMDEPTH:
        case G_SETKEYGB: case G_SETKEYR: case G_SETCONVERT: case G_CULLDL:
        case G_BRANCH_Z: case G_LINE3D: case G_SPECIAL_1: case G_SPECIAL_2: case G_SPECIAL_3:
        case G_DMA_IO:
            break;
        default:
            sStatUnknown++;
            if (sUnknownSeen[w0 >> 24] == 0)
            {
                /* Report each unhandled opcode once, with the command that carried it. */
                sUnknownSeen[w0 >> 24] = 1;
                port_log("gfx: unhandled command 0x%02X (w0 %08X w1 %p) at frame %u", w0 >> 24, w0, (void *)p1, gPortFrameCount);
            }
            break;
        }
    }
}

#undef depth

static void gfx_describe(const char *what, void *ptr)
{
    u32 file_id = 0, offset = 0;

    if (port_asset_describe(ptr, &file_id, &offset))
    {
        port_log("gfx:   %s %p = asset file %u, object offset 0x%X", what, ptr, file_id, offset);
    }
    else
    {
        port_log("gfx:   %s %p (not asset data)", what, ptr);
    }
}

void port_gfx_run_task(OSTask *task)
{
    gPortProgress[4]++;
#ifdef PORT_GFX_GPU
    port_gpu_frame_begin();
#endif
    sModelviewTop = 0;
    sScissor[0] = 0; sScissor[1] = 0; sScissor[2] = FB_W; sScissor[3] = FB_H;
    sStatTris = sStatRects = sStatUnknown = 0;
    /*
     * A display list that points at bad data must not take the game down: the frame is cut
     * short and the offending command is reported (once per distinct command).
     */
#ifdef _WIN32
    gPortFaultGuard++;
    __try
    {
        gfx_run((Gfx *)task->t.data_ptr);
    }
    __except (1)
    {
        static Gfx *sReported[16];
        static u32 sReportedNum;
        u32 i, file_id = 0, offset = 0;

        for (i = 0; i < sReportedNum && sReported[i] != sCurDl; i++);
        if (i == sReportedNum)
        {
            if (sReportedNum < sizeof(sReported) / sizeof(sReported[0]))
            {
                sReported[sReportedNum++] = sCurDl;
            }
            if (port_asset_describe(sCurDl, &file_id, &offset))
            {
                port_log("gfx: fault at frame %u in command %08X %p (asset file %u, object offset 0x%X)", gPortFrameCount,
                         (u32)sCurDl->words.w0, (void *)sCurDl->words.w1, file_id, offset);
            }
            else
            {
                port_log("gfx: fault at frame %u in command %08X %p (dl %p, built at run time)", gPortFrameCount,
                         (u32)sCurDl->words.w0, (void *)sCurDl->words.w1, sCurDl);
            }
            /* What the command was working with: the texture image, the lists that led here, and
             * the argument read as a native pointer and as two pointer tokens. */
            gfx_describe("texture image", (void *)sTimgAddr);
            gfx_describe("argument", (void *)sCurDl->words.w1);
            gfx_describe("argument low half as token", PORT_PTR(void *, (u32)sCurDl->words.w1));
#ifdef PORT_64BIT
            gfx_describe("argument high half as token", PORT_PTR(void *, (u32)(sCurDl->words.w1 >> 32)));
#endif
            for (i = 0; i < (u32)sDlDepth && i < DL_STACK_MAX; i++)
            {
                gfx_describe("called from", sDlStack[i] - 1);
            }
        }
    }
    gPortFaultGuard--;
#else
    gfx_run((Gfx *)task->t.data_ptr);
#endif
    if (gPortVerbose && (gPortFrameCount % 60) == 0)
    {
        port_log("gfx: frame %u: %u tris, %u rects, %u unknown commands", gPortFrameCount, sStatTris, sStatRects, sStatUnknown);
    }
}

void port_gfx_set_framebuffer(void *fb)
{
    /* The game presents a finished buffer: latch what has been drawn. */
#ifdef PORT_GFX_GPU
    port_gpu_frame_end();
#else
    port_memcpy(sFront, sBack, sizeof(sFront));
#endif
    sHasFrame = TRUE;
}

int n64_draws_to_screen(void)
{
#ifdef PORT_GFX_GPU
    return 1;
#else
    return 0;
#endif
}

const unsigned char *n64_framebuffer(int *width, int *height)
{
    *width = FB_W;
    *height = FB_H;
#ifdef PORT_GFX_GPU
    return NULL; /* the picture is in the GPU backend's render target */
#else
    return sHasFrame ? sFront : NULL;
#endif
}
