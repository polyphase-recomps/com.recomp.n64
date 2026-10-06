/*
 * citro3d backend of the display list interpreter (Nintendo 3DS), see port_gpu.h.
 *
 * citro3d only accepts drawing between C3D_FrameBegin and C3D_FrameEnd, which the host (the
 * engine, host/main_ctr.c) owns, while the game runs from the host's update. So the game's draws
 * are recorded - vertices into GPU-visible memory, states and draw ranges into a list - and
 * replayed by port_gpu_c3d_render() from inside the host's frame, onto whatever render target
 * the host is drawing. The last finished recording is replayed again on frames where the game
 * draws nothing new. Two recordings alternate, so the one the GPU may still be reading (replayed
 * last) is never written.
 *
 * Vertices arrive in N64 clip space and go to the GPU in its own: rotated a quarter turn for the
 * 3DS screens (whose framebuffers are portrait) and with depth -w (near) .. 0 (far), tested
 * with "greater" like the engine does. The N64 combiner becomes up to six TEV stages.
 */
#if defined(__3DS__) && defined(PORT_GFX_GPU)
#include <3ds.h>
#include <citro3d.h>
#include <stdlib.h>
#include <string.h>

#include "port_host.h"
#include "port_gpu.h"

#define N64_W 320.0f
#define N64_H 240.0f

extern const unsigned char n64port_c3d_shbin[];
extern const unsigned int n64port_c3d_shbin_size;

/* ---- textures ------------------------------------------------------------------ */
#define TEX_MAX 512
#ifndef PORT_GPU_TEX_BUDGET
#define PORT_GPU_TEX_BUDGET (4u << 20)
#endif

typedef struct CtrTex
{
    unsigned int hash;
    C3D_Tex tex;
    int live;
    unsigned int bytes;
    unsigned short width, height; /* the N64 texture; tex is padded to powers of two */
    float scale_s, scale_t;       /* width / padded width, height / padded height */
    unsigned int used;            /* id of the last recording that drew with it */
} CtrTex;

static CtrTex sTex[TEX_MAX];
/* hash -> slot + 1, open addressing; rebuilt whenever a texture goes away */
#define TEX_INDEX 1024
static unsigned short sTexIndex[TEX_INDEX];

static void index_add(unsigned int hash, unsigned int slot)
{
    unsigned int i = hash & (TEX_INDEX - 1);

    while (sTexIndex[i] != 0)
    {
        i = (i + 1) & (TEX_INDEX - 1);
    }
    sTexIndex[i] = (unsigned short)(slot + 1);
}

static void index_rebuild(void)
{
    unsigned int i;

    memset(sTexIndex, 0, sizeof(sTexIndex));
    for (i = 0; i < TEX_MAX; i++)
    {
        if (sTex[i].live)
        {
            index_add(sTex[i].hash, i);
        }
    }
}
static unsigned int sTexBytes;
static unsigned int sStatCreates, sStatFails, sStatEvicts;

/* ---- recordings ------------------------------------------------------------------ */
typedef struct CVtx
{
    float x, y, z, w;
    float s, t;
    unsigned char r, g, b, a;
} CVtx;

enum { BATCH_DRAW, BATCH_CLEAR_DEPTH };

typedef struct Batch
{
    unsigned int state; /* index into the recording's states */
    unsigned int first, count;
    float viewport[4];
    int scissor[4];
    unsigned char kind;
} Batch;

typedef struct Recording
{
    unsigned int id;
    CVtx *vtx; /* linear memory */
    unsigned int vtx_num, vtx_cap;
    Batch *batch;
    unsigned int batch_num, batch_cap;
    PortGpuState *state;
    unsigned int state_num, state_cap;
    int overflow;
} Recording;

static Recording sRec[2];
static int sRecording = -1; /* recording being written */
static int sLatest = -1;    /* last finished recording */
static int sShown = -1;     /* recording replayed last (the GPU may still read it) */
static unsigned int sNextId = 1;
static float sViewport[4] = { 0.0f, 0.0f, N64_W, N64_H };
static int sScissor[4] = { 0, 0, (int)N64_W, (int)N64_H };
static float sRect[4]; /* port_gpu_set_target_rect; width <= 0 = fit 4:3 in the target */
static u64 sTaskStart, sTaskTicks; /* time spent interpreting display lists (T&L, textures) */
static unsigned int sTaskFrames;

static unsigned int tex_bytes_of(int w, int h)
{
    return (unsigned int)(w * h * 4);
}

static int pot(int v)
{
    int p = 8;

    while (p < v && p < 1024)
    {
        p <<= 1;
    }
    return p;
}

static void texture_free(CtrTex *tex)
{
    if (tex->live)
    {
        C3D_TexDelete(&tex->tex);
        tex->live = 0;
        sTexBytes -= tex->bytes;
        index_rebuild();
    }
}

/* The oldest texture neither the GPU (the recording replayed last) nor the recording being
 * written uses; -1 if every texture is in use. */
static int texture_victim(void)
{
    unsigned int i, oldest = 0xFFFFFFFFu, keep_from;
    int victim = -1;

    keep_from = (sShown >= 0) ? sRec[sShown].id : 0xFFFFFFFFu;
    if (sRecording >= 0 && sRec[sRecording].id < keep_from)
    {
        keep_from = sRec[sRecording].id;
    }
    for (i = 0; i < TEX_MAX; i++)
    {
        if (sTex[i].live && sTex[i].used < keep_from && sTex[i].used < oldest)
        {
            oldest = sTex[i].used;
            victim = (int)i;
        }
    }
    return victim;
}

int port_gpu_texture_valid(unsigned int handle, unsigned int hash)
{
    return handle != 0 && handle <= TEX_MAX && sTex[handle - 1].live && sTex[handle - 1].hash == hash;
}

unsigned int port_gpu_texture_find(unsigned int hash)
{
    unsigned int i = hash & (TEX_INDEX - 1);

    while (sTexIndex[i] != 0)
    {
        const CtrTex *tex = &sTex[sTexIndex[i] - 1];

        if (tex->live && tex->hash == hash)
        {
            return sTexIndex[i];
        }
        i = (i + 1) & (TEX_INDEX - 1);
    }
    return 0;
}

/* Offset of texel (x, y) in a tiled texture of width `w` (8 x 8 tiles in rows, Z-order inside). */
static unsigned int tiled_offset(int x, int y, int w)
{
    unsigned int i = (unsigned int)((x & 7) | ((y & 7) << 8));

    i = (i ^ (i << 2)) & 0x1313;
    i = (i ^ (i << 1)) & 0x1515;
    i = (i | (i >> 7)) & 0x3F;
    return (unsigned int)(((y & ~7) * w + (x & ~7) * 8) + i);
}

unsigned int port_gpu_texture_create(unsigned int hash, int width, int height, const unsigned char *rgba)
{
    int pw = pot(width), ph = pot(height), x, y, victim;
    unsigned int i, slot = TEX_MAX, bytes = tex_bytes_of(pw, ph);
    unsigned char *data;
    CtrTex *tex;

    for (i = 0; i < TEX_MAX; i++)
    {
        if (!sTex[i].live)
        {
            slot = i;
            break;
        }
    }
    while (slot == TEX_MAX || sTexBytes + bytes > PORT_GPU_TEX_BUDGET)
    {
        victim = texture_victim();
        if (victim < 0)
        {
            break;
        }
        texture_free(&sTex[victim]);
        sStatEvicts++;
        if (slot == TEX_MAX)
        {
            slot = (unsigned int)victim;
        }
    }
    if (slot == TEX_MAX)
    {
        sStatFails++;
        return 0;
    }
    tex = &sTex[slot];
    if (!C3D_TexInit(&tex->tex, (u16)pw, (u16)ph, GPU_RGBA8))
    {
        /* linear memory is shared with the host: drop what we can and try once more */
        while ((victim = texture_victim()) >= 0)
        {
            texture_free(&sTex[victim]);
        }
        if (!C3D_TexInit(&tex->tex, (u16)pw, (u16)ph, GPU_RGBA8))
        {
            sStatFails++;
            return 0;
        }
    }
    /* Rows top-down as the GPU samples them (t = 1 is the first row); the padding repeats the
     * last row and column so clamping at the texture's own edge still works. */
    data = tex->tex.data;
    for (y = 0; y < ph; y++)
    {
        int sy = (y < height) ? y : height - 1;

        for (x = 0; x < pw; x++)
        {
            int sx = (x < width) ? x : width - 1;
            const unsigned char *src = rgba + (sy * width + sx) * 4;
            unsigned char *dst = data + tiled_offset(x, y, pw) * 4;

            dst[0] = src[3];
            dst[1] = src[2];
            dst[2] = src[1];
            dst[3] = src[0];
        }
    }
    C3D_TexFlush(&tex->tex);
    tex->live = 1;
    tex->hash = hash;
    tex->bytes = bytes;
    tex->width = (unsigned short)width;
    tex->height = (unsigned short)height;
    tex->scale_s = (float)width / (float)pw;
    tex->scale_t = (float)height / (float)ph;
    tex->used = (sRecording >= 0) ? sRec[sRecording].id : 0;
    sTexBytes += bytes;
    sStatCreates++;
    index_add(hash, slot);
    return slot + 1;
}

/* ---- recording ------------------------------------------------------------------- */
static int grow(void **array, unsigned int *cap, unsigned int need, unsigned int elem)
{
    unsigned int cap2;
    void *mem;

    if (need <= *cap)
    {
        return 1;
    }
    cap2 = (*cap != 0) ? *cap * 2 : 256;
    while (cap2 < need)
    {
        cap2 *= 2;
    }
    mem = realloc(*array, (size_t)cap2 * elem);
    if (mem == NULL)
    {
        return 0;
    }
    *array = mem;
    *cap = cap2;
    return 1;
}

static int vtx_reserve(Recording *rec, unsigned int count)
{
    unsigned int cap;
    CVtx *mem;

    if (rec->vtx_num + count <= rec->vtx_cap)
    {
        return 1;
    }
    /* Linear memory cannot be reallocated in place; this recording is not on the GPU. */
    cap = (rec->vtx_cap != 0) ? rec->vtx_cap * 2 : 16384;
    while (cap < rec->vtx_num + count)
    {
        cap *= 2;
    }
    mem = linearAlloc(cap * sizeof(CVtx));
    if (mem == NULL)
    {
        return 0;
    }
    if (rec->vtx != NULL)
    {
        memcpy(mem, rec->vtx, rec->vtx_num * sizeof(CVtx));
        linearFree(rec->vtx);
    }
    rec->vtx = mem;
    rec->vtx_cap = cap;
    return 1;
}

static unsigned int state_index(Recording *rec, const PortGpuState *st)
{
    if (rec->state_num != 0 && memcmp(&rec->state[rec->state_num - 1], st, sizeof(*st)) == 0)
    {
        return rec->state_num - 1;
    }
    if (!grow((void **)&rec->state, &rec->state_cap, rec->state_num + 1, sizeof(*st)))
    {
        return 0xFFFFFFFFu;
    }
    rec->state[rec->state_num] = *st;
    return rec->state_num++;
}

/* N64 clip space -> GPU clip space: a quarter turn (screens are portrait framebuffers) and
 * depth from -w..w (near..far) to -w..0. */
static void vtx_convert(CVtx *out, const PortGpuVtx *in, const PortGpuState *st, const CtrTex *tex)
{
    float z = (in->z - in->w) * 0.5f;

    if (st->decal)
    {
        z *= 1.001f; /* slightly nearer: decals win depth ties */
    }
    out->x = in->y;
    out->y = -in->x;
    out->z = z;
    out->w = in->w;
    if (tex != NULL)
    {
        out->s = in->s * tex->scale_s;
        out->t = 1.0f - in->t * tex->scale_t;
    }
    else
    {
        out->s = out->t = 0.0f;
    }
    out->r = in->r; out->g = in->g; out->b = in->b; out->a = in->a;
}

static void record(const PortGpuState *st, const PortGpuVtx *vtx, int count, int kind)
{
    static const unsigned char fan[6] = { 0, 1, 2, 0, 2, 3 };
    Recording *rec;
    const CtrTex *tex = NULL;
    unsigned int state, n = (count == 4) ? 6 : 3, i;
    Batch *last;

    if (sRecording < 0)
    {
        return; /* outside a frame */
    }
    rec = &sRec[sRecording];
    if (rec->overflow)
    {
        return;
    }
    if (st->texture != 0 && st->texture <= TEX_MAX && sTex[st->texture - 1].live)
    {
        tex = &sTex[st->texture - 1];
        sTex[st->texture - 1].used = rec->id;
    }
    state = state_index(rec, st);
    if (state == 0xFFFFFFFFu || !vtx_reserve(rec, n))
    {
        rec->overflow = 1;
        port_log("c3d: out of memory recording a frame");
        return;
    }
    for (i = 0; i < n; i++)
    {
        vtx_convert(&rec->vtx[rec->vtx_num + i], &vtx[(count == 4) ? fan[i] : i], st, tex);
    }
    last = (rec->batch_num != 0) ? &rec->batch[rec->batch_num - 1] : NULL;
    if (last != NULL && last->kind == kind && last->state == state && last->first + last->count == rec->vtx_num &&
        memcmp(last->viewport, sViewport, sizeof(sViewport)) == 0 && memcmp(last->scissor, sScissor, sizeof(sScissor)) == 0)
    {
        last->count += n;
    }
    else
    {
        Batch *b;

        if (!grow((void **)&rec->batch, &rec->batch_cap, rec->batch_num + 1, sizeof(Batch)))
        {
            rec->overflow = 1;
            return;
        }
        b = &rec->batch[rec->batch_num++];
        b->state = state;
        b->first = rec->vtx_num;
        b->count = n;
        b->kind = (unsigned char)kind;
        memcpy(b->viewport, sViewport, sizeof(sViewport));
        memcpy(b->scissor, sScissor, sizeof(sScissor));
    }
    rec->vtx_num += n;
}

/* ---- interface (game side) ------------------------------------------------------- */
void port_gpu_frame_begin(void)
{
    Recording *rec;

    /* any recording but the one the GPU may be reading */
    sRecording = (sShown == 0) ? 1 : 0;
    rec = &sRec[sRecording];
    if (sLatest == sRecording)
    {
        sLatest = -1;
    }
    rec->id = sNextId++;
    rec->vtx_num = rec->batch_num = rec->state_num = 0;
    rec->overflow = 0;
    sTaskStart = svcGetSystemTick();
    sViewport[0] = 0.0f; sViewport[1] = 0.0f; sViewport[2] = N64_W; sViewport[3] = N64_H;
    sScissor[0] = 0; sScissor[1] = 0; sScissor[2] = (int)N64_W; sScissor[3] = (int)N64_H;
}

void port_gpu_frame_end(void)
{
    static unsigned int frames;

    if (sRecording >= 0)
    {
        sLatest = sRecording;
        sRecording = -1;
        sTaskTicks += svcGetSystemTick() - sTaskStart;
        sTaskFrames++;
    }
    if (gPortVerbose && sTaskFrames >= 60)
    {
        port_log("c3d: display lists %.1f ms per frame, %u vertices, %u batches, %u textures made",
                 (double)sTaskTicks * 1000.0 / SYSCLOCK_ARM11 / sTaskFrames, sRec[sLatest].vtx_num, sRec[sLatest].batch_num,
                 sStatCreates);
        sStatCreates = 0;
        port_log("c3d: per frame: vertices %.1f ms (%llu loads), texture loads %.1f ms (%llu), texture lookups %.1f ms (%llu), triangles %.1f ms (%llu commands)",
                 (double)gPortGfxProfile[0] * 1000.0 / SYSCLOCK_ARM11 / 60, gPortGfxProfile[4] / 60,
                 (double)gPortGfxProfile[1] * 1000.0 / SYSCLOCK_ARM11 / 60, gPortGfxProfile[5] / 60,
                 (double)gPortGfxProfile[2] * 1000.0 / SYSCLOCK_ARM11 / 60, gPortGfxProfile[6] / 60,
                 (double)gPortGfxProfile[3] * 1000.0 / SYSCLOCK_ARM11 / 60, gPortGfxProfile[7] / 60);
        memset(gPortGfxProfile, 0, sizeof(gPortGfxProfile));
        sTaskTicks = 0;
        sTaskFrames = 0;
    }
    if ((++frames % 300) == 0 && (gPortVerbose || sStatFails != 0))
    {
        port_log("c3d: %u textures made, %u evicted, %u failures in 300 frames; %u KB cached",
                 sStatCreates, sStatEvicts, sStatFails, sTexBytes >> 10);
        sStatCreates = sStatEvicts = sStatFails = 0;
    }
}

void port_gpu_host_idle(void)
{
}

void port_gpu_set_target_rect(float x, float y, float width, float height)
{
    sRect[0] = x;
    sRect[1] = y;
    sRect[2] = width;
    sRect[3] = height;
}

void port_gpu_viewport(float x, float y, float width, float height)
{
    sViewport[0] = x; sViewport[1] = y; sViewport[2] = width; sViewport[3] = height;
}

void port_gpu_scissor(int x0, int y0, int x1, int y1)
{
    sScissor[0] = x0; sScissor[1] = y0; sScissor[2] = x1; sScissor[3] = y1;
}

void port_gpu_draw(const PortGpuState *state, const PortGpuVtx *vtx, int count)
{
    record(state, vtx, count, BATCH_DRAW);
}

void port_gpu_clear_depth(int x0, int y0, int x1, int y1)
{
    PortGpuState st;
    PortGpuVtx v[4];
    float vp[4];
    int i;

    memset(&st, 0, sizeof(st));
    st.cycles = 1;
    st.z_write = 1;
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
    memcpy(vp, sViewport, sizeof(vp));
    port_gpu_viewport(0.0f, 0.0f, N64_W, N64_H);
    record(&st, v, 4, BATCH_CLEAR_DEPTH);
    port_gpu_viewport(vp[0], vp[1], vp[2], vp[3]);
}

/* ---- combiner -> TEV ------------------------------------------------------------- */
/* An input of a TEV operation: a GPU source, or a constant (folded on the CPU when it can). */
enum { V_CONST, V_TEX, V_SHADE, V_COMB, V_PREV };

typedef struct TevVal
{
    unsigned char kind;
    unsigned char alpha; /* use the source's alpha (colour operations) */
    float k[3];          /* V_CONST; alpha operations use k[0] */
} TevVal;

typedef struct TevOp
{
    unsigned char func;
    unsigned char nsrc;
    TevVal src[3];
} TevOp;

#define MAX_OPS 8

typedef struct TevList
{
    TevOp op[MAX_OPS];
    int num;
    int channels; /* 3 colour, 1 alpha */
} TevList;

static TevVal val_const3(float r, float g, float b)
{
    TevVal v;

    v.kind = V_CONST; v.alpha = 0;
    v.k[0] = r; v.k[1] = g; v.k[2] = b;
    return v;
}

static TevVal val_src(unsigned char kind, unsigned char alpha)
{
    TevVal v;

    v.kind = kind; v.alpha = alpha;
    v.k[0] = v.k[1] = v.k[2] = 0.0f;
    return v;
}

static int is_const(const TevVal *v) { return v->kind == V_CONST; }

static int const_is(const TevVal *v, float value, int channels)
{
    int i;

    if (v->kind != V_CONST) return 0;
    for (i = 0; i < channels; i++)
    {
        if (v->k[i] != value) return 0;
    }
    return 1;
}

static int same_val(const TevVal *a, const TevVal *b, int channels)
{
    int i;

    if (a->kind != b->kind || a->alpha != b->alpha) return 0;
    if (a->kind != V_CONST) return 1;
    for (i = 0; i < channels; i++)
    {
        if (a->k[i] != b->k[i]) return 0;
    }
    return 1;
}

/* N64 combiner input -> TevVal. Constants come from the state's prim / env colours. */
static TevVal val_from_input(unsigned char in, const PortGpuState *st, int alpha_op)
{
    const float n = 1.0f / 255.0f;
    float pa = st->prim[3] * n, ea = st->env[3] * n;

    if (alpha_op)
    {
        switch (in)
        {
        case PORT_GPU_IN_ONE: return val_const3(1.0f, 1.0f, 1.0f);
        case PORT_GPU_IN_COMBINED: return val_src(V_COMB, 1);
        case PORT_GPU_IN_TEXEL: return val_src(V_TEX, 1);
        case PORT_GPU_IN_PRIM: return val_const3(pa, pa, pa);
        case PORT_GPU_IN_SHADE: return val_src(V_SHADE, 1);
        case PORT_GPU_IN_ENV: return val_const3(ea, ea, ea);
        default: return val_const3(0.0f, 0.0f, 0.0f);
        }
    }
    switch (in)
    {
    case PORT_GPU_IN_ONE: return val_const3(1.0f, 1.0f, 1.0f);
    case PORT_GPU_IN_COMBINED: return val_src(V_COMB, 0);
    case PORT_GPU_IN_TEXEL: return val_src(V_TEX, 0);
    case PORT_GPU_IN_PRIM: return val_const3(st->prim[0] * n, st->prim[1] * n, st->prim[2] * n);
    case PORT_GPU_IN_SHADE: return val_src(V_SHADE, 0);
    case PORT_GPU_IN_ENV: return val_const3(st->env[0] * n, st->env[1] * n, st->env[2] * n);
    case PORT_GPU_IN_COMBINED_A: return val_src(V_COMB, 1);
    case PORT_GPU_IN_TEXEL_A: return val_src(V_TEX, 1);
    case PORT_GPU_IN_PRIM_A: return val_const3(pa, pa, pa);
    case PORT_GPU_IN_SHADE_A: return val_src(V_SHADE, 1);
    case PORT_GPU_IN_ENV_A: return val_const3(ea, ea, ea);
    default: return val_const3(0.0f, 0.0f, 0.0f);
    }
}

static TevVal fold(const TevVal *a, const TevVal *b, const TevVal *c, int what)
{
    TevVal r = val_const3(0.0f, 0.0f, 0.0f);
    int i;

    for (i = 0; i < 3; i++)
    {
        switch (what)
        {
        case 0: r.k[i] = a->k[i] * b->k[i]; break;                       /* a * b */
        case 1: r.k[i] = a->k[i] * b->k[i] + c->k[i]; break;             /* a * b + c */
        case 2: r.k[i] = a->k[i] - b->k[i]; break;                       /* a - b */
        case 3: r.k[i] = a->k[i] * c->k[i] + b->k[i] * (1.0f - c->k[i]); break; /* lerp */
        default: r.k[i] = a->k[i] + b->k[i]; break;                      /* a + b */
        }
    }
    return r;
}

static void emit(TevList *list, unsigned char func, int nsrc, const TevVal *s0, const TevVal *s1, const TevVal *s2)
{
    const TevVal *src[3] = { s0, s1, s2 };
    TevVal consts[3];
    int nconst = 0, i, j;
    TevOp *op;

    if (list->num >= MAX_OPS - 1)
    {
        return;
    }
    /* one constant per stage: preload a second, different one into "previous" first */
    for (i = 0; i < nsrc; i++)
    {
        if (is_const(src[i]))
        {
            for (j = 0; j < nconst; j++)
            {
                if (same_val(&consts[j], src[i], list->channels)) break;
            }
            if (j == nconst) consts[nconst++] = *src[i];
        }
    }
    op = &list->op[list->num];
    op->func = func;
    op->nsrc = (unsigned char)nsrc;
    for (i = 0; i < nsrc; i++)
    {
        op->src[i] = *src[i];
    }
    if (nconst > 1)
    {
        TevOp *pre = op;
        TevVal prev = val_src(V_PREV, 0);

        op = &list->op[list->num + 1];
        *op = *pre;
        pre->func = GPU_REPLACE;
        pre->nsrc = 1;
        pre->src[0] = consts[1];
        for (i = 0; i < nsrc; i++)
        {
            if (same_val(&op->src[i], &consts[1], list->channels)) op->src[i] = prev;
        }
        list->num++;
    }
    list->num++;
}

static void emit_replace(TevList *l, const TevVal *a)
{
    emit(l, GPU_REPLACE, 1, a, NULL, NULL);
}

static void emit_mul(TevList *l, const TevVal *a, const TevVal *c)
{
    if (is_const(a) && is_const(c)) { TevVal k = fold(a, c, NULL, 0); emit_replace(l, &k); }
    else if (const_is(a, 1.0f, l->channels)) emit_replace(l, c);
    else if (const_is(c, 1.0f, l->channels)) emit_replace(l, a);
    else emit(l, GPU_MODULATE, 2, a, c, NULL);
}

static void emit_add(TevList *l, const TevVal *a, const TevVal *b)
{
    if (is_const(a) && is_const(b)) { TevVal k = fold(a, b, NULL, 4); emit_replace(l, &k); }
    else if (const_is(a, 0.0f, l->channels)) emit_replace(l, b);
    else if (const_is(b, 0.0f, l->channels)) emit_replace(l, a);
    else emit(l, GPU_ADD, 2, a, b, NULL);
}

/* a * c + d */
static void emit_madd(TevList *l, const TevVal *a, const TevVal *c, const TevVal *d)
{
    if (const_is(d, 0.0f, l->channels)) emit_mul(l, a, c);
    else if (const_is(a, 0.0f, l->channels) || const_is(c, 0.0f, l->channels)) emit_replace(l, d);
    else if (const_is(a, 1.0f, l->channels)) emit_add(l, c, d);
    else if (const_is(c, 1.0f, l->channels)) emit_add(l, a, d);
    else if (is_const(a) && is_const(c)) { TevVal k = fold(a, c, NULL, 0); emit_add(l, &k, d); }
    else emit(l, GPU_MULTIPLY_ADD, 3, a, c, d);
}

/* a * c + b * (1 - c) */
static void emit_lerp(TevList *l, const TevVal *a, const TevVal *b, const TevVal *c)
{
    if (is_const(a) && is_const(b) && is_const(c)) { TevVal k = fold(a, b, c, 3); emit_replace(l, &k); }
    else if (const_is(c, 0.0f, l->channels)) emit_replace(l, b);
    else if (const_is(c, 1.0f, l->channels)) emit_replace(l, a);
    else if (const_is(b, 0.0f, l->channels)) emit_mul(l, a, c);
    else if (is_const(c) && is_const(a))
    {
        /* b * (1 - k) + a * k */
        TevVal one_minus = val_const3(1.0f - c->k[0], 1.0f - c->k[1], 1.0f - c->k[2]), ak = fold(a, c, NULL, 0);
        emit_madd(l, b, &one_minus, &ak);
    }
    else if (is_const(c) && is_const(b))
    {
        TevVal bk = val_const3(b->k[0] * (1.0f - c->k[0]), b->k[1] * (1.0f - c->k[1]), b->k[2] * (1.0f - c->k[2]));
        emit_madd(l, a, c, &bk);
    }
    else emit(l, GPU_INTERPOLATE, 3, a, b, c);
}

/* (a - b) * c + d */
static void compile_cycle(TevList *l, TevVal a, TevVal b, TevVal c, TevVal d)
{
    int ch = l->channels;
    TevVal prev = val_src(V_PREV, 0);

    if (const_is(&c, 0.0f, ch) || (const_is(&a, 0.0f, ch) && const_is(&b, 0.0f, ch)) || same_val(&a, &b, ch))
    {
        emit_replace(l, &d);
    }
    else if (const_is(&b, 0.0f, ch))
    {
        emit_madd(l, &a, &c, &d);
    }
    else if (same_val(&b, &d, ch))
    {
        emit_lerp(l, &a, &b, &c);
    }
    else if (is_const(&a) && is_const(&b))
    {
        TevVal k = fold(&a, &b, NULL, 2);
        int i, negative = 0;

        for (i = 0; i < ch; i++)
        {
            if (k.k[i] < 0.0f) negative = 1;
        }
        if (!negative)
        {
            emit_madd(l, &k, &c, &d);
        }
        else
        {
            /* d - |k| * c (per channel signs mixed: approximated) */
            for (i = 0; i < 3; i++) k.k[i] = (k.k[i] < 0.0f) ? -k.k[i] : 0.0f;
            emit_mul(l, &k, &c);
            emit(l, GPU_SUBTRACT, 2, &d, &prev, NULL);
        }
    }
    else if (const_is(&a, 0.0f, ch))
    {
        emit_mul(l, &b, &c);
        emit(l, GPU_SUBTRACT, 2, &d, &prev, NULL);
    }
    else
    {
        emit(l, GPU_SUBTRACT, 2, &a, &b, NULL);
        if (is_const(&c) && is_const(&d) && !same_val(&c, &d, ch))
        {
            emit(l, GPU_MODULATE, 2, &prev, &c, NULL);
            emit_add(l, &prev, &d);
        }
        else
        {
            emit_madd(l, &prev, &c, &d);
        }
    }
}

static u8 to_u8(float v)
{
    return (v <= 0.0f) ? 0 : (v >= 1.0f) ? 255 : (u8)(v * 255.0f + 0.5f);
}

static GPU_TEVSRC tev_source(const TevVal *v)
{
    switch (v->kind)
    {
    case V_TEX: return GPU_TEXTURE0;
    case V_SHADE: return GPU_PRIMARY_COLOR;
    case V_COMB: return GPU_PREVIOUS_BUFFER;
    case V_PREV: return GPU_PREVIOUS;
    default: return GPU_CONSTANT;
    }
}

/* Writes one operation list into consecutive stages from `first`; returns the next stage. */
static void stage_write(C3D_TexEnv *env, const TevOp *op, int alpha, u32 *konst)
{
    GPU_TEVSRC src[3] = { GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS };
    int opnd[3] = { 0, 0, 0 }, i;

    for (i = 0; i < op->nsrc; i++)
    {
        src[i] = tev_source(&op->src[i]);
        if (alpha)
        {
            opnd[i] = GPU_TEVOP_A_SRC_ALPHA;
        }
        else
        {
            opnd[i] = op->src[i].alpha ? GPU_TEVOP_RGB_SRC_ALPHA : GPU_TEVOP_RGB_SRC_COLOR;
        }
        if (op->src[i].kind == V_CONST)
        {
            if (alpha)
            {
                *konst = (*konst & 0x00FFFFFFu) | ((u32)to_u8(op->src[i].k[0]) << 24);
            }
            else
            {
                *konst = (*konst & 0xFF000000u) | to_u8(op->src[i].k[0]) | ((u32)to_u8(op->src[i].k[1]) << 8) |
                         ((u32)to_u8(op->src[i].k[2]) << 16);
            }
        }
    }
    if (alpha)
    {
        C3D_TexEnvSrc(env, C3D_Alpha, src[0], src[1], src[2]);
        C3D_TexEnvOpAlpha(env, opnd[0], opnd[1], opnd[2]);
        C3D_TexEnvFunc(env, C3D_Alpha, op->func);
    }
    else
    {
        C3D_TexEnvSrc(env, C3D_RGB, src[0], src[1], src[2]);
        C3D_TexEnvOpRgb(env, opnd[0], opnd[1], opnd[2]);
        C3D_TexEnvFunc(env, C3D_RGB, op->func);
    }
}

static void apply_combiner(const PortGpuState *st)
{
    static const TevOp passthrough = { GPU_REPLACE, 1, { { V_PREV, 0, { 0.0f, 0.0f, 0.0f } } } };
    int stage = 0, cyc, i, buffer_mask = 0;

    for (cyc = 0; cyc < st->cycles && cyc < 2; cyc++)
    {
        const PortGpuCycle *c = &st->cycle[cyc];
        TevList color, alpha;
        int num;

        color.num = alpha.num = 0;
        color.channels = 3;
        alpha.channels = 1;
        compile_cycle(&color, val_from_input(c->a, st, 0), val_from_input(c->b, st, 0),
                      val_from_input(c->c, st, 0), val_from_input(c->d, st, 0));
        compile_cycle(&alpha, val_from_input(c->aa, st, 1), val_from_input(c->ab, st, 1),
                      val_from_input(c->ac, st, 1), val_from_input(c->ad, st, 1));
        num = (color.num > alpha.num) ? color.num : alpha.num;
        for (i = 0; i < num && stage < 6; i++, stage++)
        {
            C3D_TexEnv *env = C3D_GetTexEnv(stage);
            u32 konst = 0;

            C3D_TexEnvInit(env);
            stage_write(env, (i < color.num) ? &color.op[i] : &passthrough, 0, &konst);
            stage_write(env, (i < alpha.num) ? &alpha.op[i] : &passthrough, 1, &konst);
            C3D_TexEnvColor(env, konst);
        }
        if (cyc == 0 && st->cycles > 1 && stage > 0 && stage <= 4)
        {
            buffer_mask |= 1 << (stage - 1); /* the second cycle reads the first one's result */
        }
    }
    if (st->fog_blend && stage < 6)
    {
        C3D_TexEnv *env = C3D_GetTexEnv(stage++);

        C3D_TexEnvInit(env);
        C3D_TexEnvSrc(env, C3D_RGB, GPU_CONSTANT, GPU_PREVIOUS, GPU_CONSTANT);
        C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
        C3D_TexEnvFunc(env, C3D_RGB, GPU_INTERPOLATE);
        C3D_TexEnvSrc(env, C3D_Alpha, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
        C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
        C3D_TexEnvColor(env, st->fog[0] | ((u32)st->fog[1] << 8) | ((u32)st->fog[2] << 16) | ((u32)st->fog[3] << 24));
    }
    for (; stage < 6; stage++)
    {
        C3D_TexEnvInit(C3D_GetTexEnv(stage));
    }
    C3D_TexEnvBufUpdate(C3D_Both, buffer_mask);
    C3D_TexEnvBufColor(0);
}

static void apply_state(const PortGpuState *st, int kind)
{
    static const GPU_TEXTURE_WRAP_PARAM wrap[3] = { GPU_REPEAT, GPU_MIRRORED_REPEAT, GPU_CLAMP_TO_EDGE };

    if (kind == BATCH_CLEAR_DEPTH)
    {
        C3D_TexEnv *env = C3D_GetTexEnv(0);
        int i;

        C3D_TexEnvInit(env);
        C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
        for (i = 1; i < 6; i++) C3D_TexEnvInit(C3D_GetTexEnv(i));
        C3D_TexEnvBufUpdate(C3D_Both, 0);
        C3D_CullFace(GPU_CULL_NONE);
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ZERO, GPU_ONE, GPU_ZERO, GPU_ONE);
        C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_DEPTH);
        return;
    }
    if (st->texture != 0 && st->texture <= TEX_MAX && sTex[st->texture - 1].live)
    {
        C3D_Tex *tex = &sTex[st->texture - 1].tex;
        GPU_TEXTURE_FILTER_PARAM filter = st->filter ? GPU_LINEAR : GPU_NEAREST;

        C3D_TexSetWrap(tex, wrap[st->wrap_s % 3], wrap[st->wrap_t % 3]);
        C3D_TexSetFilter(tex, filter, filter);
        C3D_TexBind(0, tex);
    }
    apply_combiner(st);
    /* N64 front faces are counter-clockwise; the quarter turn keeps the winding */
    C3D_CullFace((st->cull == PORT_GPU_CULL_BACK) ? GPU_CULL_BACK_CCW : (st->cull == PORT_GPU_CULL_FRONT) ? GPU_CULL_FRONT_CCW : GPU_CULL_NONE);
    if (st->alpha_test)
    {
        C3D_AlphaTest(true, GPU_GEQUAL, 8);
    }
    else
    {
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
    }
    if (st->blend)
    {
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
    }
    else
    {
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    }
    if (st->z_test || st->z_write)
    {
        C3D_DepthTest(true, st->z_test ? GPU_GEQUAL : GPU_ALWAYS, st->z_write ? GPU_WRITE_ALL : GPU_WRITE_COLOR);
    }
    else
    {
        C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    }
}

/* ---- replay (host side, inside the host's frame) ---------------------------------- */
static DVLB_s *sShaderDvlb;
static shaderProgram_s sShader;

static int shader_init(void)
{
    if (sShaderDvlb == NULL)
    {
        sShaderDvlb = DVLB_ParseFile((u32 *)n64port_c3d_shbin, n64port_c3d_shbin_size);
        if (sShaderDvlb == NULL)
        {
            return 0;
        }
        shaderProgramInit(&sShader);
        shaderProgramSetVsh(&sShader, &sShaderDvlb->DVLE[0]);
    }
    return 1;
}

/* The game's picture inside the target, in screen pixels (y down). */
static void picture_rect(int screen_w, int screen_h, float *rect)
{
    if (sRect[2] > 0.0f && sRect[3] > 0.0f)
    {
        memcpy(rect, sRect, sizeof(sRect));
        return;
    }
    rect[3] = (float)screen_h;
    rect[2] = rect[3] * (N64_W / N64_H);
    if (rect[2] > (float)screen_w)
    {
        rect[2] = (float)screen_w;
        rect[3] = rect[2] * (N64_H / N64_W);
    }
    rect[0] = ((float)screen_w - rect[2]) * 0.5f;
    rect[1] = ((float)screen_h - rect[3]) * 0.5f;
}

void port_gpu_c3d_render(int screen_w, int screen_h)
{
    const Recording *rec;
    C3D_AttrInfo *attr;
    C3D_BufInfo *buf;
    float rect[4], kx, ky;
    unsigned int i, last_state = 0xFFFFFFFFu;
    int last_kind = -1;
    const Batch *last_vp = NULL;

    if (sLatest < 0 || !shader_init())
    {
        return;
    }
    sShown = sLatest;
    rec = &sRec[sShown];
    if (rec->vtx_num == 0)
    {
        return;
    }
    picture_rect(screen_w, screen_h, rect);
    kx = rect[2] / N64_W;
    ky = rect[3] / N64_H;

    C3D_BindProgram(&sShader);
    attr = C3D_GetAttrInfo();
    AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 4);
    AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2);
    AttrInfo_AddLoader(attr, 2, GPU_UNSIGNED_BYTE, 4);
    buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, rec->vtx, sizeof(CVtx), 3, 0x210);
    C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
    C3D_StencilTest(false, GPU_ALWAYS, 0, 0xFF, 0xFF);
    C3D_EarlyDepthTest(false, GPU_EARLYDEPTH_GREATER, 0);

    if (gPortVerbose)
    {
        static int logged;

        for (i = 0; i < rec->batch_num && logged < 12; i++)
        {
            const Batch *b = &rec->batch[i];
            const CVtx *v = &rec->vtx[b->first];
            const PortGpuState *st = &rec->state[b->state];

            logged++;
            port_log("c3d: batch %u kind %d first %u count %u tex %u vp %.0f,%.0f %.0fx%.0f | v0 %.3f %.3f %.3f %.3f st %.3f %.3f rgba %u %u %u %u | v1 %.3f %.3f %.3f %.3f st %.3f %.3f",
                     i, b->kind, b->first, b->count, st->texture, b->viewport[0], b->viewport[1], b->viewport[2], b->viewport[3],
                     v[0].x, v[0].y, v[0].z, v[0].w, v[0].s, v[0].t, v[0].r, v[0].g, v[0].b, v[0].a,
                     v[1].x, v[1].y, v[1].z, v[1].w, v[1].s, v[1].t);
        }
    }
    for (i = 0; i < rec->batch_num; i++)
    {
        const Batch *b = &rec->batch[i];

        if (b->state != last_state || b->kind != last_kind)
        {
            apply_state(&rec->state[b->state], b->kind);
            last_state = b->state;
            last_kind = b->kind;
        }
        if (last_vp == NULL || memcmp(last_vp->viewport, b->viewport, sizeof(b->viewport)) != 0 ||
            memcmp(last_vp->scissor, b->scissor, sizeof(b->scissor)) != 0)
        {
            /* screen rectangles (y down) -> the portrait framebuffer: u = screen_h - y, v = screen_w - x */
            float x = rect[0] + b->viewport[0] * kx, y = rect[1] + b->viewport[1] * ky;
            float w = b->viewport[2] * kx, h = b->viewport[3] * ky;
            int sx0 = (int)(rect[0] + b->scissor[0] * kx), sy0 = (int)(rect[1] + b->scissor[1] * ky);
            int sx1 = (int)(rect[0] + b->scissor[2] * kx), sy1 = (int)(rect[1] + b->scissor[3] * ky);

            if (sx1 < sx0) sx1 = sx0;
            if (sy1 < sy0) sy1 = sy0;
            C3D_SetViewport((u32)((float)screen_h - (y + h)), (u32)((float)screen_w - (x + w)), (u32)h, (u32)w);
            C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32)(screen_h - sy1), (u32)(screen_w - sx1), (u32)(screen_h - sy0), (u32)(screen_w - sx0));
            last_vp = b;
        }
        C3D_DrawArrays(GPU_TRIANGLES, (int)b->first, (int)b->count);
    }
    /* hand the GPU back the way the host had it: whole target, no scissor, no buffer updates */
    C3D_SetViewport(0, 0, (u32)screen_h, (u32)screen_w);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    C3D_TexEnvBufUpdate(C3D_Both, 0);
}

#endif /* __3DS__ && PORT_GFX_GPU */
