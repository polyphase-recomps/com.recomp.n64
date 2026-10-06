/*
 * The aspMain audio microcode (standard libultra audio, "ABI 1") in C, for games on libultra's
 * own synthesizer (Snowboard Kids 2 through libmus); port_audio.c hands its command lists here.
 * Addresses are segment-relative like on the RSP; a segment that was never set (the
 * synthesizer sets segment 0 to 0) leaves the full address as it is, which on these hosts is
 * the guest pointer itself.
 *
 * With PORT_RSP_HOST this file is compiled into the host of the wasm guest instead (like
 * port_gfx.c) and reads the guest's big-endian memory through port_gmem.h. DMEM is then kept as
 * native 16-bit samples: its bytes are addressed with the low bit flipped (DMEM_BYTE) and swapped
 * on the way in from and out to RDRAM. The microcode's own state records in RDRAM (filter
 * histories, envelope state) are only ever read back by it, so they stay in host order.
 */
#include <port_types.h>
#include <PR/abi.h>
#include <port_host.h>
#include "port_guest.h"
#include "port_gmem.h"

#define DMEM_SIZE 0x1000

#if defined(PORT_RSP_HOST) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define DMEM_SWAPPED 1
#define DMEM_BYTE(offset) ((offset) ^ 1)
#else
#define DMEM_SWAPPED 0
#define DMEM_BYTE(offset) (offset)
#endif

static u8 sDmem[DMEM_SIZE + 0x200] __attribute__((aligned(4))); /* slack: counts are rounded up to whole frames */
static s16 sResampleLut[64][4];
static u32 sCurW0, sCurW1; /* command being executed, for diagnostics */

static s16 clamp16(s32 value)
{
    return (value > 32767) ? 32767 : (value < -32768) ? -32768 : (s16)value;
}

/* RDRAM <-> DMEM (dmem and size are multiples of 4) */
static void dmem_transfer(u32 dmem, u8 *ram, u32 size, sb32 load)
{
#if GM_SWAPPED
    /* recomp RDRAM: word-swapped, so go byte by byte through the layout */
    u32 i;

    for (i = 0; i < size; i++)
    {
        if (load)
        {
            sDmem[DMEM_BYTE(dmem + i)] = GM_U8(ram + i);
        }
        else
        {
            *(u8 *)((uintptr_t)(ram + i) ^ 3) = sDmem[DMEM_BYTE(dmem + i)];
        }
    }
#elif DMEM_SWAPPED
    u32 i;

    for (i = 0; i < size; i += 2)
    {
        if (load)
        {
            sDmem[dmem + i] = ram[i + 1];
            sDmem[dmem + i + 1] = ram[i];
        }
        else
        {
            ram[i] = sDmem[dmem + i + 1];
            ram[i + 1] = sDmem[dmem + i];
        }
    }
#else
    if (load)
    {
        port_memcpy(sDmem + dmem, ram, size);
    }
    else
    {
        port_memcpy(ram, sDmem + dmem, size);
    }
#endif
}

/* 16-bit values from RDRAM: game data (big-endian) or one of the microcode's own records */
static void ram_load_s16(s16 *dst, const u8 *ram, u32 count, sb32 game_data)
{
#if defined(PORT_RSP_HOST)
    u32 i;

    if (game_data)
    {
        for (i = 0; i < count; i++)
        {
            dst[i] = GM_S16(ram + i * 2);
        }
        return;
    }
#endif
    port_memcpy(dst, ram, count * 2);
}

#define ABI1_DMEM_BASE 0x5C0

static u32 sSeg[64];
static struct
{
    u16 in, out, count, dry_right, wet_left, wet_right;
    s16 dry, wet, vol[2], target[2];
    s32 rate[2];
    u32 loop;
    s16 table[16 * 8];
} sA1;

static u8 *abi1_ram(u32 addr)
{
    u32 seg = (addr >> 24) & 0x3F;

    if (sSeg[seg] != 0)
    {
        addr = sSeg[seg] + (addr & 0xFFFFFF);
    }
#if defined(PORT_RSP_HOST)
    return (u8 *)GM_PTR(addr);
#endif
#if defined(GEKKO)
    /* K0_TO_PHYS() addresses: GameCube / Wii RAM is mapped cached at 0x80000000 like the N64's. */
    if (addr < 0x20000000)
    {
        addr |= 0x80000000;
    }
#endif
    return (u8 *)(uintptr_t)addr;
}

static u32 align_up(u32 v, u32 a)
{
    return (v + a - 1) & ~(a - 1);
}

static s16 *dmem_s16(u32 offset)
{
    return (s16 *)(sDmem + (offset & (DMEM_SIZE - 1)));
}

static void sample_mix(s16 *dst, s16 src, s16 gain)
{
    *dst = clamp16(*dst + ((src * gain) >> 15));
}

static void abi1_adpcm(u32 w0, u32 w1)
{
    u32 flags = (w0 >> 16) & 0xFF;
    u8 *state = abi1_ram(w1);
    s32 count = (s32)align_up(sA1.count, 32);
    u32 dmemi = sA1.in, dmemo = sA1.out;
    s16 last[16];
    s32 i;

    if (flags & A_INIT)
    {
        port_memset(last, 0, sizeof(last));
    }
    else
    {
        ram_load_s16(last, (flags & A_LOOP) ? abi1_ram(sA1.loop) : state, 16, (flags & A_LOOP) != 0);
    }
    port_memcpy(dmem_s16(dmemo), last, sizeof(last));
    dmemo += 32;
    while (count > 0)
    {
        u8 code = sDmem[DMEM_BYTE(dmemi++ & (DMEM_SIZE - 1))];
        u32 scale = code >> 4;
        const s16 *book1 = sA1.table + ((code & 0xF) << 4), *book2 = book1 + 8;
        u32 rshift = (scale < 12) ? 12 - scale : 0;
        s16 frame[16];
        s32 half;

        for (i = 0; i < 8; i++)
        {
            u8 byte = sDmem[DMEM_BYTE(dmemi++ & (DMEM_SIZE - 1))];

            frame[i * 2] = (s16)((s16)((byte & 0xF0) << 8) >> rshift);
            frame[i * 2 + 1] = (s16)((s16)((byte & 0x0F) << 12) >> rshift);
        }
        for (half = 0; half < 2; half++)
        {
            const s16 *src = frame + half * 8;
            s16 l1 = half ? last[6] : last[14], l2 = half ? last[7] : last[15];
            s16 *dst = last + half * 8;

            for (i = 0; i < 8; i++)
            {
                s32 accu = (s32)src[i] << 11, j;

                accu += book1[i] * l1 + book2[i] * l2;
                for (j = 0; j < i; j++)
                {
                    accu += book2[j] * src[i - 1 - j];
                }
                dst[i] = clamp16(accu >> 11);
            }
        }
        port_memcpy(dmem_s16(dmemo), last, sizeof(last));
        dmemo += 32;
        count -= 32;
    }
    port_memcpy(state, last, sizeof(last));
}

static void abi1_resample(u32 w0, u32 w1)
{
    u32 flags = (w0 >> 16) & 0xFF;
    u32 pitch = (w0 & 0xFFFF) << 1;
    u8 *state = abi1_ram(w1);
    s16 *samples = (s16 *)sDmem;
    u32 ipos = (sA1.in >> 1) - 4, opos = sA1.out >> 1;
    u32 count = align_up(sA1.count, 16) >> 1, accu = 0, k;
    u16 saved;

    if (flags & A_INIT)
    {
        for (k = 0; k < 4; k++)
        {
            samples[(ipos + k) & (DMEM_SIZE / 2 - 1)] = 0;
        }
    }
    else
    {
        for (k = 0; k < 4; k++)
        {
            port_memcpy(&samples[(ipos + k) & (DMEM_SIZE / 2 - 1)], state + k * 2, 2);
        }
        port_memcpy(&saved, state + 8, sizeof(saved));
        accu = saved;
    }
    while (count-- != 0)
    {
        const s16 *lut = sResampleLut[(accu >> 10) & 0x3F];
        s32 sum = 0;

        for (k = 0; k < 4; k++)
        {
            sum += samples[(ipos + k) & (DMEM_SIZE / 2 - 1)] * lut[k];
        }
        samples[opos++ & (DMEM_SIZE / 2 - 1)] = clamp16(sum >> 15);
        accu += pitch;
        ipos += accu >> 16;
        accu &= 0xFFFF;
    }
    for (k = 0; k < 4; k++)
    {
        port_memcpy(state + k * 2, &samples[(ipos + k) & (DMEM_SIZE / 2 - 1)], 2);
    }
    saved = (u16)accu;
    port_memcpy(state + 8, &saved, sizeof(saved));
}

/* Exponential volume ramps, kept between passes in the voice's 80-byte state. */
typedef struct Abi1EnvState
{
    s16 wet, dry;
    s32 target[2], rate[2], seq[2], value[2];
} Abi1EnvState;

static s16 ramp_step(s32 *value, s32 *step, s32 target)
{
    *value += *step;
    if ((*step <= 0) ? (*value <= target) : (*value >= target))
    {
        *value = target;
        *step = 0;
    }
    return (s16)(*value >> 16);
}

static void abi1_envmixer(u32 w0, u32 w1)
{
    u32 flags = (w0 >> 16) & 0xFF;
    u8 *ram = abi1_ram(w1);
    sb32 aux = (flags & A_AUX) != 0;
    s16 *in = dmem_s16(sA1.in);
    s16 *dl = dmem_s16(sA1.out), *dr = dmem_s16(sA1.dry_right);
    s16 *wl = dmem_s16(sA1.wet_left), *wr = dmem_s16(sA1.wet_right);
    Abi1EnvState st;
    s32 step[2], y, x, c, ptr = 0;

    if (flags & A_INIT)
    {
        st.wet = sA1.wet;
        st.dry = sA1.dry;
        for (c = 0; c < 2; c++)
        {
            st.value[c] = (s32)sA1.vol[c] << 16;
            st.target[c] = (s32)sA1.target[c] << 16;
            st.rate[c] = sA1.rate[c];
            st.seq[c] = sA1.vol[c] * sA1.rate[c];
        }
    }
    else
    {
        port_memcpy(&st, ram, sizeof(st));
    }
    for (c = 0; c < 2; c++)
    {
        step[c] = st.target[c] - st.value[c];
    }
    for (y = 0; y < sA1.count; y += 16)
    {
        for (c = 0; c < 2; c++)
        {
            if (step[c] != 0)
            {
                st.seq[c] = (s32)(((s64)st.seq[c] * (s64)st.rate[c]) >> 16);
                step[c] = (st.seq[c] - st.value[c]) >> 3;
            }
        }
        for (x = 0; x < 8; x++, ptr++)
        {
            s16 l = ramp_step(&st.value[0], &step[0], st.target[0]);
            s16 r = ramp_step(&st.value[1], &step[1], st.target[1]);
            s16 sample = in[ptr];

            sample_mix(&dl[ptr], sample, clamp16((l * st.dry + 0x4000) >> 15));
            sample_mix(&dr[ptr], sample, clamp16((r * st.dry + 0x4000) >> 15));
            if (aux)
            {
                sample_mix(&wl[ptr], sample, clamp16((l * st.wet + 0x4000) >> 15));
                sample_mix(&wr[ptr], sample, clamp16((r * st.wet + 0x4000) >> 15));
            }
        }
    }
    port_memcpy(ram, &st, sizeof(st));
}

static void abi1_polef(u32 w0, u32 w1)
{
    u32 flags = (w0 >> 16) & 0xFF;
    s32 gain = (s32)(u16)w0;
    u8 *state = abi1_ram(w1);
    s16 *dst = dmem_s16(sA1.out);
    u32 dmemi = sA1.in;
    s16 *h1 = sA1.table, *h2 = sA1.table + 8;
    s16 h2_before[8], l1 = 0, l2 = 0;
    s32 count = (s32)align_up(sA1.count, 16), i;

    if (!(flags & A_INIT))
    {
        port_memcpy(&l1, state + 4, sizeof(l1));
        port_memcpy(&l2, state + 6, sizeof(l2));
    }
    for (i = 0; i < 8; i++)
    {
        h2_before[i] = h2[i];
        h2[i] = (s16)(((s32)h2[i] * gain) >> 14);
    }
    do
    {
        s16 frame[8];

        for (i = 0; i < 8; i++, dmemi += 2)
        {
            frame[i] = *dmem_s16(dmemi);
        }
        for (i = 0; i < 8; i++)
        {
            s32 accu = frame[i] * gain, j;

            accu += h1[i] * l1 + h2_before[i] * l2;
            for (j = 0; j < i; j++)
            {
                accu += h2[j] * frame[i - 1 - j];
            }
            dst[i] = clamp16(accu >> 14);
        }
        l1 = dst[6];
        l2 = dst[7];
        dst += 8;
        count -= 16;
    } while (count > 0);
    port_memcpy(state, dst - 4, 8);
}

static void abi1_run(const u8 *cmd, u32 count)
{
    u32 i;

    for (i = 0; i < count; i++, cmd += 8)
    {
        u32 w0 = GM_U32(cmd), w1 = GM_U32(cmd + 4);
        u32 flags = (w0 >> 16) & 0xFF;

        sCurW0 = w0;
        sCurW1 = w1;
        switch (w0 >> 24)
        {
        case A_ADPCM:
            abi1_adpcm(w0, w1);
            break;
        case A_CLEARBUFF:
        {
            u32 dmem = (ABI1_DMEM_BASE + (w0 & 0xFFFF)) & (DMEM_SIZE - 1), size = align_up(w1 & 0xFFF, 16);

            if (dmem + size > DMEM_SIZE) size = DMEM_SIZE - dmem;
            port_memset(sDmem + dmem, 0, size);
            break;
        }
        case A_ENVMIXER:
            abi1_envmixer(w0, w1);
            break;
        case A_LOADBUFF:
        case A_SAVEBUFF:
        {
            u32 dmem = ((w0 >> 24) == A_LOADBUFF ? sA1.in : sA1.out) & ~3u, size = align_up(sA1.count, 4);
            u8 *ram = abi1_ram(w1 & ~3u);

            dmem &= DMEM_SIZE - 1;
            if (dmem + size > DMEM_SIZE) size = DMEM_SIZE - dmem;
            if (sA1.count == 0) break;
            dmem_transfer(dmem, ram, size, (w0 >> 24) == A_LOADBUFF);
            break;
        }
        case A_RESAMPLE:
            abi1_resample(w0, w1);
            break;
        case A_SEGMENT:
            sSeg[(w1 >> 24) & 0x3F] = w1 & 0xFFFFFF;
            break;
        case A_SETBUFF:
            if (flags & A_AUX)
            {
                sA1.dry_right = (u16)(ABI1_DMEM_BASE + (w0 & 0xFFFF));
                sA1.wet_left = (u16)(ABI1_DMEM_BASE + (w1 >> 16));
                sA1.wet_right = (u16)(ABI1_DMEM_BASE + (w1 & 0xFFFF));
            }
            else
            {
                sA1.in = (u16)(ABI1_DMEM_BASE + (w0 & 0xFFFF));
                sA1.out = (u16)(ABI1_DMEM_BASE + (w1 >> 16));
                sA1.count = (u16)w1;
            }
            break;
        case A_SETVOL:
            if (flags & A_AUX)
            {
                sA1.dry = (s16)w0;
                sA1.wet = (s16)w1;
            }
            else
            {
                u32 lr = (flags & A_LEFT) ? 0 : 1;

                if (flags & A_VOL)
                {
                    sA1.vol[lr] = (s16)w0;
                }
                else
                {
                    sA1.target[lr] = (s16)w0;
                    sA1.rate[lr] = (s32)w1;
                }
            }
            break;
        case A_DMEMMOVE:
        {
            u32 dmemi = ABI1_DMEM_BASE + (w0 & 0xFFFF), dmemo = ABI1_DMEM_BASE + (w1 >> 16);
            u32 size = align_up(w1 & 0xFFFF, 16), k;

            for (k = 0; k < size; k++) /* forwards, byte by byte, as the RSP does */
            {
                sDmem[DMEM_BYTE((dmemo + k) & (DMEM_SIZE - 1))] = sDmem[DMEM_BYTE((dmemi + k) & (DMEM_SIZE - 1))];
            }
            break;
        }
        case A_LOADADPCM:
        {
            u32 size = align_up(w0 & 0xFFFF, 8);

            if (size > sizeof(sA1.table)) size = sizeof(sA1.table);
            ram_load_s16(sA1.table, abi1_ram(w1), size / 2, TRUE);
            break;
        }
        case A_MIXER:
        {
            s16 gain = (s16)w0;
            s16 *in = dmem_s16(ABI1_DMEM_BASE + (w1 >> 16)), *out = dmem_s16(ABI1_DMEM_BASE + (w1 & 0xFFFF));
            u32 k, n = align_up(sA1.count, 32) >> 1;

            for (k = 0; k < n; k++)
            {
                sample_mix(&out[k], in[k], gain);
            }
            break;
        }
        case A_INTERLEAVE:
        {
            s16 *out = dmem_s16(sA1.out);
            s16 *left = dmem_s16(ABI1_DMEM_BASE + (w1 >> 16)), *right = dmem_s16(ABI1_DMEM_BASE + (w1 & 0xFFFF));
            u32 k, n = align_up(sA1.count, 16) >> 1;
            s16 tmp[DMEM_SIZE / 2];

            for (k = 0; k < n; k++)
            {
                tmp[k * 2] = left[k];
                tmp[k * 2 + 1] = right[k];
            }
            port_memcpy(out, tmp, n * 4);
            break;
        }
        case A_POLEF:
            if (sA1.count != 0)
            {
                abi1_polef(w0, w1);
            }
            break;
        case A_SETLOOP:
            sA1.loop = w1;
            break;
        default: /* A_SPNOOP */
            break;
        }
    }
}

static void resample_lut_init(void)
{
    s32 i;

    /* Four-tap cubic (Catmull-Rom) interpolation between taps 1 and 2. */
    for (i = 0; i < 64; i++)
    {
        f32 f = i / 64.0F, f2 = f * f, f3 = f2 * f;

        sResampleLut[i][0] = (s16)((-0.5F * f3 + f2 - 0.5F * f) * 32767.0F);
        sResampleLut[i][1] = (s16)((1.5F * f3 - 2.5F * f2 + 1.0F) * 32767.0F);
        sResampleLut[i][2] = (s16)((-1.5F * f3 + 2.0F * f2 + 0.5F * f) * 32767.0F);
        sResampleLut[i][3] = (s16)((0.5F * f3 - 0.5F * f2) * 32767.0F);
    }
}

static void abi1_task(const u8 *cmds, u32 count)
{
    if (sResampleLut[0][1] == 0)
    {
        resample_lut_init();
    }
    abi1_run(cmds, count);
}

#ifdef PORT_RSP_HOST
/* The guest's osSpTaskStartGo hands over an ABI 1 list by its guest address. */
void port_audio_run_guest_abi1(u32 cmds, u32 count)
{
    abi1_task((const u8 *)GM_PTR(cmds), count);
}
#else
void port_audio_abi1_run(const void *cmds, u32 count)
{
    abi1_task((const u8 *)cmds, count);
}
#endif
