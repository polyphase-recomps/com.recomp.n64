/*
 * Audio port layer (built when SSB_AUDIO is ON, otherwise port_audio_stub.c stands in).
 *
 * The game's own driver (src/sys/audio.c) and synthesizer (libultra n_audio) run unchanged.
 * What the N64 provided around them is replaced here:
 *
 *  - Bank, sequence and sound effect files are big-endian with 32-bit offsets. Native copies
 *    are built in the game's audio heap when the driver loads them.
 *  - The synthesizer emits a command list for the n_aspMain RSP microcode. The commands are
 *    executed here in C (the "naudio" ABI: fixed work buffer offsets, 184 samples a pass).
 *  - Addresses inside audio commands are 24-bit. Everything the commands reference lives in
 *    the audio heap, so an address is an offset into it (see osVirtualToPhysical in port_io.c).
 *  - Finished buffers go to a queue the host drains once per video frame; a virtual AI FIFO
 *    reports how much is "still playing" so the driver's rate control behaves as on hardware.
 *
 * Recomp mode (PORT_RSP_HOST + PORT_RSP_RECOMP, runtime/recomp) compiles only the command
 * interpreter and the output queue, as native code over the recompiled game's RDRAM (its
 * word-swapped layout, port_gmem.h). Nothing is converted there: commands carry physical
 * addresses, game data (ADPCM bytes, codebooks, loop states, mixed output) is read and written
 * big-endian through the accessors, and DMEM holds native 16-bit samples (bytes at offset ^ 1).
 * The microcode's own state records stay in host order: only it reads them back.
 */
#include <port_types.h>
#include <PR/os.h>
#if !defined(PORT_RSP_RECOMP) /* (the bank and sequence loaders, which recomp mode leaves out) */
#include <PR/libaudio.h>
#endif
#include <PR/abi.h>
#include <port_host.h>
#if !defined(PORT_RSP_RECOMP)
#include <port_game.h>
#endif
#include "port_guest.h"

#if defined(PORT_RSP_RECOMP)
#include "port_gmem.h"

static u32 sCurW0, sCurW1; /* command being executed, for diagnostics */

/* 24-bit physical address -> RDRAM (state records are word aligned, so host order holds) */
static u8 *audio_ram(u32 addr)
{
    return gRecompRdram + (addr & 0x00FFFFFF);
}

/* DMEM holds native 16-bit samples: on a little-endian host a byte's place in its sample is
 * flipped; RDRAM bytes as recomp_layout.h places them */
#define DMEM_BYTE(offset) ((offset) ^ (RECOMP_HOST_BE ? 0u : 1u))
#define RAM_BYTE(p) (*(u8 *)((uintptr_t)(p) ^ RECOMP_XOR8))
#else
#define DMEM_BYTE(offset) (offset)

/* ---- addresses -------------------------------------------------------------------- */
/* Offset 0 must stay distinguishable from NULL. */
#define PORT_AUDIO_ADDR_BIAS 0x1000

static u8 *sHeap;
static u32 sHeapSize;

static u8 *audio_heap(void)
{
    if (sHeap == NULL)
    {
        sHeap = port_game_audio_heap(&sHeapSize);
    }
    return sHeap;
}

sb32 port_audio_is_heap(const void *ptr)
{
    const u8 *heap = audio_heap();

    return (const u8 *)ptr >= heap && (const u8 *)ptr < heap + sHeapSize;
}

u32 port_audio_addr(const void *ptr)
{
    return (u32)((const u8 *)ptr - audio_heap()) + PORT_AUDIO_ADDR_BIAS;
}

static u32 sCurW0, sCurW1; /* command being executed, for diagnostics */

static u8 *audio_ram(u32 addr)
{
    u8 *heap = audio_heap();

    addr &= 0x00FFFFFF;
    if (addr < PORT_AUDIO_ADDR_BIAS || addr - PORT_AUDIO_ADDR_BIAS >= sHeapSize)
    {
        static u8 sNowhere[0x1000];
        static sb32 sWarned;

        if (!sWarned)
        {
            sWarned = TRUE;
            port_log("audio: command %08X %08X references address 0x%X outside the audio heap", sCurW0, sCurW1, addr);
        }
        return sNowhere;
    }
    return heap + (addr - PORT_AUDIO_ADDR_BIAS);
}

/* ---- big-endian file access ---------------------------------------------------------- */
static u32 be32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static u16 be16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }

static const u8 *rom_at(uintptr_t offset, uintptr_t size)
{
    /* Valid until the next rom_at(): each loader below works from one view at a time. */
    return port_rom_view((u32)offset, (u32)size);
}

static void *heap_alloc(ALHeap *heap, u32 size)
{
    void *ptr = alHeapDBAlloc(NULL, 0, heap, 1, size);

    if (ptr == NULL)
    {
        port_fatal("audio: heap exhausted (%u more bytes needed)", size);
    }
    return ptr;
}

/* ---- bank files ------------------------------------------------------------------------ */
typedef struct BankLoad
{
    const u8 *ctl;       /* the .ctl file */
    u32 ctl_size;
    uintptr_t tbl_start; /* ROM offset of the wave table file */
    ALHeap *heap;
    void **made;         /* native object per .ctl offset / 4, so shared objects stay shared */
} BankLoad;

static void **bank_slot(BankLoad *load, u32 offset)
{
    if (offset == 0 || offset >= load->ctl_size)
    {
        port_fatal("audio: bank offset 0x%X is outside the file", offset);
    }
    return &load->made[offset / 4];
}

static ALWaveTable *bank_wavetable(BankLoad *load, u32 offset)
{
    void **slot = bank_slot(load, offset);
    const u8 *src = load->ctl + offset;
    ALWaveTable *wave;

    if (*slot != NULL)
    {
        return *slot;
    }
    wave = heap_alloc(load->heap, sizeof(*wave));
    *slot = wave;
    wave->base = (u8 *)(load->tbl_start + be32(src)); /* a ROM offset; samples are fetched on demand */
    wave->len = be32(src + 4);
    wave->type = src[8];
    wave->flags = 1;
    if (wave->type == AL_ADPCM_WAVE)
    {
        u32 loop = be32(src + 12), book = be32(src + 16);

        if (book != 0)
        {
            const u8 *b = load->ctl + book;
            s32 order = be32(b), npredictors = be32(b + 4), count = order * npredictors * 8, i;
            ALADPCMBook *native = heap_alloc(load->heap, 8 + count * sizeof(s16));

            native->order = order;
            native->npredictors = npredictors;
            for (i = 0; i < count; i++)
            {
                native->book[i] = (s16)be16(b + 8 + i * 2);
            }
            wave->waveInfo.adpcmWave.book = native;
        }
        if (loop != 0)
        {
            const u8 *l = load->ctl + loop;
            ALADPCMloop *native = heap_alloc(load->heap, sizeof(*native));
            s32 i;

            native->start = be32(l);
            native->end = be32(l + 4);
            native->count = be32(l + 8);
            for (i = 0; i < ADPCMFSIZE; i++)
            {
                native->state[i] = (s16)be16(l + 12 + i * 2);
            }
            wave->waveInfo.adpcmWave.loop = native;
        }
    }
    else
    {
        u32 loop = be32(src + 12);

        if (loop != 0)
        {
            const u8 *l = load->ctl + loop;
            ALRawLoop *native = heap_alloc(load->heap, sizeof(*native));

            native->start = be32(l);
            native->end = be32(l + 4);
            native->count = be32(l + 8);
            wave->waveInfo.rawWave.loop = native;
        }
    }
    return wave;
}

static ALSound *bank_sound(BankLoad *load, u32 offset)
{
    void **slot = bank_slot(load, offset), **sub;
    const u8 *src = load->ctl + offset;
    ALSound *sound;
    u32 envelope = be32(src), keymap = be32(src + 4), wave = be32(src + 8);

    if (*slot != NULL)
    {
        return *slot;
    }
    sound = heap_alloc(load->heap, sizeof(*sound));
    *slot = sound;
    sound->samplePan = src[12];
    sound->sampleVolume = src[13];
    sound->flags = 1;

    sub = bank_slot(load, envelope);
    if (*sub == NULL)
    {
        const u8 *e = load->ctl + envelope;
        ALEnvelope *native = heap_alloc(load->heap, sizeof(*native));

        native->attackTime = be32(e);
        native->decayTime = be32(e + 4);
        native->releaseTime = be32(e + 8);
        native->attackVolume = e[12];
        native->decayVolume = e[13];
        *sub = native;
    }
    sound->envelope = *sub;

    sub = bank_slot(load, keymap);
    if (*sub == NULL)
    {
        ALKeyMap *native = heap_alloc(load->heap, sizeof(*native));

        port_memcpy(native, load->ctl + keymap, 6); /* six single bytes */
        *sub = native;
    }
    sound->keyMap = *sub;
    sound->wavetable = bank_wavetable(load, wave);
    return sound;
}

static ALInstrument *bank_instrument(BankLoad *load, u32 offset)
{
    void **slot = bank_slot(load, offset);
    const u8 *src = load->ctl + offset;
    ALInstrument *inst;
    s32 count = (s16)be16(src + 14), i;

    if (*slot != NULL)
    {
        return *slot;
    }
    inst = heap_alloc(load->heap, sizeof(*inst) + (count > 1 ? count - 1 : 0) * sizeof(ALSound *));
    *slot = inst;
    port_memcpy(inst, src, 12); /* volume .. vibDelay: single bytes */
    inst->flags = 1;
    inst->bendRange = (s16)be16(src + 12);
    inst->soundCount = count;
    for (i = 0; i < count; i++)
    {
        inst->soundArray[i] = bank_sound(load, be32(src + 16 + i * 4));
    }
    return inst;
}

ALBank *port_audio_load_bank(uintptr_t ctl_start, uintptr_t ctl_end, uintptr_t tbl_start, ALHeap *heap)
{
    BankLoad load;
    const u8 *src;
    ALBank *bank;
    u32 offset;
    s32 count, i;

    load.ctl_size = (u32)(ctl_end - ctl_start);
    load.ctl = rom_at(ctl_start, load.ctl_size);
    load.tbl_start = tbl_start;
    load.heap = heap;
    load.made = port_arena_alloc((unsigned long long)(load.ctl_size / 4 + 1) * sizeof(void *), 16);

    if (be16(load.ctl) != 0x4231) /* 'B1' */
    {
        port_fatal("audio: bank file at ROM 0x%llX has revision 0x%04X", (unsigned long long)ctl_start, be16(load.ctl));
    }
    /* The driver only uses the first bank of a file. */
    offset = be32(load.ctl + 4);
    src = load.ctl + offset;
    count = (s16)be16(src);
    bank = heap_alloc(heap, sizeof(*bank) + (count > 1 ? count - 1 : 0) * sizeof(ALInstrument *));
    bank->instCount = count;
    bank->flags = 1;
    bank->sampleRate = be32(src + 4);
    bank->percussion = (be32(src + 8) != 0) ? bank_instrument(&load, be32(src + 8)) : NULL;
    for (i = 0; i < count; i++)
    {
        u32 inst = be32(src + 12 + i * 4);

        bank->instArray[i] = (inst != 0) ? bank_instrument(&load, inst) : NULL;
    }
    port_log("audio: bank at ROM 0x%llX: %d instruments, %d Hz", (unsigned long long)ctl_start, count, bank->sampleRate);
    return bank;
}

/* ---- sequences --------------------------------------------------------------------------- */
ALSeqFile *port_audio_load_seqfile(uintptr_t sbk_start, ALHeap *heap)
{
    const u8 *src = rom_at(sbk_start, 4);
    s32 count = (s16)be16(src + 2), i;
    ALSeqFile *file = heap_alloc(heap, sizeof(*file) + (count > 1 ? count - 1 : 0) * sizeof(ALSeqData));

    src = rom_at(sbk_start, 4 + count * 8);
    file->revision = be16(src);
    file->seqCount = count;
    for (i = 0; i < count; i++)
    {
        file->seqArray[i].offset = (u8 *)(sbk_start + be32(src + 4 + i * 8)); /* ROM offset, read on demand */
        file->seqArray[i].len = be32(src + 8 + i * 8);
    }
    return file;
}

/* A compressed MIDI sequence starts with 16 track offsets and the division; the rest is bytes. */
void port_audio_swap_cseq_header(u8 *data)
{
    u32 *words = (u32 *)data;
    s32 i;

    for (i = 0; i < 17; i++)
    {
        words[i] = be32(data + i * 4);
    }
}

/* ---- sound effect (FGM) files -------------------------------------------------------------- */
/* A count followed by that many offsets (from the start of the file) to byte streams. */
uintptr_t *port_audio_load_fgm_package(uintptr_t start, uintptr_t end, s32 *count, ALHeap *heap)
{
    u32 size = (u32)(end - start), i;
    const u8 *src = rom_at(start, size);
    u8 *raw = heap_alloc(heap, size);
    uintptr_t *table;

    port_memcpy(raw, src, size);
    *count = be32(src);
    table = heap_alloc(heap, (*count + 1) * sizeof(uintptr_t));
    for (i = 0; i < (u32)*count; i++)
    {
        table[i] = (uintptr_t)raw + be32(src + 4 + i * 4);
    }
    return table;
}

/* A count followed by 16-byte records: four bytes and three floats. */
uintptr_t *port_audio_load_fgm_params(uintptr_t start, uintptr_t end, s32 *count, ALHeap *heap)
{
    u32 size = (u32)(end - start), i, j;
    const u8 *src = rom_at(start, size);
    u8 *records;

    *count = be32(src);
    records = heap_alloc(heap, *count * 16 + 16);
    for (i = 0; i < (u32)*count; i++)
    {
        const u8 *record = src + 4 + i * 16;

        port_memcpy(records + i * 16, record, 4);
        for (j = 1; j < 4; j++)
        {
            u32 word = be32(record + j * 4);

            port_memcpy(records + i * 16 + j * 4, &word, 4);
        }
    }
    return (uintptr_t *)records;
}
#endif /* !PORT_RSP_RECOMP */

/* ---- n_aspMain command list ------------------------------------------------------------------ */
#define NAUDIO_COUNT     0x170 /* bytes of one channel per pass: 184 samples */
#define NAUDIO_MAIN      0x4F0
#define NAUDIO_MAIN2     0x660
#define NAUDIO_DRY_LEFT  0x9D0
#define NAUDIO_DRY_RIGHT 0xB40
#define NAUDIO_WET_LEFT  0xCB0
#define NAUDIO_WET_RIGHT 0xE20
#define DMEM_SIZE        0x1000

static u8 sDmem[DMEM_SIZE + 0x200]; /* slack: counts are rounded up to whole frames */
static s16 sBook[16 * 8];
static u32 sLoopAddr;
static s16 sDry, sWet, sVol[2], sTarget[2];
static s32 sRate[2];
static s16 sResampleLut[64][4];

#define DMEM16(offset) ((s16 *)(sDmem + ((offset) & (DMEM_SIZE - 1))))

static s16 clamp16(s32 value)
{
    return (value > 32767) ? 32767 : (value < -32768) ? -32768 : (s16)value;
}

static void acmd_adpcm(u32 w0, u32 w1)
{
    u8 *state = audio_ram(w0);
    u32 flags = w1 >> 28;
    s32 count = (((w1 >> 16) & 0xFFF) + 0x1F) & ~0x1F;
    u32 dmemi = NAUDIO_MAIN + ((w1 >> 12) & 0xF);
    u32 dmemo = NAUDIO_MAIN + (w1 & 0xFFF);
    s16 last[16];
    s32 i;

    if (flags & A_INIT)
    {
        port_memset(last, 0, sizeof(last));
    }
    else
    {
#if defined(PORT_RSP_RECOMP)
        if (flags & A_LOOP)
        {
            const u8 *loop = audio_ram(sLoopAddr); /* game data: the wave's loop state */

            for (i = 0; i < 16; i++)
            {
                last[i] = GM_S16(loop + i * 2);
            }
        }
        else
#endif
        port_memcpy(last, (flags & A_LOOP) ? audio_ram(sLoopAddr) : state, sizeof(last));
    }
    port_memcpy(DMEM16(dmemo), last, sizeof(last));
    dmemo += 32;

    while (count > 0)
    {
        u8 code = sDmem[DMEM_BYTE(dmemi++ & (DMEM_SIZE - 1))];
        u32 scale = code >> 4;
        const s16 *book1 = sBook + ((code & 0xF) << 4), *book2 = book1 + 8;
        u32 rshift = (scale < 12) ? 12 - scale : 0;
        s16 frame[16];
        s32 half;

        for (i = 0; i < 8; i++)
        {
            u8 byte = sDmem[DMEM_BYTE(dmemi++ & (DMEM_SIZE - 1))];

            frame[i * 2] = (s16)((s16)((byte & 0xF0) << 8) >> rshift);
            frame[i * 2 + 1] = (s16)((s16)((byte & 0x0F) << 12) >> rshift);
        }
        /* Two groups of eight: each sample is predicted from the two before the group and
         * the residuals already decoded inside it. */
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
        port_memcpy(DMEM16(dmemo), last, sizeof(last));
        dmemo += 32;
        count -= 32;
    }
    port_memcpy(state, last, sizeof(last));
}

static void acmd_resample(u32 w0, u32 w1)
{
    u8 *state = audio_ram(w0);
    u32 flags = w1 >> 30;
    u32 pitch = ((w1 >> 14) & 0xFFFF) << 1;
    u32 ipos = (NAUDIO_MAIN + ((w1 >> 2) & 0xFFF)) / 2 - 4;
    u32 opos = ((w1 & 3) ? NAUDIO_MAIN2 : NAUDIO_MAIN) / 2;
    s16 *samples = (s16 *)sDmem;
    u32 accu = 0, count = NAUDIO_COUNT / 2;
    u16 saved;

    if (flags & A_INIT)
    {
        port_memset(&samples[ipos], 0, 4 * sizeof(s16));
    }
    else
    {
        port_memcpy(&samples[ipos], state, 4 * sizeof(s16));
        port_memcpy(&saved, state + 8, sizeof(saved));
        accu = saved;
    }
    while (count-- != 0)
    {
        const s16 *lut = sResampleLut[(accu >> 10) & 0x3F];
        const s16 *in = &samples[ipos & (DMEM_SIZE / 2 - 1)];

        samples[opos++ & (DMEM_SIZE / 2 - 1)] = clamp16((in[0] * lut[0] + in[1] * lut[1] + in[2] * lut[2] + in[3] * lut[3]) >> 15);
        accu += pitch;
        ipos += accu >> 16;
        accu &= 0xFFFF;
    }
    port_memcpy(state, &samples[ipos & (DMEM_SIZE / 2 - 1)], 4 * sizeof(s16));
    saved = (u16)accu;
    port_memcpy(state + 8, &saved, sizeof(saved));
}

/* What the envelope mixer keeps between passes (the game reserves 80 bytes for it). */
typedef struct EnvState
{
    s16 wet, dry;
    s32 target[2], step[2], value[2];
} EnvState;

static void acmd_envmixer(u32 w0, u32 w1)
{
    u8 *ram = audio_ram(w1);
    EnvState st;
    s16 *in = DMEM16(NAUDIO_MAIN);
    s16 *dl = DMEM16(NAUDIO_DRY_LEFT), *dr = DMEM16(NAUDIO_DRY_RIGHT);
    s16 *wl = DMEM16(NAUDIO_WET_LEFT), *wr = DMEM16(NAUDIO_WET_RIGHT);
    s32 k, c;

    sVol[1] = (s16)w0;
    if ((w0 >> 16) & A_INIT)
    {
        st.wet = sWet;
        st.dry = sDry;
        for (c = 0; c < 2; c++)
        {
            st.step[c] = sRate[c] / 8;
            st.value[c] = (s32)sVol[c] << 16;
            st.target[c] = (s32)sTarget[c] << 16;
        }
    }
    else
    {
        port_memcpy(&st, ram, sizeof(st));
    }
    for (k = 0; k < NAUDIO_COUNT / 2; k++)
    {
        s32 vol[2], sample = in[k];

        for (c = 0; c < 2; c++)
        {
            st.value[c] += st.step[c];
            if ((st.step[c] <= 0) ? (st.value[c] <= st.target[c]) : (st.value[c] >= st.target[c]))
            {
                st.value[c] = st.target[c];
                st.step[c] = 0;
            }
            vol[c] = st.value[c] >> 16;
        }
        dl[k] = clamp16(dl[k] + ((sample * clamp16((vol[0] * st.dry + 0x4000) >> 15)) >> 15));
        dr[k] = clamp16(dr[k] + ((sample * clamp16((vol[1] * st.dry + 0x4000) >> 15)) >> 15));
        wl[k] = clamp16(wl[k] + ((sample * clamp16((vol[0] * st.wet + 0x4000) >> 15)) >> 15));
        wr[k] = clamp16(wr[k] + ((sample * clamp16((vol[1] * st.wet + 0x4000) >> 15)) >> 15));
    }
    port_memcpy(ram, &st, sizeof(st));
}

static void acmd_setvol(u32 w0, u32 w1)
{
    u32 flags = w0 >> 16;

    if (flags & A_VOL)
    {
        if (flags & A_LEFT)
        {
            sVol[0] = (s16)w0;
            sDry = (s16)(w1 >> 16);
            sWet = (s16)w1;
        }
        else
        {
            sTarget[1] = (s16)w0;
            sRate[1] = (s32)w1;
        }
    }
    else
    {
        sTarget[0] = (s16)w0;
        sRate[0] = (s32)w1;
    }
}

static void acmd_mixer(u32 w0, u32 w1)
{
    s16 gain = (s16)w0;
    s16 *in = DMEM16(NAUDIO_MAIN + (w1 >> 16)), *out = DMEM16(NAUDIO_MAIN + (w1 & 0xFFFF));
    s32 k;

    for (k = 0; k < NAUDIO_COUNT / 2; k++)
    {
        out[k] = clamp16(out[k] + ((in[k] * gain) >> 15));
    }
}

static void acmd_polef(u32 w0, u32 w1)
{
    u32 flags = w0 >> 16;
    s32 gain = w0 & 0xFFFF;
    u32 dmem = ((w1 >> 24) == 0) ? NAUDIO_MAIN : NAUDIO_MAIN2;
    u8 *state = audio_ram(w1);
    s16 *buffer = DMEM16(dmem);
    const s16 *h1 = sBook;
    s16 h2[8], h2_before[8];
    s16 l1 = 0, l2 = 0;
    s32 count = (NAUDIO_COUNT + 15) & ~15, i;

    if (!(flags & A_INIT))
    {
        port_memcpy(&l1, state + 4, sizeof(l1));
        port_memcpy(&l2, state + 6, sizeof(l2));
    }
    for (i = 0; i < 8; i++)
    {
        h2_before[i] = sBook[8 + i];
        h2[i] = (s16)(((s32)sBook[8 + i] * gain) >> 14);
    }
    while (count > 0)
    {
        s16 frame[8];

        port_memcpy(frame, buffer, sizeof(frame));
        for (i = 0; i < 8; i++)
        {
            s32 accu = frame[i] * gain, j;

            accu += h1[i] * l1 + h2_before[i] * l2;
            for (j = 0; j < i; j++)
            {
                accu += h2[j] * frame[i - 1 - j];
            }
            buffer[i] = clamp16(accu >> 14);
        }
        l1 = buffer[6];
        l2 = buffer[7];
        buffer += 8;
        count -= 16;
    }
    port_memcpy(state + 4, &l1, sizeof(l1));
    port_memcpy(state + 6, &l2, sizeof(l2));
}

/* N64_AUDIO_TRACE=T: log the commands of tasks T..T+3 (bring-up aid) */
static s32 sTraceFrom = -2;
static u32 sTaskCount;

static void trace_begin_task(void)
{
    if (sTraceFrom == -2)
    {
        s32 from = port_env_int("N64_AUDIO_TRACE");

        sTraceFrom = (from > 0) ? from : -1;
    }
    sTaskCount++;
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

#if defined(PORT_RSP_RECOMP)
void port_audio_run_guest_abi1(u32 cmds, u32 count);

static sb32 sAbi1; /* the game's audio microcode is aspMain (ABI 1), not n_aspMain */

void port_audio_set_abi1(int abi1)
{
    sAbi1 = abi1 != 0;
}

/* The game's osSpTaskStartGo hands over an audio task (OSTask) by its address. */
void port_audio_run_guest_task(u32 task)
{
    const u8 *t = audio_ram(task);
    u32 data = GM_U32(t + 48), count = GM_U32(t + 52) / 8, i; /* t.data_ptr, t.data_size */

    if (sResampleLut[0][1] == 0)
    {
        resample_lut_init();
    }
    trace_begin_task();
    if (sAbi1)
    {
        port_audio_run_guest_abi1(data, count);
        return;
    }
    for (i = 0; i < count; i++)
    {
        const u8 *cmd = audio_ram(data + i * 8);
        u32 w0 = GM_U32(cmd), w1 = GM_U32(cmd + 4);
#else
extern long long int aspMainTextStart[];

void port_audio_run_task(OSTask *task)
{
    Acmd *cmd = (Acmd *)task->t.data_ptr;
    u32 count = task->t.data_size / sizeof(Acmd), i;

    if (sResampleLut[0][1] == 0)
    {
        resample_lut_init();
    }
    trace_begin_task();
    if (task->t.ucode == (u64 *)aspMainTextStart)
    {
        port_audio_abi1_run(cmd, count);
        return;
    }
    for (i = 0; i < count; i++, cmd++)
    {
        u32 w0 = cmd->words.w0, w1 = cmd->words.w1;
#endif

        sCurW0 = w0;
        sCurW1 = w1;
        if (sTraceFrom >= 0 && sTaskCount >= (u32)sTraceFrom && sTaskCount < (u32)sTraceFrom + 4)
        {
            port_log("acmd: task %u cmd %u %08X %08X", sTaskCount, i, w0, w1);
        }

        switch (w0 >> 24)
        {
        case A_ADPCM:
            acmd_adpcm(w0, w1);
            break;
        case A_CLEARBUFF:
        {
            u32 dmem = (NAUDIO_MAIN + (w0 & 0xFFFF)) & (DMEM_SIZE - 1), size = w1 & 0xFFF;

            if (dmem + size > DMEM_SIZE)
            {
                size = DMEM_SIZE - dmem;
            }
            port_memset(sDmem + dmem, 0, size);
            break;
        }
        case A_ENVMIXER:
            acmd_envmixer(w0, w1);
            break;
        case A_LOADBUFF:
        case A_SAVEBUFF:
        {
            u32 size = ((w0 >> 12) & 0xFFF), dmem = (NAUDIO_MAIN + (w0 & 0xFFF)) & (DMEM_SIZE - 1);
            u8 *ram = audio_ram(w1);

            if (dmem + size > DMEM_SIZE)
            {
                size = DMEM_SIZE - dmem;
            }
#if defined(PORT_RSP_RECOMP)
            {
                u32 k;

                for (k = 0; k < size; k++)
                {
                    if ((w0 >> 24) == A_LOADBUFF)
                    {
                        sDmem[DMEM_BYTE(dmem + k)] = GM_U8(ram + k);
                    }
                    else
                    {
                        RAM_BYTE(ram + k) = sDmem[DMEM_BYTE(dmem + k)];
                    }
                }
            }
#else
            if ((w0 >> 24) == A_LOADBUFF)
            {
                port_memcpy(sDmem + dmem, ram, size);
            }
            else
            {
                port_memcpy(ram, sDmem + dmem, size);
            }
#endif
            break;
        }
        case A_RESAMPLE:
            acmd_resample(w0, w1);
            break;
        case A_SETVOL:
            acmd_setvol(w0, w1);
            break;
        case A_DMEMMOVE:
        {
            u32 dmemi = (NAUDIO_MAIN + (w0 & 0xFFFF)) & (DMEM_SIZE - 1), dmemo = (NAUDIO_MAIN + (w1 >> 16)) & (DMEM_SIZE - 1);
            u32 size = ((w1 & 0xFFFF) + 3) & ~3, k;
            u8 tmp[DMEM_SIZE];

            if (size > DMEM_SIZE - dmemi) size = DMEM_SIZE - dmemi;
            if (size > DMEM_SIZE - dmemo) size = DMEM_SIZE - dmemo;
            for (k = 0; k < size; k++) tmp[k] = sDmem[DMEM_BYTE(dmemi + k)]; /* the ranges may overlap */
#if defined(PORT_RSP_RECOMP)
            for (k = 0; k < size; k++) sDmem[DMEM_BYTE(dmemo + k)] = tmp[k];
#else
            port_memcpy(sDmem + dmemo, tmp, size);
#endif
            break;
        }
        case A_LOADADPCM:
        {
            u32 size = w0 & 0xFFFF;

            if (size > sizeof(sBook))
            {
                size = sizeof(sBook);
            }
#if defined(PORT_RSP_RECOMP)
            {
                const u8 *book = audio_ram(w1); /* game data */
                u32 k;

                for (k = 0; k < size / 2; k++)
                {
                    sBook[k] = GM_S16(book + k * 2);
                }
            }
#else
            port_memcpy(sBook, audio_ram(w1), size);
#endif
            break;
        }
        case A_MIXER:
            acmd_mixer(w0, w1);
            break;
        case A_INTERLEAVE:
        {
            s16 *out = DMEM16(NAUDIO_MAIN), *left = DMEM16(NAUDIO_DRY_LEFT), *right = DMEM16(NAUDIO_DRY_RIGHT);
            s32 k;

            for (k = 0; k < NAUDIO_COUNT / 2; k++)
            {
                out[k * 2] = left[k];
                out[k * 2 + 1] = right[k];
            }
            break;
        }
        case A_POLEF:
            acmd_polef(w0, w1);
            break;
        case A_SETLOOP:
            sLoopAddr = w1;
            break;
        default: /* A_SPNOOP, A_SEGMENT, A_SETBUFF: nothing to do in this ABI */
            break;
        }
    }
}

/* ---- output ------------------------------------------------------------------------------------ */
#define PORT_AUDIO_RATE 32000 /* until the game sets one */

static s32 sAiRate = PORT_AUDIO_RATE;

void port_audio_set_rate(u32 rate)
{
    if (rate >= 8000 && rate <= 96000)
    {
        sAiRate = (s32)rate;
    }
}
#define PORT_AUDIO_OUT_FRAMES 4096 /* far more than one video frame produces */

static s16 sOut[PORT_AUDIO_OUT_FRAMES * 2];
static s32 sOutFrames;
/* The AI plays one DMA buffer while holding the next: AI_LEN_REG only counts the one playing.
 * Games size their next buffer from that, so the length has to behave the same way. */
static s32 sAiPlaying;   /* samples left in the buffer the virtual DAC is playing */
static s32 sAiNext;      /* samples in the buffer queued behind it */
static s32 sAiDrainFrac; /* the output rate / 60 is not a whole number */

void port_audio_submit(void *samples, u32 size)
{
    s32 frames = size / 4;

    if (samples == NULL || frames <= 0)
    {
        return;
    }
    if (sAiPlaying == 0)
    {
        sAiPlaying = frames;
    }
    else
    {
        sAiNext += frames; /* a full FIFO drops the buffer on hardware; keeping it is kinder */
    }
    if (sOutFrames + frames > PORT_AUDIO_OUT_FRAMES)
    {
        frames = PORT_AUDIO_OUT_FRAMES - sOutFrames; /* the host is not draining; drop the excess */
    }
    port_memcpy(&sOut[sOutFrames * 2], samples, (unsigned long long)frames * 4);
    sOutFrames += frames;
}

#if defined(PORT_RSP_RECOMP)
/* osAiSetNextBuffer: big-endian stereo frames at a physical address in RDRAM */
void port_audio_submit_guest(u32 addr, u32 size)
{
    static s16 sNative[PORT_AUDIO_OUT_FRAMES * 2];
    const u8 *src = audio_ram(addr & ~1u);
    u32 i, count;

    if (size / 4 > PORT_AUDIO_OUT_FRAMES)
    {
        size = PORT_AUDIO_OUT_FRAMES * 4;
    }
    count = size / 2;
    for (i = 0; i < count; i++)
    {
        sNative[i] = GM_S16(src + i * 2);
    }
    port_audio_submit(sNative, size);
}
#endif

/* Bytes left in the buffer being played, as AI_LEN_REG reports them. */
u32 port_audio_ai_length(void)
{
    return (u32)sAiPlaying * 4;
}

void port_audio_frame_begin(void)
{
    /* One video frame of playback leaves the DAC; what the host has not fetched is dropped. */
    s32 drain;

    sAiDrainFrac += sAiRate;
    drain = sAiDrainFrac / 60;
    sAiDrainFrac %= 60;
    while (drain > 0 && sAiPlaying > 0)
    {
        s32 step = (drain < sAiPlaying) ? drain : sAiPlaying;

        sAiPlaying -= step;
        drain -= step;
        if (sAiPlaying == 0)
        {
            sAiPlaying = sAiNext;
            sAiNext = 0;
        }
    }
    sOutFrames = 0;
}

const short *n64_audio(int *frames, int *sample_rate)
{
    *frames = sOutFrames;
    *sample_rate = sAiRate;
    return sOut;
}
