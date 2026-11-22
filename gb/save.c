/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 The fm1-gb authors */
/* Battery saves: the cartridge's RAM (and MBC3's clock) kept in the flash, in XDATA1 above the panel's pair
 * (0xE7000..0xFBFFF), as two copies: the current one is the newer whose head and data check, a save goes to the other
 * (its sectors erased as the data reaches them, its head written last), so a power cut mid-save leaves the last
 * save. Part of app.c's unity build.
 *
 * When: the game switched its RAM off after writing it (how most games end a save): soon after; RAM written and
 * left on (a game that keeps work there): 5 s after the last write, at most once a minute; and nothing is written
 * when the RAM is what the flash holds already. One erase or one 1 KiB write a frame, so the game plays on. */

#define SAV_LO 0xE7000u
#define SAV_HI 0xFC000u
#define SAV_MAGIC 0x56534247u                       /* "GBSV" */
#define SAV_RTC 16u                                 /* MBC3's clock: s m h dl dh, latched, 2 spare, rtc_sub */
typedef struct { uint32_t magic, seq, len, crc, game, rsv[2], hcrc; } sav_head_t;
_Static_assert(sizeof(sav_head_t) == 32, "a copy's head");

static uint8_t sav_buf[32768u + SAV_RTC] PLAT_POOL;   /* what is being written: the RAM when the save began */
static uint32_t sav_len;                            /* the data's bytes (0: the game keeps nothing) */
static uint32_t sav_n;                              /* a copy's bytes, whole sectors */
static uint32_t sav_game;                           /* this game, so another game's save is never loaded */
static uint32_t sav_cur, sav_seq;                   /* the current copy's base (0: none) and sequence */
static uint32_t sav_seen_writes, sav_seen_off, sav_changed_ms, sav_done_ms, sav_count, sav_skipped;
static uint8_t sav_dirty;
static struct {
    uint8_t on;
    uint32_t at, pos, erased, crc;
} sw;

static void sav_snap(uint8_t *dst)                  /* the RAM and the clock, as they are saved */
{
    if (gb.eram_size)
        memcpy(dst, gb.eram, gb.eram_size);
    if (gb.has_rtc) {
        uint8_t *r = dst + gb.eram_size;
        memcpy(r, gb.rtc, 5);
        memcpy(r + 5, gb.rtc_latched, 5);
        r[10] = r[11] = 0;
        memcpy(r + 12, &gb.rtc_sub, 4);
    }
}

static uint32_t sav_check(uint32_t base)            /* a whole copy of this game's: its sequence + 1; else 0 */
{
    const sav_head_t *h = (const sav_head_t *)(const void *)plat_data(base);
    const uint8_t *d = plat_data(base + 32u);
    if (!h || !d || h->magic != SAV_MAGIC || h->hcrc != crc32_add(0, (const uint8_t *)h, 28u) || h->len != sav_len ||
        h->game != sav_game || crc32_add(0, d, h->len) != h->crc)
        return 0;
    return h->seq + 1u;
}

static void sav_init(void)                          /* the game's save, if the flash holds one */
{
    uint8_t id[24];
    uint32_t a, b;
    sav_len = gb.has_battery ? gb.eram_size + (gb.has_rtc ? SAV_RTC : 0u) : 0u;
    sav_n = (32u + sav_len + 0xFFFu) & ~0xFFFu;
    sav_cur = 0;
    sw.on = 0;
    if (!sav_len || SAV_LO + 2u * sav_n > SAV_HI) {
        sav_len = 0;
        return;
    }
    memcpy(id, gb.title, 16);
    memcpy(id + 16, &gb.global_sum, 2);
    id[18] = gb.header_sum;
    id[19] = 0;
    memcpy(id + 20, &gb.eram_size, 4);
    sav_game = crc32_add(0, id, sizeof id);
    a = sav_check(SAV_LO);
    b = sav_check(SAV_LO + sav_n);
    if (a && (!b || (int32_t)(a - b) > 0)) {
        sav_cur = SAV_LO;
        sav_seq = a - 1u;
    } else if (b) {
        sav_cur = SAV_LO + sav_n;
        sav_seq = b - 1u;
    }
    if (sav_cur) {
        const uint8_t *d = plat_data(sav_cur + 32u);
        if (gb.eram_size)
            memcpy(gb.eram, d, gb.eram_size);
        if (gb.has_rtc) {
            memcpy(gb.rtc, d + gb.eram_size, 5);
            memcpy(gb.rtc_latched, d + gb.eram_size + 5u, 5);
            memcpy(&gb.rtc_sub, d + gb.eram_size + 12u, 4);
        }
    }
    sav_seen_writes = gb.eram_writes;
    sav_seen_off = gb.eram_off_count;
    sav_dirty = 0;
}

static int sav_begin(void)                          /* the RAM taken as it is now: 1 a save begun, 0 none needed */
{
    sav_snap(sav_buf);
    sav_dirty = 0;
    if (sav_cur && !memcmp(plat_data(sav_cur + 32u), sav_buf, sav_len)) {
        sav_skipped++;                              /* (the flash holds it already: no wear) */
        return 0;
    }
    sw.at = sav_cur == SAV_LO ? SAV_LO + sav_n : SAV_LO;
    sw.pos = 0;
    sw.erased = sw.at;
    sw.crc = crc32_add(0, sav_buf, sav_len);
    sw.on = 1;
    return 1;
}

static void sav_step(void)                          /* one erase, or one write of 1 KiB, or the head */
{
    uint32_t k;
    if (!sw.on)
        return;
    if (sw.pos < sav_len) {
        k = sav_len - sw.pos;
        if (k > 1024u)
            k = 1024u;
        if (sw.erased < sw.at + 32u + sw.pos + k) {   /* the next sector first */
            if (plat_data_erase(sw.erased)) {
                sw.on = 0;                          /* (the flash refused: the last save stays; try again later) */
                sav_dirty = 1;
                return;
            }
            sw.erased += 0x1000u;
            return;
        }
        if (plat_data_write(sw.at + 32u + sw.pos, sav_buf + sw.pos, k)) {
            sw.on = 0;
            sav_dirty = 1;
            return;
        }
        sw.pos += k;
        return;
    }
    {
        sav_head_t h;
        memset(&h, 0, sizeof h);
        h.magic = SAV_MAGIC;
        h.seq = sav_seq + 1u;
        h.len = sav_len;
        h.crc = sw.crc;
        h.game = sav_game;
        h.hcrc = crc32_add(0, (const uint8_t *)&h, 28u);
        sw.on = 0;
        if (sw.erased == sw.at && plat_data_erase(sw.at))   /* (no data: the head's sector still to erase) */
            return;
        if (plat_data_write(sw.at, &h, sizeof h) || !sav_check(sw.at)) {
            sav_dirty = 1;
            return;
        }
        sav_cur = sw.at;
        sav_seq = h.seq;
        sav_count++;
        sav_done_ms = plat_ms();
    }
}

static void sav_poll(uint32_t now)                  /* each frame: a save due begun, a save under way stepped */
{
    if (!sav_len)
        return;
    if (gb.eram_writes != sav_seen_writes) {
        sav_seen_writes = gb.eram_writes;
        sav_changed_ms = now;
        sav_dirty = 1;
    }
    if (sw.on) {
        sav_step();
        return;
    }
    if (!sav_dirty)
        return;
    if (gb.eram_off_count != sav_seen_off) {        /* the game closed its RAM: a save ended */
        if (now - sav_changed_ms >= 200u) {
            sav_seen_off = gb.eram_off_count;
            sav_begin();
        }
    } else if (now - sav_changed_ms >= 5000u && now - sav_done_ms >= 60000u) {
        sav_begin();
    }
}

static int sav_flush(void)                          /* a save now, whole (the menu, an update): 1 one was written */
{
    int wrote = 0;
    if (!sav_len)
        return 0;
    if (gb.eram_writes != sav_seen_writes) {
        sav_seen_writes = gb.eram_writes;
        sav_dirty = 1;
    }
    if (!sw.on && sav_dirty)
        sav_begin();
    while (sw.on) {
        sav_step();
        wrote = 1;
    }
    sav_seen_off = gb.eram_off_count;
    return wrote;
}
