/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 The fm1-gb authors */
/* The game: where the ROM is, and its banks for the core (gb_bank_fn). Part of app.c's unity build.
 *
 * On the FM-1 the ROM is in the app slot, after the code, put there when the package was made (tools/fm1gb.py,
 * web/fm1gb.js): the slot's last 64 bytes are a trailer that says where the image starts and what it holds. The
 * image is the ROM as it is, read in place from the flash; or, for a ROM too big for the slot, a bank table and the
 * banks, some kept as they are and some LZ4-compressed, the compressed ones decompressed into a cache in RAM as the
 * game maps them. On the host the ROM is a file (FM1GB_ROM in the environment, or the script's "rom FILE"). */

#define GBR_MAGIC 0x42474D46u                       /* "FMGB" */
#define GBR_VERSION 1u
#define GBR_F_TABLE 1u                              /* a bank table first: entries of (offset, length | GBR_RAW) */
#define GBR_RAW 0x80000000u                         /* this bank as it is (else LZ4) */
typedef struct {
    uint32_t magic, version;
    uint32_t back;                                  /* the image starts this many bytes before the trailer */
    uint32_t image_len;                             /* its bytes */
    uint32_t rom_size;                              /* the ROM's (whole 16 KiB banks) */
    uint32_t image_crc;                             /* CRC-32 of the image */
    uint32_t flags;                                 /* GBR_F_* */
    char name[32];                                  /* the ROM file's name, for the screen */
    uint32_t tcrc;                                  /* CRC-32 of the 60 bytes above */
} gbr_trailer_t;
_Static_assert(sizeof(gbr_trailer_t) == 64, "the trailer is the slot's last 64 bytes");

#define ROM_CACHE 8u                                /* compressed banks held decompressed: 16 KiB each */
static uint8_t rom_cache[ROM_CACHE][0x4000] PLAT_POOL;
static int32_t rom_cache_bank[ROM_CACHE];
static uint32_t rom_cache_used[ROM_CACHE], rom_cache_clock;
static int32_t rom_slot_line[2] = {-1, -1};         /* the cache line each slot of the core reads now */
static const uint8_t *rom_image;                    /* the image (the ROM itself without a table) */
static const uint32_t *rom_table;                   /* (offset, length) a bank: 0 without one */
static uint32_t rom_size, rom_misses, rom_bad;
static char rom_name[33];
static int rom_state;                               /* ROM_* */
enum { ROM_NONE, ROM_OK, ROM_DAMAGED, ROM_BAD_TRAILER };

static uint32_t crc_tab[256];
static void crc_init(void)
{
    uint32_t i, k;
    for (i = 0; i < 256u; i++) {
        uint32_t c = i;
        for (k = 0; k < 8u; k++)
            c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_tab[i] = c;
    }
}
static uint32_t crc32_add(uint32_t c, const uint8_t *p, uint32_t n)   /* zlib's CRC-32: crc32_add(0, ...) */
{
    c = ~c;
    while (n--)
        c = crc_tab[(c ^ *p++) & 0xFFu] ^ (c >> 8);
    return ~c;
}

static const uint8_t *rom_bank(void *user, uint32_t bank, int slot)
{
    uint32_t off, len, i;
    int32_t best = -1, other = rom_slot_line[slot ^ 1];
    (void)user;
    if (!rom_table)
        return rom_image + bank * 0x4000u;
    off = rom_table[2u * bank];
    len = rom_table[2u * bank + 1u];
    if (len & GBR_RAW) {
        rom_slot_line[slot] = -1;
        return rom_image + off;
    }
    for (i = 0; i < ROM_CACHE; i++)
        if (rom_cache_bank[i] == (int32_t)bank) {
            rom_cache_used[i] = ++rom_cache_clock;
            rom_slot_line[slot] = (int32_t)i;
            return rom_cache[i];
        }
    for (i = 0; i < ROM_CACHE; i++)                 /* the line used longest ago, not the other slot's */
        if ((int32_t)i != other && (best < 0 || rom_cache_used[i] < rom_cache_used[best]))
            best = (int32_t)i;
    rom_misses++;
    if (fast_lz4(rom_cache[best], 0x4000u, rom_image + off, len) != 0x4000) {
        rom_bad++;                                  /* (checked whole at the start: never, but never a crash) */
        memset(rom_cache[best], 0xFF, 0x4000u);
    }
    rom_cache_bank[best] = (int32_t)bank;
    rom_cache_used[best] = ++rom_cache_clock;
    rom_slot_line[slot] = (int32_t)best;
    return rom_cache[best];
}

/* the trailer at the slot's end checked, the image and its table found and checked */
static int rom_from_trailer(const gbr_trailer_t *t)
{
    const uint8_t *img;
    uint32_t i;
    if (t->magic != GBR_MAGIC)
        return ROM_NONE;
    if (t->version != GBR_VERSION || t->tcrc != crc32_add(0, (const uint8_t *)t, 60u) || t->back < t->image_len ||
        !t->rom_size || (t->rom_size & 0x3FFFu))
        return ROM_BAD_TRAILER;
#ifndef PLAT_HOST
    if (t->back > FM1B_APP_END - 0x4000u - 0x120u - 0xF24u - sizeof *t)   /* (inside the app slot: app.ld, build.py) */
        return ROM_BAD_TRAILER;
#endif
    img = (const uint8_t *)t - t->back;
    if (crc32_add(0, img, t->image_len) != t->image_crc)
        return ROM_DAMAGED;
    rom_image = img;
    rom_size = t->rom_size;
    rom_table = (t->flags & GBR_F_TABLE) ? (const uint32_t *)(const void *)img : 0;
    if (rom_table)
        for (i = 0; i < rom_size / 0x4000u; i++) {
            uint32_t off = rom_table[2u * i], len = rom_table[2u * i + 1u] & ~GBR_RAW;
            if (off > t->image_len || len > t->image_len - off || ((rom_table[2u * i + 1u] & GBR_RAW) && len != 0x4000u))
                return ROM_DAMAGED;
        }
    for (i = 0; i < 31u && t->name[i]; i++)
        rom_name[i] = t->name[i];
    rom_name[i] = 0;
    return ROM_OK;
}

#ifdef PLAT_HOST
static uint8_t *host_rom;
static int rom_load_file(const char *path)       /* the host: a .gb file, padded to whole banks */
{
    FILE *f = fopen(path, "rb");
    long n;
    const char *base = strrchr(path, '/');
    uint32_t i;
    if (!f)
        return ROM_NONE;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0x8000 || n > 8 * 1024 * 1024) {
        fclose(f);
        return ROM_DAMAGED;
    }
    rom_size = ((uint32_t)n + 0x3FFFu) & ~0x3FFFu;
    free(host_rom);
    host_rom = malloc(rom_size);
    memset(host_rom, 0xFF, rom_size);
    if (fread(host_rom, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        return ROM_DAMAGED;
    }
    fclose(f);
    rom_image = host_rom;
    rom_table = 0;
    base = base ? base + 1 : path;
    for (i = 0; i < 32u && base[i]; i++)
        rom_name[i] = base[i];
    rom_name[i] = 0;
    return ROM_OK;
}
/* the host: the slot's tail as the package holds it (gb/tools/fm1gb.py image), its trailer last, read as the FM-1 does */
static uint8_t *host_img;
static int rom_load_image(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    if (!f)
        return ROM_NONE;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    free(host_img);
    host_img = malloc((size_t)n + 16u);
    if (n < 64 || fread(host_img, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        return ROM_BAD_TRAILER;
    }
    fclose(f);
    return rom_from_trailer((const gbr_trailer_t *)(const void *)(host_img + n - (long)sizeof(gbr_trailer_t)));
}
#else
extern uint8_t _app_xip_end[];                      /* the app slot's end (app.ld; build.py moves it with app_end) */
#endif

static void rom_find(void)
{
    uint32_t i;
    crc_init();
    for (i = 0; i < ROM_CACHE; i++)
        rom_cache_bank[i] = -1;
    rom_slot_line[0] = rom_slot_line[1] = -1;
    rom_image = 0;
    rom_table = 0;
#ifdef PLAT_HOST
    {
        const char *p = getenv("FM1GB_ROM"), *img = getenv("FM1GB_IMAGE");
        if (img && *img)
            rom_state = rom_load_image(img);
        else
            rom_state = p && *p ? rom_load_file(p) : ROM_NONE;
    }
#else
    rom_state = rom_from_trailer((const gbr_trailer_t *)(const void *)(_app_xip_end - sizeof(gbr_trailer_t)));
#endif
}
