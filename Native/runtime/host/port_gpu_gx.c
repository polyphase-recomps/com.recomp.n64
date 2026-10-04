/*
 * GX backend of the display list interpreter (GameCube / Wii), see port_gpu.h.
 *
 * Vertices arrive transformed to N64 clip space. GX takes eye-space positions and one of two
 * fixed projection forms, so the clip-space position is passed through one of them:
 *   perspective  position (x, y, -w), projection with unit x / y scale: GX gets clip x, y, w
 *                back and derives depth from w, which is exact for a perspective projection
 *                (PortGpuState.depth_k1 / depth_k2);
 *   orthographic position (x / w, y / w, z / w) with w constant.
 * The N64 combiner (a - b) * c + d becomes one or two TEV stages per cycle.
 */
#if defined(GEKKO) && defined(PORT_GFX_GPU)
#include <gccore.h>
#include <malloc.h>
#include <string.h>

#include "port_host.h"
#include "port_gpu.h"

#define N64_W 320.0f
#define N64_H 240.0f
#define VTXFMT GX_VTXFMT7 /* out of the way of whoever else draws with GX */

/* ---- render target ---------------------------------------------------------- */
static f32 sTargetW, sTargetH; /* EFB area the N64 screen maps to */
static GXRModeObj *sMode;
static void *sFifo;

/* Inside an engine GX is already set up; all this backend needs is the size of the EFB. */
static void target_init(void)
{
    if (sMode == NULL)
    {
        sMode = VIDEO_GetPreferredMode(NULL);
    }
    sTargetW = (f32)sMode->fbWidth;
    sTargetH = (f32)sMode->efbHeight;
}

/* ---- cached GX state ---------------------------------------------------------- */
static PortGpuState sState;
static int sStateValid;
static f32 sViewport[4] = { -1.0f, -1.0f, -1.0f, -1.0f };
static int sScissor[4] = { -1, -1, -1, -1 };
static int sProjMode = -1; /* 0 perspective, 1 orthographic */
static f32 sProjK1, sProjK2;
static int sVtxDescSet;

/* ---- textures ------------------------------------------------------------------ */
#define TEX_MAX 512
/* Decoded textures are 4 bytes per texel; keep what is cached within a budget. */
#ifndef PORT_GPU_TEX_BUDGET
#define PORT_GPU_TEX_BUDGET (8u << 20)
#endif

typedef struct GxTex
{
    unsigned int hash;
    GXTexObj obj;
    void *data;
    unsigned int bytes;
    unsigned int used; /* frame counter value when last drawn with */
    unsigned int serial; /* sDrawSerial when last drawn with */
    unsigned char wrap_s, wrap_t, filter;
} GxTex;

static unsigned int sDraws; /* draw calls this frame */
static GxTex sTex[TEX_MAX];
static unsigned int sTexBytes;
static unsigned int sFrame = 1;
/*
 * The graphics processor works through the command FIFO behind the CPU (on hardware; emulators
 * run it in step). Texture memory must not be freed or rewritten while a queued draw still
 * refers to it, or that draw samples whatever is there by then: sDrawSerial counts draws,
 * sSyncSerial is the count the processor is known to have finished.
 */
static unsigned int sDrawSerial = 1, sSyncSerial;
static unsigned int sStatCreates, sStatSyncs, sStatFails, sStatPeak, sStatEvicts;

static void texture_free(GxTex *tex)
{
    if (tex->data != NULL)
    {
        if (tex->serial > sSyncSerial)
        {
            GX_DrawDone(); /* wait until every queued draw is on screen */
            sStatSyncs++;
            sSyncSerial = sDrawSerial;
        }
        free(tex->data);
        tex->data = NULL;
        sTexBytes -= tex->bytes;
    }
}

/* Drop the least recently used texture; 0 when there is none left. */
static int texture_evict(void)
{
    unsigned int i, victim = TEX_MAX, oldest = 0xFFFFFFFFu;

    for (i = 0; i < TEX_MAX; i++)
    {
        if (sTex[i].data != NULL && sTex[i].used < oldest)
        {
            oldest = sTex[i].used;
            victim = i;
        }
    }
    if (victim == TEX_MAX)
    {
        return 0;
    }
    texture_free(&sTex[victim]);
    sStatEvicts++;
    return 1;
}

/* Drop least recently used textures until `needed` more bytes fit in the budget. */
static void texture_make_room(unsigned int needed)
{
    while (sTexBytes + needed > PORT_GPU_TEX_BUDGET && texture_evict())
    {
    }
}

int port_gpu_texture_valid(unsigned int handle, unsigned int hash)
{
    return handle != 0 && handle <= TEX_MAX && sTex[handle - 1].data != NULL && sTex[handle - 1].hash == hash;
}

unsigned int port_gpu_texture_find(unsigned int hash)
{
    unsigned int i;

    for (i = 0; i < TEX_MAX; i++)
    {
        if (sTex[i].data != NULL && sTex[i].hash == hash)
        {
            return i + 1;
        }
    }
    return 0;
}

unsigned int port_gpu_texture_create(unsigned int hash, int width, int height, const unsigned char *rgba)
{
    unsigned int i, slot = 0, oldest = 0xFFFFFFFFu;
    int tiles_x = (width + 3) / 4, tiles_y = (height + 3) / 4, x, y;
    unsigned char *data;
    GxTex *tex;

    for (i = 0; i < TEX_MAX; i++)
    {
        if (sTex[i].data == NULL)
        {
            slot = i;
            break;
        }
        if (sTex[i].used < oldest)
        {
            oldest = sTex[i].used;
            slot = i;
        }
    }
    if (gPortVerbose)
    {
        port_log("gx: texture %08X %dx%d -> slot %u (%u bytes cached)", hash, width, height, slot, sTexBytes);
    }
    tex = &sTex[slot];
    texture_free(tex);
    texture_make_room((unsigned int)(tiles_x * tiles_y * 64));
    /* The heap is shared with the host engine: when it has no room, make do with a smaller cache. */
    while ((data = memalign(32, (size_t)tiles_x * tiles_y * 64)) == NULL && texture_evict())
    {
    }
    if (data == NULL)
    {
        sStatFails++;
        return 0;
    }
    /* GX_TF_RGBA8: 4x4 blocks, each 32 bytes of (A, R) pairs then 32 bytes of (G, B) pairs. */
    for (y = 0; y < tiles_y * 4; y++)
    {
        int sy = (y < height) ? y : height - 1;

        for (x = 0; x < tiles_x * 4; x++)
        {
            int sx = (x < width) ? x : width - 1;
            const unsigned char *src = rgba + (sy * width + sx) * 4;
            unsigned char *block = data + ((y / 4) * tiles_x + (x / 4)) * 64;
            int index = ((y & 3) * 4 + (x & 3)) * 2;

            block[index] = src[3];
            block[index + 1] = src[0];
            block[32 + index] = src[1];
            block[32 + index + 1] = src[2];
        }
    }
    DCFlushRange(data, (u32)(tiles_x * tiles_y * 64));
    sStatCreates++;
    tex->data = data;
    tex->bytes = (unsigned int)(tiles_x * tiles_y * 64);
    sTexBytes += tex->bytes;
    tex->hash = hash;
    tex->used = sFrame;
    tex->wrap_s = tex->wrap_t = tex->filter = 0xFF;
    GX_InitTexObj(&tex->obj, data, (u16)width, (u16)height, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InvalidateTexAll();
    sStateValid = 0; /* the slot may have been bound under its previous contents */
    return slot + 1;
}

static void bind_texture(const PortGpuState *st)
{
    static const u8 wrap[3] = { GX_REPEAT, GX_MIRROR, GX_CLAMP };
    GxTex *tex = &sTex[st->texture - 1];

    tex->used = sFrame;
    if (tex->wrap_s != st->wrap_s || tex->wrap_t != st->wrap_t || tex->filter != st->filter)
    {
        u8 filter = st->filter ? GX_LINEAR : GX_NEAR;

        tex->wrap_s = st->wrap_s; tex->wrap_t = st->wrap_t; tex->filter = st->filter;
        GX_InitTexObjWrapMode(&tex->obj, wrap[st->wrap_s % 3], wrap[st->wrap_t % 3]);
        GX_InitTexObjFilterMode(&tex->obj, filter, filter);
    }
    GX_LoadTexObj(&tex->obj, GX_TEXMAP0);
}

/* ---- combiner -> TEV ------------------------------------------------------------ */
static u8 tev_color_in(unsigned char in)
{
    switch (in)
    {
    case PORT_GPU_IN_ONE: return GX_CC_ONE;
    case PORT_GPU_IN_COMBINED: return GX_CC_CPREV;
    case PORT_GPU_IN_TEXEL: return GX_CC_TEXC;
    case PORT_GPU_IN_PRIM: return GX_CC_C0;
    case PORT_GPU_IN_SHADE: return GX_CC_RASC;
    case PORT_GPU_IN_ENV: return GX_CC_C1;
    case PORT_GPU_IN_COMBINED_A: return GX_CC_APREV;
    case PORT_GPU_IN_TEXEL_A: return GX_CC_TEXA;
    case PORT_GPU_IN_PRIM_A: return GX_CC_A0;
    case PORT_GPU_IN_SHADE_A: return GX_CC_RASA;
    case PORT_GPU_IN_ENV_A: return GX_CC_A1;
    default: return GX_CC_ZERO;
    }
}

static u8 tev_alpha_in(unsigned char in)
{
    switch (in)
    {
    case PORT_GPU_IN_ONE: return GX_CA_KONST; /* the stage's constant alpha is selected as 1 */
    case PORT_GPU_IN_COMBINED: return GX_CA_APREV;
    case PORT_GPU_IN_TEXEL: return GX_CA_TEXA;
    case PORT_GPU_IN_PRIM: return GX_CA_A0;
    case PORT_GPU_IN_SHADE: return GX_CA_RASA;
    case PORT_GPU_IN_ENV: return GX_CA_A1;
    default: return GX_CA_ZERO;
    }
}

static void tev_stage_common(u8 stage, int has_tex)
{
    GX_SetTevOrder(stage, has_tex ? GX_TEXCOORD0 : GX_TEXCOORDNULL, has_tex ? GX_TEXMAP0 : GX_TEXMAP_DISABLE, GX_COLOR0A0);
    GX_SetTevKAlphaSel(stage, GX_TEV_KASEL_1);
    GX_SetTevKColorSel(stage, GX_TEV_KCSEL_K0);
}

/*
 * One N64 cycle, (a - b) * c + d, on two TEV stages. A stage computes d' +- lerp(a', b', c');
 * the first stage produces a * c + d, the second subtracts b * c. When the form allows it the
 * second stage just passes the result on.
 */
static u8 tev_cycle(u8 stage, const PortGpuCycle *cy, int has_tex)
{
    u8 a = tev_color_in(cy->a), b = tev_color_in(cy->b), c = tev_color_in(cy->c), d = tev_color_in(cy->d);
    u8 aa = tev_alpha_in(cy->aa), ab = tev_alpha_in(cy->ab), ac = tev_alpha_in(cy->ac), ad = tev_alpha_in(cy->ad);
    int color_two, alpha_two;

    tev_stage_common(stage, has_tex);
    /* colour, first stage */
    if (cy->b == PORT_GPU_IN_ZERO || cy->c == PORT_GPU_IN_ZERO)
    {
        GX_SetTevColorIn(stage, GX_CC_ZERO, a, (cy->c == PORT_GPU_IN_ZERO) ? GX_CC_ZERO : c, d); /* a * c + d */
        GX_SetTevColorOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        color_two = 0;
    }
    else if (cy->b == cy->d)
    {
        GX_SetTevColorIn(stage, b, a, c, GX_CC_ZERO); /* lerp(b, a, c) */
        GX_SetTevColorOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        color_two = 0;
    }
    else if (cy->a == PORT_GPU_IN_ZERO)
    {
        GX_SetTevColorIn(stage, GX_CC_ZERO, b, c, d); /* d - b * c */
        GX_SetTevColorOp(stage, GX_TEV_SUB, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        color_two = 0;
    }
    else
    {
        /* a * c + d into register 2 (the previous result may still be an input of b or c) */
        GX_SetTevColorIn(stage, GX_CC_ZERO, a, c, d);
        GX_SetTevColorOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_FALSE, GX_TEVREG2);
        color_two = 1;
    }
    /* alpha, first stage */
    if (cy->ab == PORT_GPU_IN_ZERO || cy->ac == PORT_GPU_IN_ZERO)
    {
        GX_SetTevAlphaIn(stage, GX_CA_ZERO, aa, (cy->ac == PORT_GPU_IN_ZERO) ? GX_CA_ZERO : ac, ad);
        GX_SetTevAlphaOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        alpha_two = 0;
    }
    else if (cy->ab == cy->ad)
    {
        GX_SetTevAlphaIn(stage, ab, aa, ac, GX_CA_ZERO);
        GX_SetTevAlphaOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        alpha_two = 0;
    }
    else if (cy->aa == PORT_GPU_IN_ZERO)
    {
        GX_SetTevAlphaIn(stage, GX_CA_ZERO, ab, ac, ad);
        GX_SetTevAlphaOp(stage, GX_TEV_SUB, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        alpha_two = 0;
    }
    else
    {
        GX_SetTevAlphaIn(stage, GX_CA_ZERO, aa, ac, ad);
        GX_SetTevAlphaOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_FALSE, GX_TEVREG2);
        alpha_two = 1;
    }
    if (!color_two && !alpha_two)
    {
        return stage + 1;
    }
    /* second stage: subtract b * c from the partial result, or pass the finished one on */
    stage++;
    tev_stage_common(stage, has_tex);
    if (color_two)
    {
        GX_SetTevColorIn(stage, GX_CC_ZERO, b, c, GX_CC_C2);
        GX_SetTevColorOp(stage, GX_TEV_SUB, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    }
    else
    {
        GX_SetTevColorIn(stage, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_CPREV);
        GX_SetTevColorOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    }
    if (alpha_two)
    {
        GX_SetTevAlphaIn(stage, GX_CA_ZERO, ab, ac, GX_CA_A2);
        GX_SetTevAlphaOp(stage, GX_TEV_SUB, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    }
    else
    {
        GX_SetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
        GX_SetTevAlphaOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    }
    return stage + 1;
}

static void apply_state(const PortGpuState *st)
{
    static const u8 cull[3] = { GX_CULL_NONE, GX_CULL_FRONT, GX_CULL_BACK }; /* GX front faces are clockwise */
    int has_tex = (st->texture != 0);
    GXColor color;
    u8 stage = GX_TEVSTAGE0;
    int i;

    if (gPortVerbose)
    {
        port_log("gx: state cyc %d c0 (%d-%d)*%d+%d a0 (%d-%d)*%d+%d c1 (%d-%d)*%d+%d a1 (%d-%d)*%d+%d tex %u blend %d atest %d fog %d prim %02X%02X%02X%02X env %02X%02X%02X%02X",
                 st->cycles, st->cycle[0].a, st->cycle[0].b, st->cycle[0].c, st->cycle[0].d,
                 st->cycle[0].aa, st->cycle[0].ab, st->cycle[0].ac, st->cycle[0].ad,
                 st->cycle[1].a, st->cycle[1].b, st->cycle[1].c, st->cycle[1].d,
                 st->cycle[1].aa, st->cycle[1].ab, st->cycle[1].ac, st->cycle[1].ad,
                 st->texture, st->blend, st->alpha_test, st->fog_blend,
                 st->prim[0], st->prim[1], st->prim[2], st->prim[3], st->env[0], st->env[1], st->env[2], st->env[3]);
    }
    if (has_tex)
    {
        bind_texture(st);
    }
    for (i = 0; i < st->cycles; i++)
    {
        stage = tev_cycle(stage, &st->cycle[i], has_tex);
    }
    if (st->fog_blend)
    {
        /* fog colour (constant colour 0) over the result by the fog alpha (register 2 alpha) */
        tev_stage_common(stage, has_tex);
        GX_SetTevColorIn(stage, GX_CC_CPREV, GX_CC_KONST, GX_CC_A2, GX_CC_ZERO);
        GX_SetTevColorOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
        GX_SetTevAlphaOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        stage++;
        color.r = st->fog[0]; color.g = st->fog[1]; color.b = st->fog[2]; color.a = st->fog[3];
        GX_SetTevKColor(GX_KCOLOR0, color);
        GX_SetTevColor(GX_TEVREG2, color);
    }
    GX_SetNumTevStages(stage);
    GX_SetNumTexGens(has_tex ? 1 : 0);
    GX_SetNumChans(1);

    color.r = st->prim[0]; color.g = st->prim[1]; color.b = st->prim[2]; color.a = st->prim[3];
    GX_SetTevColor(GX_TEVREG0, color);
    color.r = st->env[0]; color.g = st->env[1]; color.b = st->env[2]; color.a = st->env[3];
    GX_SetTevColor(GX_TEVREG1, color);

    GX_SetCullMode(cull[st->cull % 3]);
    if (st->z_test || st->z_write)
    {
        GX_SetZMode(GX_TRUE, st->z_test ? GX_LEQUAL : GX_ALWAYS, st->z_write ? GX_TRUE : GX_FALSE);
    }
    else
    {
        GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    }
    if (st->blend)
    {
        GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    }
    else
    {
        GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    }
    if (st->alpha_test)
    {
        GX_SetAlphaCompare(GX_GEQUAL, 8, GX_AOP_AND, GX_ALWAYS, 0);
        GX_SetZCompLoc(GX_FALSE); /* depth is written only where the pixel survives */
    }
    else
    {
        GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
        GX_SetZCompLoc(GX_TRUE);
    }
}

static void set_projection(int ortho, f32 k1, f32 k2)
{
    Mtx44 proj;

    if (ortho == sProjMode && (ortho || (k1 == sProjK1 && k2 == sProjK2)))
    {
        return;
    }
    memset(proj, 0, sizeof(proj));
    proj[0][0] = 1.0f;
    proj[1][1] = 1.0f;
    if (ortho)
    {
        /* N64 depth -1..1 to the GX range -1..0 */
        proj[2][2] = 0.5f;
        proj[2][3] = -0.5f;
        proj[3][3] = 1.0f;
        GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);
    }
    else
    {
        /* eye z is -w: clip z = (k1 * w + k2 - w) / 2 */
        proj[2][2] = (1.0f - k1) * 0.5f;
        proj[2][3] = k2 * 0.5f;
        proj[3][2] = -1.0f;
        GX_LoadProjectionMtx(proj, GX_PERSPECTIVE);
    }
    sProjMode = ortho;
    sProjK1 = k1;
    sProjK2 = k2;
}

static void set_vertex_format(void)
{
    Mtx identity;

    if (sVtxDescSet)
    {
        return;
    }
    sVtxDescSet = 1;
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_SetVtxAttrFmt(VTXFMT, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GX_SetVtxAttrFmt(VTXFMT, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetVtxAttrFmt(VTXFMT, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    guMtxIdentity(identity);
    GX_LoadPosMtxImm(identity, GX_PNMTX0);
    GX_SetCurrentMtx(GX_PNMTX0);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_TRUE);
}

/* ---- interface ------------------------------------------------------------------ */
void port_gpu_frame_begin(void)
{
    if (sTargetW == 0.0f)
    {
        target_init();
    }
    /* libogc throttles the command FIFO by suspending "the GX thread" when it fills up. That
     * has to be the thread that is writing commands, which is this game thread now. */
    GX_SetCurrentGXThread();
    /* Someone else may have drawn since the last task: start from unknown GX state. */
    sStateValid = 0;
    sVtxDescSet = 0;
    sProjMode = -1;
    sViewport[2] = -1.0f;
    sScissor[2] = -1;
}

void port_gpu_frame_end(void)
{
    sDraws = 0;
    sFrame++;
    if (sTexBytes > sStatPeak) sStatPeak = sTexBytes;
    if ((sFrame % 300) == 0)
    {
        /* Worth a line whenever the cache is under pressure (a symptom is slow or missing geometry). */
        if (gPortVerbose || sStatFails != 0 || sStatSyncs > 30)
        {
            port_log("gx: %u textures made, %u evicted, %u waits, %u failures in 300 frames; %u KB cached",
                     sStatCreates, sStatEvicts, sStatSyncs, sStatFails, sTexBytes >> 10);
        }
        sStatCreates = sStatSyncs = sStatFails = sStatEvicts = 0;
    }
    /* Leave GX the way a host engine expects to find it for its own drawing: these are the
     * settings engines tend to make once rather than per draw. */
    GX_SetViewport(0.0f, 0.0f, sTargetW, sTargetH, 0.0f, 1.0f);
    GX_SetScissor(0, 0, (u32)sTargetW, (u32)sTargetH);
    GX_SetCullMode(GX_CULL_FRONT);
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GX_SetZCompLoc(GX_TRUE);
    GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_TRUE);
    GX_SetCurrentMtx(GX_PNMTX0);
    sStateValid = 0;
    sViewport[2] = -1.0f;
    sScissor[2] = -1;
}

void port_gpu_host_idle(void)
{
    /* Back on the host thread: it is the one that draws (and presents) from here on. */
    GX_SetCurrentGXThread();
}

void port_gpu_viewport(float x, float y, float width, float height)
{
    f32 sx = sTargetW / N64_W, sy = sTargetH / N64_H;

    if (x == sViewport[0] && y == sViewport[1] && width == sViewport[2] && height == sViewport[3])
    {
        return;
    }
    sViewport[0] = x; sViewport[1] = y; sViewport[2] = width; sViewport[3] = height;
    GX_SetViewport(x * sx, y * sy, width * sx, height * sy, 0.0f, 1.0f);
}

void port_gpu_scissor(int x0, int y0, int x1, int y1)
{
    f32 sx = sTargetW / N64_W, sy = sTargetH / N64_H;

    if (x0 == sScissor[0] && y0 == sScissor[1] && x1 == sScissor[2] && y1 == sScissor[3])
    {
        return;
    }
    sScissor[0] = x0; sScissor[1] = y0; sScissor[2] = x1; sScissor[3] = y1;
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    GX_SetScissor((u32)(x0 * sx), (u32)(y0 * sy), (u32)((x1 - x0) * sx), (u32)((y1 - y0) * sy));
}

void port_gpu_draw(const PortGpuState *state, const PortGpuVtx *vtx, int count)
{
    int i;

    sDraws++;

    set_vertex_format();
    if (!sStateValid || memcmp(state, &sState, sizeof(sState)) != 0)
    {
        sState = *state;
        sStateValid = 1;
        apply_state(state);
    }
    if (state->texture != 0)
    {
        sTex[state->texture - 1].used = sFrame;
        sTex[state->texture - 1].serial = sDrawSerial + 1;
    }
    set_projection(state->ortho, state->depth_k1, state->depth_k2);

    sDrawSerial++;
    GX_Begin((count == 4) ? GX_TRIANGLEFAN : GX_TRIANGLES, VTXFMT, (u16)count);
    for (i = 0; i < count; i++)
    {
        const PortGpuVtx *v = &vtx[i];

        if (state->ortho)
        {
            f32 iw = (v->w != 0.0f) ? 1.0f / v->w : 1.0f;

            GX_Position3f32(v->x * iw, v->y * iw, v->z * iw);
        }
        else
        {
            GX_Position3f32(v->x, v->y, -v->w);
        }
        GX_Color4u8(v->r, v->g, v->b, v->a);
        GX_TexCoord2f32(v->s, v->t);
    }
    GX_End();
}

void port_gpu_clear_depth(int x0, int y0, int x1, int y1)
{
    PortGpuState st;
    PortGpuVtx v[4];
    int i;

    memset(&st, 0, sizeof(st));
    st.cycles = 1;
    st.cycle[0].d = PORT_GPU_IN_SHADE;
    st.cycle[0].ad = PORT_GPU_IN_SHADE;
    st.z_write = 1;
    st.ortho = 1;
    for (i = 0; i < 4; i++)
    {
        int right = (i == 1 || i == 2), bottom = (i >= 2);

        v[i].x = (float)(right ? x1 : x0) / (N64_W / 2.0f) - 1.0f;
        v[i].y = 1.0f - (float)(bottom ? y1 : y0) / (N64_H / 2.0f);
        v[i].z = 1.0f; /* farthest */
        v[i].w = 1.0f;
        v[i].s = v[i].t = 0.0f;
        v[i].r = v[i].g = v[i].b = v[i].a = 0;
    }
    port_gpu_viewport(0.0f, 0.0f, N64_W, N64_H);
    port_gpu_scissor(0, 0, (int)N64_W, (int)N64_H);
    set_vertex_format();
    GX_SetColorUpdate(GX_FALSE);
    GX_SetAlphaUpdate(GX_FALSE);
    port_gpu_draw(&st, v, 4);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_TRUE);
}

/* ---- standalone use (host/main_ogc.c): GX is ours alone ------------------------------- */
void port_gpu_gx_init(GXRModeObj *mode)
{
    GXColor black = { 0, 0, 0, 255 };
    f32 yscale;
    u32 xfb_height;

    sMode = mode;
    target_init();
    sFifo = memalign(32, 256 * 1024);
    memset(sFifo, 0, 256 * 1024);
    GX_Init(sFifo, 256 * 1024);
    GX_SetCopyClear(black, GX_MAX_Z24);
    yscale = GX_GetYScaleFactor(mode->efbHeight, mode->xfbHeight);
    xfb_height = GX_SetDispCopyYScale(yscale);
    GX_SetDispCopySrc(0, 0, mode->fbWidth, mode->efbHeight);
    GX_SetDispCopyDst(mode->fbWidth, xfb_height);
    GX_SetCopyFilter(mode->aa, mode->sample_pattern, GX_TRUE, mode->vfilter);
    GX_SetFieldMode(mode->field_rendering, ((mode->viHeight == 2 * mode->xfbHeight) ? GX_ENABLE : GX_DISABLE));
    GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
    GX_SetDispCopyGamma(GX_GM_1_0);
    GX_SetDither(GX_FALSE);
    GX_SetViewport(0.0f, 0.0f, sTargetW, sTargetH, 0.0f, 1.0f);
    GX_SetScissor(0, 0, mode->fbWidth, mode->efbHeight);
}

void port_gpu_gx_present(void *xfb)
{
    GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_TRUE);
    GX_CopyDisp(xfb, GX_TRUE); /* the area outside the game's own fills stays black */
    GX_DrawDone();
    sStateValid = 0;
}

/* Test aid: the render target scaled to 320 x 240, as RGB8 rows (buffer of 320 * 240 * 3). */
void port_gpu_gx_read_frame(unsigned char *rgb)
{
    static unsigned char *sCopy;
    int x, y;

    if (sCopy == NULL)
    {
        sCopy = memalign(32, 320 * 240 * 4);
    }
    GX_SetTexCopySrc(0, 0, (u16)sTargetW, (u16)sTargetH);
    GX_SetTexCopyDst((u16)(sTargetW / 2), (u16)(sTargetH / 2), GX_TF_RGBA8, GX_TRUE);
    GX_CopyTex(sCopy, GX_FALSE);
    GX_PixModeSync();
    GX_DrawDone();
    DCInvalidateRange(sCopy, 320 * 240 * 4);
    for (y = 0; y < 240; y++)
    {
        for (x = 0; x < 320; x++)
        {
            const unsigned char *block = sCopy + ((y / 4) * 80 + (x / 4)) * 64;
            int index = ((y & 3) * 4 + (x & 3)) * 2;
            unsigned char *dst = rgb + (y * 320 + x) * 3;

            dst[0] = block[index + 1];
            dst[1] = block[32 + index];
            dst[2] = block[32 + index + 1];
        }
    }
}

#endif /* GEKKO && PORT_GFX_GPU */
