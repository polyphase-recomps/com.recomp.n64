/*
 * Host services for the guest side that are the same on every platform: logging, arena
 * bookkeeping, overlays, files and the ROM. What differs (address space, coroutines, fault
 * containment) lives in the port_host_<platform>.c backends behind port_host_plat.h.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port_host.h"
#include "port_host_plat.h"

/* ---- fault containment ---------------------------------------------------
 * Inside an editor a crash in game code must not take the host process down.
 * With containment on, a fatal error or hardware exception raised while a game
 * coroutine is running abandons that coroutine, marks the runtime as faulted
 * and returns control to whoever resumed it. The game is not resumed again. */
int gPortContainFaults;
int gPortFaulted;
int gPortFaultGuard;
int gPortVerbose; /* bring-up aid: backends log what they are doing while this is set */
unsigned long long gPortGfxProfile[8];

#if !defined(__3DS__)
unsigned long long port_ticks(void)
{
    return 0;
}
#endif
/* Bring-up aid: how often the game has gone through the main runtime entry points. A hang
 * report prints them twice; the ones still counting are where a busy loop is going round. */
unsigned int gPortProgress[8];

int port_faulted(void)
{
    return gPortFaulted;
}

/* ---- logging ------------------------------------------------------------- */
static void (*sLogSink)(const char *line);

void port_set_log_sink(void (*sink)(const char *line))
{
    sLogSink = sink;
}

static void log_line(const char *prefix, const char *fmt, va_list args)
{
    char buf[1024];
    int n = snprintf(buf, sizeof(buf), "%s", prefix);
    vsnprintf(buf + n, sizeof(buf) - n, fmt, args);
    if (sLogSink != NULL)
    {
        sLogSink(buf);
    }
    else
    {
        fprintf(stderr, "%s\n", buf);
        fflush(stderr);
    }
}

void port_log(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log_line("[n64] ", fmt, args);
    va_end(args);
}

void port_logv(const char *prefix, const char *fmt, void *args)
{
    log_line(prefix, fmt, *(va_list *)args);
}

void port_fatal(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log_line("[n64] FATAL: ", fmt, args);
    va_end(args);
    if (gPortContainFaults)
    {
        port_plat_coro_abandon(); /* does not return when called from game code */
        return;
    }
    port_plat_abort();
}

/* Integer debug switch from the environment (0 when unset). */
int port_env_int(const char *name)
{
    const char *value = getenv(name);

    return (value != NULL) ? atoi(value) : 0;
}

#ifndef PORT_WASM_HOST /* (the wasm guest keeps its arena and overlays in its own memory) */
/* ---- arena ----------------------------------------------------------------
 * One reservation the backend places where every address inside it can be stored
 * as a 32-bit token (see port_prelude.h). */
#ifndef PORT_ARENA_SIZE
#define PORT_ARENA_SIZE (256ull << 20)
#endif

static unsigned char *sArenaBase;
static unsigned long long sArenaUsed;
static unsigned long long sArenaCommitted;

void *port_arena_alloc(unsigned long long size, unsigned long long align)
{
    unsigned long long start, end;

    if (sArenaBase == NULL)
    {
        sArenaBase = port_plat_arena_reserve(PORT_ARENA_SIZE);
        if (sArenaBase == NULL)
        {
            port_fatal("could not reserve %llu MB for the arena", (unsigned long long)(PORT_ARENA_SIZE >> 20));
        }
    }
    if (align < 16)
    {
        align = 16;
    }
    start = (sArenaUsed + align - 1) & ~(align - 1);
    end = start + size;
    if (end > PORT_ARENA_SIZE)
    {
        port_fatal("arena exhausted (%llu bytes requested)", size);
    }
    if (end > sArenaCommitted)
    {
        unsigned long long commit_end = (end + 0xFFFFF) & ~0xFFFFFull;
        if (commit_end > PORT_ARENA_SIZE)
        {
            commit_end = PORT_ARENA_SIZE;
        }
        if (!port_plat_arena_commit(sArenaBase + sArenaCommitted, commit_end - sArenaCommitted))
        {
            port_fatal("arena commit failed");
        }
        sArenaCommitted = commit_end;
    }
    sArenaUsed = end;
    memset(sArenaBase + start, 0, size);
    return sArenaBase + start;
}

/* Bounds of the module's code and data, from symbols its linker script defines. */
#if defined(__3DS__)
extern char __start__[], __end__[];
#define IMAGE_START __start__
#define IMAGE_END __end__
#elif defined(__ELF__) && !defined(GEKKO)
extern char __ehdr_start[], _end[];
#define IMAGE_START __ehdr_start
#define IMAGE_END _end
#endif

int port_addr_is_native(const void *p)
{
    const unsigned char *addr = p;

    if (sArenaBase != NULL && addr >= sArenaBase && addr < sArenaBase + PORT_ARENA_SIZE)
    {
        return 1;
    }
#ifdef IMAGE_START
    if (addr >= (const unsigned char *)IMAGE_START && addr < (const unsigned char *)IMAGE_END)
    {
        return 1;
    }
#endif
    return 0;
}

void port_arena_reset(void)
{
    sArenaUsed = 0;
}

unsigned long long port_arena_used(void)
{
    return sArenaUsed;
}

#endif

void port_memcpy(void *dst, const void *src, unsigned long long size)
{
    memmove(dst, src, size);
}

void port_memset(void *dst, int value, unsigned long long size)
{
    memset(dst, value, size);
}

/* ---- ROM ------------------------------------------------------------------- */
static unsigned char *sRomData;
static unsigned int sRomSize;

#ifndef PORT_WASM_HOST
/* ---- overlays ------------------------------------------------------------ */
/*
 * Boot-time copy of each overlay's variables. The .bss range is copied as well rather than
 * cleared: tables that hold pointer tokens are initialised at start-up and live there.
 */
static char **sOverlayData;
static char **sOverlayBss;

static void overlay_restore(unsigned int index)
{
    const PortOverlay *ovl = &gPortOverlays[index];

    memcpy(ovl->data_start, sOverlayData[index], (size_t)(ovl->data_end - ovl->data_start));
    memcpy(ovl->bss_start, sOverlayBss[index], (size_t)(ovl->bss_end - ovl->bss_start));
}

static char *overlay_copy(const char *start, const char *end)
{
    char *copy = malloc((size_t)(end - start) + 1);

    memcpy(copy, start, (size_t)(end - start));
    return copy;
}

void port_overlays_reset_all(void)
{
    unsigned int i;

    if (sOverlayData == NULL)
    {
        sOverlayData = malloc(sizeof(*sOverlayData) * gPortOverlaysNum);
        sOverlayBss = malloc(sizeof(*sOverlayBss) * gPortOverlaysNum);
        for (i = 0; i < gPortOverlaysNum; i++)
        {
            sOverlayData[i] = overlay_copy(gPortOverlays[i].data_start, gPortOverlays[i].data_end);
            sOverlayBss[i] = overlay_copy(gPortOverlays[i].bss_start, gPortOverlays[i].bss_end);
        }
        return;
    }
    for (i = 0; i < gPortOverlaysNum; i++)
    {
        overlay_restore(i);
    }
}

void port_overlay_load(unsigned long long rom_start)
{
    unsigned int i;

    for (i = 0; i < gPortOverlaysNum; i++)
    {
        if ((unsigned long long)(uintptr_t)gPortOverlays[i].rom_start == rom_start)
        {
            if (sOverlayData != NULL)
            {
                overlay_restore(i);
            }
            return;
        }
    }
    port_log("overlay at ROM 0x%llX is not in the overlay table", rom_start);
}

#endif

unsigned int port_file_read(const char *path, void *data, unsigned int size)
{
    FILE *f = fopen(path, "rb");
    unsigned int got;

    if (f == NULL)
    {
        return 0;
    }
    got = (unsigned int)fread(data, 1, size, f);
    fclose(f);
    return got;
}

int port_file_write(const char *path, const void *data, unsigned int size)
{
    FILE *f = fopen(path, "wb");
    int ok;

    if (f == NULL)
    {
        return 0;
    }
    ok = fwrite(data, 1, size, f) == size;
    fclose(f);
    return ok;
}

/*
 * Game data comes from one file: either the ROM itself, or a pack made by
 * tools/make_rom_pack.py that holds only the asset ranges of the ROM (no game code), addressed
 * by their original ROM offsets. Either is held in memory or, with PORT_ROM_STREAM (targets
 * short on RAM), left on the storage device and read piece by piece.
 */
#define PORT_ROM_RANGES_MAX 64

typedef struct PortRomRange
{
    unsigned int rom_offset, size, file_offset;
} PortRomRange;

static FILE *sRomFile;
static PortRomRange sRomRanges[PORT_ROM_RANGES_MAX];
static unsigned int sRomRangesNum;
static unsigned char *sRomView; /* scratch for port_rom_view() when streaming */
static unsigned int sRomViewSize;

static unsigned int rom_be32(const unsigned char *p)
{
    return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) | ((unsigned int)p[2] << 8) | p[3];
}

int port_rom_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    unsigned char header[16];
    unsigned int i;
    long size;

    if (f == NULL)
    {
        port_log("cannot open game data '%s'", path);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    memset(header, 0, sizeof(header));
    if (size < 0x1000 || fread(header, 1, sizeof(header), f) != sizeof(header))
    {
        port_log("'%s' is too small to be game data", path);
        fclose(f);
        return 0;
    }
    if (memcmp(header, "N64PAK1", 8) == 0)
    {
        sRomSize = rom_be32(header + 8);
        sRomRangesNum = rom_be32(header + 12);
        if (sRomRangesNum == 0 || sRomRangesNum > PORT_ROM_RANGES_MAX)
        {
            port_log("'%s': bad range count %u", path, sRomRangesNum);
            fclose(f);
            return 0;
        }
        for (i = 0; i < sRomRangesNum; i++)
        {
            unsigned char entry[12];

            if (fread(entry, 1, sizeof(entry), f) != sizeof(entry))
            {
                port_log("'%s': truncated range table", path);
                fclose(f);
                return 0;
            }
            sRomRanges[i].rom_offset = rom_be32(entry);
            sRomRanges[i].size = rom_be32(entry + 4);
            sRomRanges[i].file_offset = rom_be32(entry + 8);
        }
    }
    else if (header[0] == 0x80 && header[1] == 0x37)
    {
        /* A whole ROM: one range covering all of it. */
        sRomSize = (unsigned int)size;
        sRomRangesNum = 1;
        sRomRanges[0].rom_offset = 0;
        sRomRanges[0].size = sRomSize;
        sRomRanges[0].file_offset = 0;
    }
    else
    {
        port_log("'%s' is neither an asset pack nor a big-endian (.z64) N64 ROM", path);
        fclose(f);
        return 0;
    }
    free(sRomData);
    sRomData = NULL;
    if (sRomFile != NULL)
    {
        fclose(sRomFile);
        sRomFile = NULL;
    }
#ifdef PORT_ROM_STREAM
    sRomFile = f;
#else
    fseek(f, 0, SEEK_SET);
    sRomData = malloc(size);
    if (sRomData == NULL || fread(sRomData, 1, size, f) != (size_t)size)
    {
        port_log("cannot read '%s' into memory", path);
        fclose(f);
        return 0;
    }
    fclose(f);
#endif
    return 1;
}

/* Where in the file the ROM bytes [offset, offset + size) are. */
static unsigned int rom_locate(unsigned int offset, unsigned int size)
{
    unsigned int i;

    for (i = 0; i < sRomRangesNum; i++)
    {
        const PortRomRange *range = &sRomRanges[i];

        if (offset >= range->rom_offset && offset - range->rom_offset < range->size)
        {
            if (size > range->size - (offset - range->rom_offset))
            {
                break;
            }
            return range->file_offset + (offset - range->rom_offset);
        }
    }
    port_fatal("game data does not contain ROM range 0x%X + 0x%X", offset, size);
    return 0;
}

void port_rom_read(unsigned int offset, void *dst, unsigned int size)
{
    unsigned int at = rom_locate(offset, size);

    if (sRomData != NULL)
    {
        memcpy(dst, sRomData + at, size);
    }
    else if (sRomFile == NULL || fseek(sRomFile, (long)at, SEEK_SET) != 0 || fread(dst, 1, size, sRomFile) != size)
    {
        port_fatal("game data read failed (0x%X + 0x%X)", offset, size);
    }
}

const unsigned char *port_rom_view(unsigned int offset, unsigned int size)
{
    if (sRomData != NULL)
    {
        return sRomData + rom_locate(offset, size);
    }
    if (size > sRomViewSize)
    {
        free(sRomView);
        sRomView = malloc(size);
        sRomViewSize = size;
    }
    port_rom_read(offset, sRomView, size);
    return sRomView;
}

unsigned int port_rom_size(void)
{
    return sRomSize;
}

