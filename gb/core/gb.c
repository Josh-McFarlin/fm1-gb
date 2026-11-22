/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 The fm1-gb authors */
/* The Game Boy (DMG) emulator core: gb.h says what it is and how the host drives it.
 *
 * Time is counted in T-cycles from the start of the current frame (LY 0): every timestamp moves back a frame when a
 * new one starts (rebase), so 32 bits never wrap. The CPU runs whole instructions until the next event (a PPU mode
 * change, the timer's overflow, a serial transfer's end, the APU's 512 Hz frame sequencer) or the end of the slice;
 * the events then run in time order. Reads and writes inside an instruction see the time of its start. The timer is
 * worked out from the clock when it is read (no event a tick), the APU catches up when it is touched, and once a
 * frame-sequencer step. */
#include "gb.h"
#if defined __has_include
#if __has_include(<string.h>)
#include <string.h>
#define GB_STRING_H 1
#endif
#endif
#ifndef GB_STRING_H                                 /* (a freestanding build: the FM-1's, felucca-bsp's libc behind them) */
#define memcpy __builtin_memcpy
#define memset __builtin_memset
#endif

#define FZ 0x80u
#define FN 0x40u
#define FH 0x20u
#define FC 0x10u
#define NEVER 0x7FFFFFFF

enum {
    R_P1 = 0x00, R_SB = 0x01, R_SC = 0x02, R_DIV = 0x04, R_TIMA = 0x05, R_TMA = 0x06, R_TAC = 0x07, R_IF = 0x0F,
    R_NR10 = 0x10, R_NR11, R_NR12, R_NR13, R_NR14, R_NR20, R_NR21, R_NR22, R_NR23, R_NR24,
    R_NR30, R_NR31, R_NR32, R_NR33, R_NR34, R_NR40, R_NR41, R_NR42, R_NR43, R_NR44, R_NR50, R_NR51, R_NR52,
    R_WAVE = 0x30,
    R_LCDC = 0x40, R_STAT, R_SCY, R_SCX, R_LY, R_LYC, R_DMA, R_BGP, R_OBP0, R_OBP1, R_WY, R_WX
};
enum { IRQ_VBLANK = 1, IRQ_STAT = 2, IRQ_TIMER = 4, IRQ_SERIAL = 8, IRQ_JOYPAD = 16 };

static void apu_sync(gb_t *g);
static void apu_fs(gb_t *g);

/* ------------------------------------------------------------------------------------------------ the map --- */
static uint32_t pow2_ceil(uint32_t n)
{
    uint32_t p = 1;
    while (p < n)
        p <<= 1;
    return p;
}

static uint32_t bank_of(const gb_t *g, uint32_t n)
{
    uint32_t m = pow2_ceil(g->rom_banks);
    n &= m - 1u;
    if (n >= g->rom_banks)
        n %= g->rom_banks;
    return n;
}

static int eram_direct(const gb_t *g)         /* the RAM read in place (writes always count, through the slow path) */
{
    return g->ram_on && g->eram && g->eram_size >= 0x2000u && g->mbc != MBC_2 && !(g->mbc == MBC_3 && g->ram_bank >= 8u);
}

static uint32_t eram_off(const gb_t *g)
{
    uint32_t bank = g->mbc == MBC_3 ? (g->ram_bank & 3u) : g->ram_bank;
    return (bank * 0x2000u) & (g->eram_size - 1u);
}

static void map_rom(gb_t *g)
{
    uint32_t b0 = bank_of(g, g->rom0_bank), bx = bank_of(g, g->rom_bank), i;
    g->rom0 = g->bank_fn(g->bank_user, b0, 0);
    g->romx = g->bank_fn(g->bank_user, bx, 1);
    for (i = 0; i < 4u; i++) {
        g->rmap[i] = g->rom0 + i * 0x1000u;
        g->rmap[4u + i] = g->romx + i * 0x1000u;
    }
}

static void map_ram(gb_t *g)
{
    if (eram_direct(g)) {
        uint32_t o = eram_off(g);
        g->rmap[0xA] = g->eram + o;
        g->rmap[0xB] = g->eram + o + 0x1000u;
    } else {
        g->rmap[0xA] = g->rmap[0xB] = 0;
    }
    g->wmap[0xA] = g->wmap[0xB] = 0;
}

static void map_fixed(gb_t *g)
{
    g->rmap[0x8] = g->wmap[0x8] = g->vram;
    g->rmap[0x9] = g->wmap[0x9] = g->vram + 0x1000;
    g->rmap[0xC] = g->wmap[0xC] = g->wram;
    g->rmap[0xD] = g->wmap[0xD] = g->wram + 0x1000;
    g->rmap[0xE] = g->wmap[0xE] = g->wram;    /* echo */
    g->rmap[0xF] = 0;
    g->wmap[0xF] = 0;
    for (int i = 0; i < 8; i++)
        g->wmap[i] = 0;
}

static void map_all(gb_t *g)
{
    map_fixed(g);
    map_rom(g);
    map_ram(g);
}

/* ------------------------------------------------------------------------------------------------- events --- */
static void limit_update(gb_t *g)
{
    int32_t t = g->run_end;
    if (g->ppu_next < t)
        t = g->ppu_next;
    if (g->tim_next < t)
        t = g->tim_next;
    if (g->ser_next < t)
        t = g->ser_next;
    if (g->fs_next < t)
        t = g->fs_next;
    g->limit = t;
}

static void rtc_second(gb_t *g)
{
    uint8_t *r = g->rtc;
    uint32_t day;
    if (r[4] & 0x40u)
        return;                                     /* halted */
    r[0] = (uint8_t)((r[0] + 1u) & 63u);
    if (r[0] != 60u)
        return;
    r[0] = 0;
    r[1] = (uint8_t)((r[1] + 1u) & 63u);
    if (r[1] != 60u)
        return;
    r[1] = 0;
    r[2] = (uint8_t)((r[2] + 1u) & 31u);
    if (r[2] != 24u)
        return;
    r[2] = 0;
    day = (uint32_t)r[3] | (uint32_t)(r[4] & 1u) << 8;
    day++;
    if (day > 511u) {
        day = 0;
        r[4] |= 0x80u;                              /* the day counter's carry */
    }
    r[3] = (uint8_t)day;
    r[4] = (uint8_t)((r[4] & 0xFEu) | (day >> 8));
}

static void rebase(gb_t *g, int32_t t)          /* time t becomes 0 */
{
    int32_t lo;
    g->clock -= t;
    g->run_end -= t;
    g->limit -= t;
    if (g->ppu_next != NEVER)
        g->ppu_next -= t;
    if (g->tim_next != NEVER)
        g->tim_next -= t;
    if (g->ser_next != NEVER)
        g->ser_next -= t;
    if (g->fs_next != NEVER)
        g->fs_next -= t;
    g->apu_clock -= t;
    g->line_start -= t;
    g->div_base -= t;
    g->tima_time -= t;
    if (!(g->io[R_TAC] & 4u))
        g->tima_time = g->clock;                    /* (not counting: no need to keep it old) */
    lo = g->tima_time < g->clock ? g->tima_time : g->clock;
    while (g->div_base + 0x100000 <= lo)           /* the divider's phase only matters modulo 2^20 */
        g->div_base += 0x100000;
    if (g->has_rtc) {
        g->rtc_sub += t;
        while (g->rtc_sub >= (int32_t)GB_CLOCK) {
            g->rtc_sub -= (int32_t)GB_CLOCK;
            rtc_second(g);
        }
    }
}

/* ------------------------------------------------------------------------------------------------- timer --- */
static const uint8_t TAC_SHIFT[4] = {10, 4, 6, 8};

static uint32_t tima_incs(const gb_t *g, int32_t t0, int32_t t1)
{
    uint32_t sh = TAC_SHIFT[g->io[R_TAC] & 3u];
    return ((uint32_t)(t1 - g->div_base) >> sh) - ((uint32_t)(t0 - g->div_base) >> sh);
}

static void tima_sync(gb_t *g)
{
    if (g->io[R_TAC] & 4u)
        g->tima_val = (uint8_t)(g->tima_val + tima_incs(g, g->tima_time, g->clock));
    g->tima_time = g->clock;
}

static void tima_schedule(gb_t *g)
{
    if (g->io[R_TAC] & 4u) {
        uint32_t sh = TAC_SHIFT[g->io[R_TAC] & 3u];
        uint32_t m = ((uint32_t)(g->tima_time - g->div_base) >> sh) + 1u + (255u - g->tima_val);
        g->tim_next = g->div_base + (int32_t)(m << sh);
    } else {
        g->tim_next = NEVER;
    }
}

static void tima_overflow(gb_t *g)
{
    g->tima_time = g->tim_next;
    g->tima_val = g->io[R_TMA];
    g->iflag |= IRQ_TIMER;
    tima_schedule(g);
}

static void div_write(gb_t *g)
{
    uint32_t cnt = (uint32_t)(g->clock - g->div_base);
    tima_sync(g);
    if ((g->io[R_TAC] & 4u) && (cnt & (1u << (TAC_SHIFT[g->io[R_TAC] & 3u] - 1u)))) {   /* a falling edge: a tick */
        if (g->tima_val == 0xFFu) {
            g->tima_val = g->io[R_TMA];
            g->iflag |= IRQ_TIMER;
        } else {
            g->tima_val++;
        }
    }
    if (cnt & 0x1000u)                              /* the APU's frame sequencer counts bit 12's falls */
        apu_fs(g);
    g->div_base = g->clock;
    g->tima_time = g->clock;
    tima_schedule(g);
    g->fs_next = g->clock + 8192;
}

/* -------------------------------------------------------------------------------------------------- PPU --- */
static uint32_t TILE_LO[256][2];                /* a byte's 8 bits as 8 bytes (0 / 1), the first pixel (bit 7) first */

static void tables_init(void)
{
    static int done;
    uint32_t b, i;
    if (done)
        return;
    for (b = 0; b < 256u; b++) {
        uint8_t px[8];
        for (i = 0; i < 8u; i++)
            px[i] = (uint8_t)((b >> (7u - i)) & 1u);
        memcpy(TILE_LO[b], px, 8);
    }
    done = 1;
}

static void stat_update(gb_t *g)
{
    uint8_t s = g->io[R_STAT], line = 0;
    if (g->lcd_on) {
        if ((s & 0x40u) && g->ly == g->io[R_LYC])
            line = 1;
        if ((s & 0x08u) && g->mode == 0)
            line = 1;
        if ((s & 0x20u) && g->mode == 2)
            line = 1;
        if ((s & 0x10u) && g->mode == 1)
            line = 1;
        if ((s & 0x20u) && g->mode == 1 && g->ly == 144u)   /* (the OAM interrupt fires at VBlank's start too) */
            line = 1;
    }
    if (line && !g->stat_line)
        g->iflag |= IRQ_STAT;
    g->stat_line = line;
}

/* a tile row's 8 colour indices (0..3) at dst (8-aligned in a uint32_t buffer) */
static inline void tile_px(uint32_t *dst, const uint8_t *row)
{
    const uint32_t *lo = TILE_LO[row[0]], *hi = TILE_LO[row[1]];
    dst[0] = lo[0] | hi[0] << 1;
    dst[1] = lo[1] | hi[1] << 1;
}

static void render_line(gb_t *g)
{
    uint32_t buf32[44];                                 /* 22 tiles of colour indices */
    uint8_t *buf = (uint8_t *)buf32;
    uint8_t bgi[GB_W], pal[4];
    uint8_t *out;
    uint32_t lcdc = g->io[R_LCDC], ly = g->ly, x, i;
    int32_t wx = 160;
    int win = 0;
    if (!g->win_y_hit && ly == g->io[R_WY])
        g->win_y_hit = 1;
    if ((lcdc & 0x20u) && (lcdc & 1u) && g->win_y_hit && g->io[R_WX] <= 166u) {
        win = 1;
        wx = (int32_t)g->io[R_WX] - 7;
    }
    if (g->skip_render || !g->fb) {
        if (win)
            g->win_line++;
        return;
    }
    out = g->fb + ly * GB_W;
    if (lcdc & 1u) {
        const int unsigned_tiles = (lcdc & 0x10u) != 0;
        if (wx > 0) {                                   /* the background, up to the window */
            uint32_t y = (ly + g->io[R_SCY]) & 255u, scx = g->io[R_SCX];
            const uint8_t *map = g->vram + ((lcdc & 0x08u) ? 0x1C00u : 0x1800u) + (y >> 3) * 32u;
            uint32_t row = (y & 7u) * 2u, tx = scx >> 3, n = ((uint32_t)wx + (scx & 7u) + 7u) >> 3;
            if (n > 21u)
                n = 21u;
            for (i = 0; i < n; i++) {
                uint32_t t = map[(tx + i) & 31u];
                const uint8_t *r = g->vram + (unsigned_tiles ? t * 16u : 0x1000u + (uint32_t)((int32_t)(int8_t)t * 16)) + row;
                tile_px(buf32 + 2u * i, r);
            }
            memcpy(bgi, buf + (scx & 7u), (uint32_t)(wx < 160 ? wx : 160));
        }
        if (win) {                                      /* the window from wx on */
            uint32_t wl = g->win_line, start = wx < 0 ? 0u : (uint32_t)wx, skip = wx < 0 ? (uint32_t)-wx : 0u;
            const uint8_t *map = g->vram + ((lcdc & 0x40u) ? 0x1C00u : 0x1800u) + (wl >> 3) * 32u;
            uint32_t row = (wl & 7u) * 2u, n = (160u - start + skip + 7u) >> 3;
            if (n > 21u)
                n = 21u;
            for (i = 0; i < n; i++) {
                uint32_t t = map[i & 31u];
                const uint8_t *r = g->vram + (unsigned_tiles ? t * 16u : 0x1000u + (uint32_t)((int32_t)(int8_t)t * 16)) + row;
                tile_px(buf32 + 2u * i, r);
            }
            memcpy(bgi + start, buf + skip, 160u - start);
            g->win_line++;
        }
        for (i = 0; i < 4u; i++)
            pal[i] = (uint8_t)(GB_PAL_BG | ((g->io[R_BGP] >> (2u * i)) & 3u));
        for (x = 0; x < GB_W; x++)
            out[x] = pal[bgi[x]];
    } else {                                            /* (DMG: background and window off, white) */
        memset(bgi, 0, sizeof bgi);
        memset(out, GB_PAL_BG | (g->io[R_BGP] & 3u), GB_W);
    }
    if (lcdc & 0x02u) {                                 /* the sprites: 10 a line, the leftmost first */
        uint8_t idx[10], done[GB_W + 16];
        uint32_t h = (lcdc & 0x04u) ? 16u : 8u, n = 0, k;
        for (i = 0; i < 40u && n < 10u; i++) {
            uint32_t sy = (uint32_t)g->oam[i * 4u];
            if (ly + 16u >= sy && ly + 16u < sy + h)
                idx[n++] = (uint8_t)i;
        }
        for (i = 1; i < n; i++) {                       /* by x, stable (OAM order on a tie) */
            uint8_t s = idx[i];
            int32_t j = (int32_t)i - 1;
            while (j >= 0 && g->oam[idx[j] * 4u + 1u] > g->oam[s * 4u + 1u]) {
                idx[j + 1] = idx[j];
                j--;
            }
            idx[j + 1] = s;
        }
        memset(done, 0, sizeof done);
        for (k = 0; k < n; k++) {
            const uint8_t *o = g->oam + idx[k] * 4u;
            uint32_t row = ly + 16u - o[0], tile = o[2], attr = o[3], lo, hi, p;
            int32_t sx = (int32_t)o[1] - 8;
            uint8_t opal[4];
            if (attr & 0x40u)
                row = h - 1u - row;
            if (h == 16u)
                tile &= 0xFEu;
            lo = g->vram[tile * 16u + row * 2u];
            hi = g->vram[tile * 16u + row * 2u + 1u];
            for (i = 0; i < 4u; i++)
                opal[i] = (uint8_t)(((attr & 0x10u) ? GB_PAL_OBJ1 : GB_PAL_OBJ0) |
                                    ((g->io[(attr & 0x10u) ? R_OBP1 : R_OBP0] >> (2u * i)) & 3u));
            for (p = 0; p < 8u; p++) {
                int32_t xx = sx + (int32_t)p;
                uint32_t bit = (attr & 0x20u) ? p : 7u - p, c;
                if (xx < 0 || xx >= GB_W || done[xx])
                    continue;
                c = ((lo >> bit) & 1u) | (((hi >> bit) & 1u) << 1);
                if (!c)
                    continue;
                done[xx] = 1;                           /* (a sprite behind the background still hides the ones under it) */
                if ((attr & 0x80u) && bgi[xx])
                    continue;
                out[xx] = opal[c];
            }
        }
    }
}

static void frame_done(gb_t *g)
{
    g->frames++;
    if (g->vblank)
        g->vblank(g);
}

static void ppu_event(gb_t *g)
{
    int32_t t = g->ppu_next;
    if (!g->lcd_on) {                                   /* off: a blank frame now and then, and time moves on */
        rebase(g, t);
        g->ppu_next = GB_FRAME;
        frame_done(g);
        return;
    }
    switch (g->mode) {
    case 2:                                             /* OAM scan done: the line is drawn as the transfer starts */
        g->mode = 3;
        render_line(g);
        g->ppu_next = t + 172;
        stat_update(g);
        break;
    case 3:
        g->mode = 0;
        g->ppu_next = t + 204;
        stat_update(g);
        break;
    case 0:
        g->ly++;
        g->line_start = t;
        if (g->ly == 144u) {
            g->mode = 1;
            g->iflag |= IRQ_VBLANK;
            g->ppu_next = t + GB_LINE;
            stat_update(g);
            frame_done(g);
        } else {
            g->mode = 2;
            g->ppu_next = t + 80;
            stat_update(g);
        }
        break;
    default:                                            /* VBlank */
        g->ly++;
        if (g->ly == 154u) {
            g->ly = 0;
            g->mode = 2;
            g->win_line = 0;
            g->win_y_hit = 0;
            rebase(g, t);
            g->line_start = 0;
            g->ppu_next = 80;
        } else {
            g->line_start = t;
            g->ppu_next = t + GB_LINE;
        }
        stat_update(g);
        break;
    }
}

static void lcdc_write(gb_t *g, uint8_t v)
{
    uint8_t was = g->lcd_on;
    g->io[R_LCDC] = v;
    g->lcd_on = (uint8_t)(v >> 7);
    if (was && !g->lcd_on) {                            /* off: LY 0, mode 0, a white screen */
        g->ly = 0;
        g->mode = 0;
        g->stat_line = 0;
        if (g->fb)
            memset(g->fb, GB_PAL_BG, GB_W * GB_H);
        g->ppu_next = g->clock + GB_FRAME;
    } else if (!was && g->lcd_on) {                     /* on: a new frame from line 0 */
        g->ly = 0;
        g->mode = 2;
        g->win_line = 0;
        g->win_y_hit = 0;
        g->line_start = g->clock;
        g->ppu_next = g->clock + 80;
        stat_update(g);
    }
    limit_update(g);
}

/* -------------------------------------------------------------------------------------------------- APU --- */
static const uint8_t DUTY[4] = {0x01, 0x81, 0x87, 0x7E};
static const uint8_t NOISE_DIV[8] = {8, 16, 32, 48, 64, 80, 96, 112};

static inline void sq_level(struct gb_sq *c)
{
    c->out = ((DUTY[c->duty] >> (7u - c->pos)) & 1u) ? c->vol : 0;
}

static inline int32_t sq_run(struct gb_sq *c, int32_t k)
{
    int32_t acc = 0, t = c->timer, out = c->out;
    while (k >= t) {
        acc += out * t;
        k -= t;
        t = c->period;
        c->pos = (uint8_t)((c->pos + 1u) & 7u);
        out = ((DUTY[c->duty] >> (7u - c->pos)) & 1u) ? c->vol : 0;
    }
    c->timer = t - k;
    c->out = (uint8_t)out;
    return acc + out * k;
}

static inline int32_t wave_run(gb_t *g, struct gb_wave *c, int32_t k)
{
    int32_t acc = 0, t = c->timer, out = c->out;
    while (k >= t) {
        uint8_t b;
        acc += out * t;
        k -= t;
        t = c->period;
        c->pos = (uint8_t)((c->pos + 1u) & 31u);
        b = g->io[R_WAVE + (c->pos >> 1)];
        c->sample = (uint8_t)((c->pos & 1u) ? (b & 15u) : (b >> 4));
        out = c->sample >> c->vol_shift;
    }
    c->timer = t - k;
    c->out = (uint8_t)out;
    return acc + out * k;
}

static inline int32_t noise_run(struct gb_noise *c, int32_t k)
{
    int32_t acc = 0, t = c->timer, out = c->out;
    if (!c->period) {                                   /* (shift 14, 15: the LFSR stands) */
        return out * k;
    }
    while (k >= t) {
        uint32_t x;
        acc += out * t;
        k -= t;
        t = c->period;
        x = (c->lfsr ^ (c->lfsr >> 1)) & 1u;
        c->lfsr = (uint16_t)((c->lfsr >> 1) | (x << 14));
        if (c->narrow)
            c->lfsr = (uint16_t)((c->lfsr & ~0x40u) | (x << 6));
        out = (c->lfsr & 1u) ? 0 : c->vol;
    }
    c->timer = t - k;
    c->out = (uint8_t)out;
    return acc + out * k;
}

static void sample_out(gb_t *g)
{
    uint32_t nr51 = g->io[R_NR51], nr50 = g->io[R_NR50], i;
    int32_t l = 0, r = 0, n = g->acc_n;
    for (i = 0; i < 4u; i++) {
        if (nr51 & (0x10u << i))
            l += g->acc[i];
        if (nr51 & (1u << i))
            r += g->acc[i];
        g->acc[i] = 0;
    }
    l = l * (int32_t)(((nr50 >> 4) & 7u) + 1u) * 96 / n;
    r = r * (int32_t)((nr50 & 7u) + 1u) * 96 / n;
    /* the high-pass: the DMG's output capacitor (y = x - x' + 255/256 y') */
    g->hp_l = l - g->hp_xl + g->hp_l - (g->hp_l >> 8);
    g->hp_xl = l;
    g->hp_r = r - g->hp_xr + g->hp_r - (g->hp_r >> 8);
    g->hp_xr = r;
    l = g->hp_l;
    r = g->hp_r;
    l = l > 32767 ? 32767 : l < -32768 ? -32768 : l;
    r = r > 32767 ? 32767 : r < -32768 ? -32768 : r;
    {
        uint32_t w = g->awr & g->amask;
        g->abuf[2u * w] = (int16_t)l;
        g->abuf[2u * w + 1u] = (int16_t)r;
        g->awr++;
    }
    g->acc_n = 0;
}

static int32_t sample_len(gb_t *g)                       /* the next sample's T-cycles (exact on average) */
{
    int32_t n = (int32_t)(g->smp_step >> 16);
    g->smp_frac += g->smp_step & 0xFFFFu;
    if (g->smp_frac >= (1u << 16)) {
        g->smp_frac -= 1u << 16;
        n++;
    }
    return n;
}

static void apu_sync(gb_t *g)
{
    int32_t n = g->clock - g->apu_clock;
    if (n <= 0)
        return;
    g->apu_clock = g->clock;
    if (!g->abuf)
        return;
    while (n > 0) {
        int32_t k = g->acc_want - g->acc_n;
        if (k > n)
            k = n;
        if (g->ch1.on)
            g->acc[0] += sq_run(&g->ch1, k);
        if (g->ch2.on)
            g->acc[1] += sq_run(&g->ch2, k);
        if (g->ch3.on)
            g->acc[2] += wave_run(g, &g->ch3, k);
        if (g->ch4.on)
            g->acc[3] += noise_run(&g->ch4, k);
        g->acc_n += k;
        n -= k;
        if (g->acc_n >= g->acc_want) {
            sample_out(g);
            g->acc_want = sample_len(g);
        }
    }
}

static void env_step(uint8_t *vol, uint8_t *t, uint8_t per, uint8_t up)
{
    if (!per)
        return;
    if (*t)
        (*t)--;
    if (*t)
        return;
    *t = per;
    if (up && *vol < 15u)
        (*vol)++;
    else if (!up && *vol)
        (*vol)--;
}

static uint32_t sweep_calc(gb_t *g)
{
    struct gb_sq *c = &g->ch1;
    uint32_t d = c->sw_freq >> c->sw_shift, nf;
    if (c->sw_dir) {
        nf = c->sw_freq - d;
        c->sw_neg = 1;
    } else {
        nf = c->sw_freq + d;
    }
    if (nf > 2047u)
        c->on = 0;
    return nf;
}

static void apu_fs(gb_t *g)                             /* the frame sequencer's step (512 Hz) */
{
    uint32_t s = g->fs_step;
    apu_sync(g);
    g->fs_step = (uint8_t)((s + 1u) & 7u);
    if (!g->apu_on)
        return;
    if (!(s & 1u)) {                                    /* lengths: 256 Hz */
        if (g->ch1.len_on && g->ch1.len && !--g->ch1.len)
            g->ch1.on = 0;
        if (g->ch2.len_on && g->ch2.len && !--g->ch2.len)
            g->ch2.on = 0;
        if (g->ch3.len_on && g->ch3.len && !--g->ch3.len)
            g->ch3.on = 0;
        if (g->ch4.len_on && g->ch4.len && !--g->ch4.len)
            g->ch4.on = 0;
    }
    if (s == 2u || s == 6u) {                           /* the sweep: 128 Hz */
        struct gb_sq *c = &g->ch1;
        if (c->sw_t)
            c->sw_t--;
        if (!c->sw_t) {
            c->sw_t = c->sw_per ? c->sw_per : 8u;
            if (c->sw_on && c->sw_per) {
                uint32_t nf = sweep_calc(g);
                if (nf <= 2047u && c->sw_shift) {
                    c->sw_freq = (uint16_t)nf;
                    c->freq = (uint16_t)nf;
                    c->period = (2048 - (int32_t)nf) * 4;
                    sweep_calc(g);
                }
            }
        }
    }
    if (s == 7u) {                                      /* the envelopes: 64 Hz */
        env_step(&g->ch1.vol, &g->ch1.env_t, g->ch1.env_per, g->ch1.env_dir);
        env_step(&g->ch2.vol, &g->ch2.env_t, g->ch2.env_per, g->ch2.env_dir);
        env_step(&g->ch4.vol, &g->ch4.env_t, g->ch4.env_per, g->ch4.env_dir);
        sq_level(&g->ch1);
        sq_level(&g->ch2);
        if (g->ch4.out)
            g->ch4.out = g->ch4.vol;
    }
}

static void noise_period(struct gb_noise *c, uint8_t nr43)
{
    uint32_t s = nr43 >> 4;
    c->narrow = (uint8_t)((nr43 >> 3) & 1u);
    c->period = s >= 14u ? 0 : (int32_t)((uint32_t)NOISE_DIV[nr43 & 7u] << s);
}

static uint8_t apu_read(gb_t *g, uint32_t r)
{
    static const uint8_t OR[0x17] = {0x80, 0x3F, 0x00, 0xFF, 0xBF, 0xFF, 0x3F, 0x00, 0xFF, 0xBF, 0x7F, 0xFF, 0x9F, 0xFF,
                                     0xBF, 0xFF, 0xFF, 0x00, 0x00, 0xBF, 0x00, 0x00, 0x70};
    apu_sync(g);
    if (r >= R_WAVE)
        return g->io[r];
    if (r == R_NR52)
        return (uint8_t)(0x70u | (g->apu_on ? 0x80u : 0u) | (g->ch1.on ? 1u : 0u) | (g->ch2.on ? 2u : 0u) |
                         (g->ch3.on ? 4u : 0u) | (g->ch4.on ? 8u : 0u));
    if (r > R_NR52)
        return 0xFF;
    return (uint8_t)(g->io[r] | OR[r - R_NR10]);
}

/* NRx4's length enable: switched on in the first half of a length period (the next frame-sequencer step clocks no
 * length), the counter is clocked once at once; a trigger with the counter at 0 then loads max - 1 */
static void len_enable(gb_t *g, uint8_t *on, uint32_t *len, uint8_t *len_on, uint8_t v, uint32_t max)
{
    uint8_t was = *len_on;
    *len_on = (uint8_t)((v >> 6) & 1u);
    if ((g->fs_step & 1u) && !was && *len_on && *len) {
        if (!--*len && !(v & 0x80u))
            *on = 0;
    }
    if ((v & 0x80u) && !*len)
        *len = ((g->fs_step & 1u) && *len_on) ? max - 1u : max;
}

static void sq_trigger(gb_t *g, struct gb_sq *c, uint8_t nrx2)
{
    c->on = c->dac;
    if (!c->len)
        c->len = 64;
    c->period = (2048 - (int32_t)c->freq) * 4;
    c->timer = c->period;
    c->vol = nrx2 >> 4;
    c->env_dir = (uint8_t)((nrx2 >> 3) & 1u);
    c->env_per = nrx2 & 7u;
    c->env_t = c->env_per;
    sq_level(c);
    if (c == &g->ch1) {
        c->sw_freq = c->freq;
        c->sw_t = c->sw_per ? c->sw_per : 8u;
        c->sw_on = (uint8_t)(c->sw_per || c->sw_shift);
        c->sw_neg = 0;
        if (c->sw_shift)
            sweep_calc(g);
    }
}

static void apu_write(gb_t *g, uint32_t r, uint8_t v)
{
    apu_sync(g);
    if (r >= R_WAVE) {
        g->io[r] = v;
        return;
    }
    if (r == R_NR52) {
        if (!(v & 0x80u) && g->apu_on) {               /* off: every register cleared */
            uint32_t i;
            uint32_t l1 = g->ch1.len, l2 = g->ch2.len, l3 = g->ch3.len, l4 = g->ch4.len;   /* (DMG: lengths kept) */
            for (i = R_NR10; i < R_NR52; i++)
                g->io[i] = 0;
            memset(&g->ch1, 0, sizeof g->ch1);
            memset(&g->ch2, 0, sizeof g->ch2);
            memset(&g->ch3, 0, sizeof g->ch3);
            memset(&g->ch4, 0, sizeof g->ch4);
            g->ch1.len = l1;
            g->ch2.len = l2;
            g->ch3.len = l3;
            g->ch4.len = l4;
            g->ch1.period = g->ch1.timer = g->ch2.period = g->ch2.timer = 8192;
            g->ch3.period = g->ch3.timer = 4096;
            g->ch3.vol_shift = 4;
            g->ch4.lfsr = 0x7FFF;
            g->apu_on = 0;
        } else if ((v & 0x80u) && !g->apu_on) {
            g->apu_on = 1;
            g->fs_step = 0;
        }
        return;
    }
    if (!g->apu_on) {                                   /* (DMG: the lengths can be written while off) */
        if (r == R_NR11)
            g->ch1.len = 64u - (v & 63u);
        else if (r == R_NR21)
            g->ch2.len = 64u - (v & 63u);
        else if (r == R_NR31)
            g->ch3.len = 256u - v;
        else if (r == R_NR41)
            g->ch4.len = 64u - (v & 63u);
        return;
    }
    g->io[r] = v;
    switch (r) {
    case R_NR10:
        g->ch1.sw_per = (uint8_t)((v >> 4) & 7u);
        if (g->ch1.sw_dir && !(v & 8u) && g->ch1.sw_neg)
            g->ch1.on = 0;                              /* (negate used, then cleared) */
        g->ch1.sw_dir = (uint8_t)((v >> 3) & 1u);
        g->ch1.sw_shift = v & 7u;
        break;
    case R_NR11:
    case R_NR21: {
        struct gb_sq *c = r == R_NR11 ? &g->ch1 : &g->ch2;
        c->duty = v >> 6;
        c->len = 64u - (v & 63u);
        sq_level(c);
        break;
    }
    case R_NR12:
    case R_NR22: {
        struct gb_sq *c = r == R_NR12 ? &g->ch1 : &g->ch2;
        c->dac = (v & 0xF8u) != 0;
        if (!c->dac)
            c->on = 0;
        break;
    }
    case R_NR13:
    case R_NR23: {
        struct gb_sq *c = r == R_NR13 ? &g->ch1 : &g->ch2;
        c->freq = (uint16_t)((c->freq & 0x700u) | v);
        c->period = (2048 - (int32_t)c->freq) * 4;
        break;
    }
    case R_NR14:
    case R_NR24: {
        struct gb_sq *c = r == R_NR14 ? &g->ch1 : &g->ch2;
        c->freq = (uint16_t)((c->freq & 0xFFu) | (uint32_t)(v & 7u) << 8);
        c->period = (2048 - (int32_t)c->freq) * 4;
        len_enable(g, &c->on, &c->len, &c->len_on, v, 64);
        if (v & 0x80u)
            sq_trigger(g, c, g->io[r - 2u]);
        break;
    }
    case R_NR30:
        g->ch3.dac = (uint8_t)(v >> 7);
        if (!g->ch3.dac)
            g->ch3.on = 0;
        break;
    case R_NR31:
        g->ch3.len = 256u - v;
        break;
    case R_NR32: {
        static const uint8_t SH[4] = {4, 0, 1, 2};
        g->ch3.vol_shift = SH[(v >> 5) & 3u];
        g->ch3.out = (uint8_t)(g->ch3.sample >> g->ch3.vol_shift);
        break;
    }
    case R_NR33:
        g->ch3.freq = (uint16_t)((g->ch3.freq & 0x700u) | v);
        g->ch3.period = (2048 - (int32_t)g->ch3.freq) * 2;
        break;
    case R_NR34:
        g->ch3.freq = (uint16_t)((g->ch3.freq & 0xFFu) | (uint32_t)(v & 7u) << 8);
        g->ch3.period = (2048 - (int32_t)g->ch3.freq) * 2;
        len_enable(g, &g->ch3.on, &g->ch3.len, &g->ch3.len_on, v, 256);
        if (v & 0x80u) {
            g->ch3.on = g->ch3.dac;
            if (!g->ch3.len)
                g->ch3.len = 256;
            g->ch3.timer = g->ch3.period + 6;
            g->ch3.pos = 0;
        }
        break;
    case R_NR41:
        g->ch4.len = 64u - (v & 63u);
        break;
    case R_NR42:
        g->ch4.dac = (v & 0xF8u) != 0;
        if (!g->ch4.dac)
            g->ch4.on = 0;
        break;
    case R_NR43:
        noise_period(&g->ch4, v);
        break;
    case R_NR44:
        len_enable(g, &g->ch4.on, &g->ch4.len, &g->ch4.len_on, v, 64);
        if (v & 0x80u) {
            struct gb_noise *c = &g->ch4;
            uint8_t nr42 = g->io[R_NR42];
            c->on = c->dac;
            if (!c->len)
                c->len = 64;
            noise_period(c, g->io[R_NR43]);
            c->timer = c->period ? c->period : 8;
            c->lfsr = 0x7FFF;
            c->vol = nr42 >> 4;
            c->env_dir = (uint8_t)((nr42 >> 3) & 1u);
            c->env_per = nr42 & 7u;
            c->env_t = c->env_per;
            c->out = 0;
        }
        break;
    default:
        break;
    }
}

/* --------------------------------------------------------------------------------------------- the bus --- */
static uint8_t eram_read(gb_t *g, uint32_t a)
{
    if (!g->ram_on)
        return 0xFF;
    if (g->mbc == MBC_2)
        return g->eram ? (uint8_t)(g->eram[a & 0x1FFu] | 0xF0u) : 0xFF;
    if (g->mbc == MBC_3 && g->ram_bank >= 8u)
        return g->ram_bank <= 12u ? g->rtc_latched[g->ram_bank - 8u] : 0xFF;
    if (!g->eram || !g->eram_size)
        return 0xFF;
    return g->eram[(eram_off(g) + (a & 0x1FFFu)) & (g->eram_size - 1u)];
}

static void eram_write(gb_t *g, uint32_t a, uint8_t v)
{
    if (!g->ram_on)
        return;
    if (g->mbc == MBC_2) {
        if (g->eram) {
            g->eram[a & 0x1FFu] = v & 0x0Fu;
            g->eram_writes++;
        }
        return;
    }
    if (g->mbc == MBC_3 && g->ram_bank >= 8u) {
        static const uint8_t MASK[5] = {63, 63, 31, 255, 0xC1};
        if (g->ram_bank <= 12u) {
            g->rtc[g->ram_bank - 8u] = v & MASK[g->ram_bank - 8u];
            g->rtc_latched[g->ram_bank - 8u] = g->rtc[g->ram_bank - 8u];
            if (g->ram_bank == 8u)
                g->rtc_sub = 0;
            g->eram_writes++;
        }
        return;
    }
    if (!g->eram || !g->eram_size)
        return;
    g->eram[(eram_off(g) + (a & 0x1FFFu)) & (g->eram_size - 1u)] = v;
    g->eram_writes++;
}

static void ram_enable(gb_t *g, uint8_t v)
{
    uint8_t on = (uint8_t)((v & 0x0Fu) == 0x0Au);
    if (g->ram_on && !on)
        g->eram_off_count++;
    g->ram_on = on;
}

static void mbc_write(gb_t *g, uint32_t a, uint8_t v)
{
    uint32_t rb = g->rom_bank, r0 = g->rom0_bank;
    switch (g->mbc) {
    case MBC_1:
        if (a < 0x2000u) {
            ram_enable(g, v);
        } else if (a < 0x4000u) {
            g->mbc1_lo = v & 0x1Fu;
            if (!g->mbc1_lo)
                g->mbc1_lo = 1;
        } else if (a < 0x6000u) {
            g->mbc1_hi = v & 3u;
        } else {
            g->mbc_mode = v & 1u;
        }
        g->rom_bank = (uint32_t)g->mbc1_hi << 5 | g->mbc1_lo;
        g->rom0_bank = g->mbc_mode ? (uint32_t)g->mbc1_hi << 5 : 0u;
        g->ram_bank = g->mbc_mode ? g->mbc1_hi : 0u;
        break;
    case MBC_2:
        if (a < 0x4000u) {
            if (a & 0x100u) {
                g->rom_bank = v & 0x0Fu;
                if (!g->rom_bank)
                    g->rom_bank = 1;
            } else {
                ram_enable(g, v);
            }
        }
        break;
    case MBC_3:
        if (a < 0x2000u) {
            ram_enable(g, v);
        } else if (a < 0x4000u) {
            g->rom_bank = v & 0x7Fu;
            if (!g->rom_bank)
                g->rom_bank = 1;
        } else if (a < 0x6000u) {
            g->ram_bank = v;
        } else {
            if (!g->rtc_latch_arm && v == 1u)
                memcpy(g->rtc_latched, g->rtc, 5);
            g->rtc_latch_arm = v;
        }
        break;
    case MBC_5:
        if (a < 0x2000u)
            ram_enable(g, v);
        else if (a < 0x3000u)
            g->rom_bank = (g->rom_bank & 0x100u) | v;
        else if (a < 0x4000u)
            g->rom_bank = (g->rom_bank & 0xFFu) | (uint32_t)(v & 1u) << 8;
        else if (a < 0x6000u)
            g->ram_bank = v & 0x0Fu;
        break;
    default:                                            /* no mapper: RAM (0x08, 0x09) is always there */
        break;
    }
    if (g->rom_bank != rb || g->rom0_bank != r0)
        map_rom(g);
    map_ram(g);
}

static uint8_t joyp_read(const gb_t *g)
{
    uint8_t sel = g->io[R_P1] & 0x30u, r = 0x0F;
    if (!(sel & 0x10u))
        r &= (uint8_t)~(g->buttons & 0x0Fu);
    if (!(sel & 0x20u))
        r &= (uint8_t)~(g->buttons >> 4);
    return (uint8_t)(0xC0u | sel | r);
}

static uint8_t io_read(gb_t *g, uint32_t r)
{
    switch (r) {
    case R_P1:
        return joyp_read(g);
    case R_SB:
        return g->io[R_SB];
    case R_SC:
        return (uint8_t)(g->io[R_SC] | 0x7Eu);
    case R_DIV:
        return (uint8_t)((uint32_t)(g->clock - g->div_base) >> 8);
    case R_TIMA:
        return (g->io[R_TAC] & 4u) ? (uint8_t)(g->tima_val + tima_incs(g, g->tima_time, g->clock)) : g->tima_val;
    case R_TMA:
        return g->io[R_TMA];
    case R_TAC:
        return (uint8_t)(g->io[R_TAC] | 0xF8u);
    case R_IF:
        return (uint8_t)(g->iflag | 0xE0u);
    case R_STAT:
        return (uint8_t)(0x80u | (g->io[R_STAT] & 0x78u) | (g->lcd_on && g->ly == g->io[R_LYC] ? 4u : 0u) |
                         (g->lcd_on ? g->mode : 0u));
    case R_LY:
        return g->ly;
    case R_LCDC:
    case R_SCY:
    case R_SCX:
    case R_LYC:
    case R_DMA:
    case R_BGP:
    case R_OBP0:
    case R_OBP1:
    case R_WY:
    case R_WX:
        return g->io[r];
    default:
        if (r >= R_NR10 && r < 0x40u)
            return apu_read(g, r);
        return 0xFF;
    }
}

static uint8_t rd_slow(gb_t *g, uint32_t a)
{
    if (a >= 0xA000u && a < 0xC000u)
        return eram_read(g, a);
    if (a < 0xFE00u)
        return g->wram[a & 0x1FFFu];                    /* (0xF000..0xFDFF: the echo) */
    if (a < 0xFEA0u)
        return g->oam[a - 0xFE00u];
    if (a < 0xFF00u)
        return 0x00;
    if (a >= 0xFF80u)
        return a == 0xFFFFu ? g->ie : g->hram[a - 0xFF80u];
    return io_read(g, a & 0x7Fu);
}

static void io_write(gb_t *g, uint32_t r, uint8_t v)
{
    switch (r) {
    case R_P1:
        g->io[R_P1] = v & 0x30u;
        break;
    case R_SB:
        g->io[R_SB] = v;
        break;
    case R_SC:
        g->io[R_SC] = v & 0x81u;
        g->ser_next = (v & 0x81u) == 0x81u ? g->clock + 4096 : NEVER;   /* our own clock: 8 bits in, nobody there */
        if ((v & 0x81u) == 0x81u && g->serial)
            g->serial(g, g->io[R_SB]);
        limit_update(g);
        break;
    case R_DIV:
        div_write(g);
        limit_update(g);
        break;
    case R_TIMA:
        tima_sync(g);
        g->tima_val = v;
        tima_schedule(g);
        limit_update(g);
        break;
    case R_TMA:
        g->io[R_TMA] = v;
        break;
    case R_TAC:
        tima_sync(g);
        g->io[R_TAC] = v & 7u;
        g->tima_time = g->clock;
        tima_schedule(g);
        limit_update(g);
        break;
    case R_IF:
        g->iflag = v & 0x1Fu;
        break;
    case R_LCDC:
        lcdc_write(g, v);
        break;
    case R_STAT:
        if (g->lcd_on && !g->stat_line && (g->mode == 0 || g->mode == 1 || g->ly == g->io[R_LYC]))
            g->iflag |= IRQ_STAT;                       /* (DMG: a write raises it for a moment) */
        g->io[R_STAT] = v & 0x78u;
        stat_update(g);
        break;
    case R_LY:
        break;
    case R_LYC:
        g->io[R_LYC] = v;
        stat_update(g);
        break;
    case R_DMA: {
        uint32_t src = (uint32_t)v << 8, i;
        g->io[R_DMA] = v;
        if (src >= 0xE000u)
            src -= 0x2000u;
        for (i = 0; i < 0xA0u; i++) {
            uint32_t a = src + i;
            const uint8_t *p = g->rmap[a >> 12];
            g->oam[i] = p ? p[a & 0xFFFu] : rd_slow(g, a);
        }
        break;
    }
    default:
        if (r >= R_NR10 && r < 0x40u)
            apu_write(g, r, v);
        else
            g->io[r] = v;
        break;
    }
}

static void wr_slow(gb_t *g, uint32_t a, uint8_t v)
{
    if (a < 0x8000u) {
        mbc_write(g, a, v);
        return;
    }
    if (a >= 0xA000u && a < 0xC000u) {
        eram_write(g, a, v);
        return;
    }
    if (a < 0xFE00u) {
        g->wram[a & 0x1FFFu] = v;
        return;
    }
    if (a < 0xFEA0u) {
        g->oam[a - 0xFE00u] = v;
        return;
    }
    if (a < 0xFF00u)
        return;
    if (a >= 0xFF80u) {
        if (a == 0xFFFFu)
            g->ie = v;
        else
            g->hram[a - 0xFF80u] = v;
        return;
    }
    io_write(g, a & 0x7Fu, v);
}

static inline uint8_t rd8(gb_t *g, uint32_t a)
{
    const uint8_t *p = g->rmap[a >> 12];
    return p ? p[a & 0xFFFu] : rd_slow(g, a);
}

static inline void wr8(gb_t *g, uint32_t a, uint8_t v)
{
    uint8_t *p = g->wmap[a >> 12];
    if (p)
        p[a & 0xFFFu] = v;
    else
        wr_slow(g, a, v);
}

/* ---------------------------------------------------------------------------------------------- the CPU --- */
static const uint8_t CYC[256] = {
    4, 12, 8, 8, 4, 4, 8, 4, 20, 8, 8, 8, 4, 4, 8, 4,
    4, 12, 8, 8, 4, 4, 8, 4, 12, 8, 8, 8, 4, 4, 8, 4,
    8, 12, 8, 8, 4, 4, 8, 4, 8, 8, 8, 8, 4, 4, 8, 4,
    8, 12, 8, 8, 12, 12, 12, 4, 8, 8, 8, 8, 4, 4, 8, 4,
    4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
    4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
    4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
    8, 8, 8, 8, 8, 8, 4, 8, 4, 4, 4, 4, 4, 4, 8, 4,
    4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
    4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
    4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
    4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
    8, 12, 12, 16, 12, 16, 8, 16, 8, 16, 12, 0, 12, 24, 8, 16,
    8, 12, 12, 4, 12, 16, 8, 16, 8, 16, 12, 4, 12, 4, 8, 16,
    12, 12, 8, 4, 4, 16, 8, 16, 16, 4, 16, 4, 4, 4, 8, 16,
    12, 12, 8, 4, 4, 16, 8, 16, 12, 8, 16, 4, 4, 4, 8, 16,
};

#define RD(x) rd8(g, (uint32_t)(x) & 0xFFFFu)
#define WR(x, v) wr8(g, (uint32_t)(x) & 0xFFFFu, (uint8_t)(v))
#define IMM8() (RD(pc++))
#define IMM16(dst) do { uint32_t lo_ = RD(pc); dst = lo_ | (uint32_t)RD(pc + 1u) << 8; pc += 2u; } while (0)
#define PUSH(v) do { uint32_t v_ = (v); sp = (sp - 1u) & 0xFFFFu; WR(sp, v_ >> 8); sp = (sp - 1u) & 0xFFFFu; WR(sp, v_); } while (0)
#define POP(dst) do { uint32_t lo_ = RD(sp); sp = (sp + 1u) & 0xFFFFu; dst = lo_ | (uint32_t)RD(sp) << 8; sp = (sp + 1u) & 0xFFFFu; } while (0)
#define BC ((uint32_t)b << 8 | c)
#define DE ((uint32_t)d << 8 | e)
#define HL ((uint32_t)h << 8 | l)
#define SET_BC(v) do { uint32_t v_ = (v); b = (uint8_t)(v_ >> 8); c = (uint8_t)v_; } while (0)
#define SET_DE(v) do { uint32_t v_ = (v); d = (uint8_t)(v_ >> 8); e = (uint8_t)v_; } while (0)
#define SET_HL(v) do { uint32_t v_ = (v); h = (uint8_t)(v_ >> 8); l = (uint8_t)v_; } while (0)

#define ADD(v) do { uint32_t v_ = (v), r_ = a + v_; \
    f = (uint8_t)(((r_ & 0xFFu) ? 0u : FZ) | (((a & 15u) + (v_ & 15u)) > 15u ? FH : 0u) | (r_ > 0xFFu ? FC : 0u)); a = (uint8_t)r_; } while (0)
#define ADC(v) do { uint32_t v_ = (v), cy_ = (f >> 4) & 1u, r_ = a + v_ + cy_; \
    f = (uint8_t)(((r_ & 0xFFu) ? 0u : FZ) | (((a & 15u) + (v_ & 15u) + cy_) > 15u ? FH : 0u) | (r_ > 0xFFu ? FC : 0u)); a = (uint8_t)r_; } while (0)
#define SUB(v) do { uint32_t v_ = (v), r_ = a - v_; \
    f = (uint8_t)(FN | ((r_ & 0xFFu) ? 0u : FZ) | ((a & 15u) < (v_ & 15u) ? FH : 0u) | (a < v_ ? FC : 0u)); a = (uint8_t)r_; } while (0)
#define SBC(v) do { uint32_t v_ = (v), cy_ = (f >> 4) & 1u, r_ = a - v_ - cy_; \
    f = (uint8_t)(FN | ((r_ & 0xFFu) ? 0u : FZ) | ((a & 15u) < (v_ & 15u) + cy_ ? FH : 0u) | (a < v_ + cy_ ? FC : 0u)); a = (uint8_t)r_; } while (0)
#define AND(v) do { a &= (uint8_t)(v); f = (uint8_t)((a ? 0u : FZ) | FH); } while (0)
#define XOR(v) do { a ^= (uint8_t)(v); f = (uint8_t)(a ? 0u : FZ); } while (0)
#define OR(v) do { a |= (uint8_t)(v); f = (uint8_t)(a ? 0u : FZ); } while (0)
#define CP(v) do { uint32_t v_ = (v), r_ = a - v_; \
    f = (uint8_t)(FN | ((r_ & 0xFFu) ? 0u : FZ) | ((a & 15u) < (v_ & 15u) ? FH : 0u) | (a < v_ ? FC : 0u)); } while (0)
#define INC(r) do { r = (uint8_t)(r + 1u); f = (uint8_t)((f & FC) | (r ? 0u : FZ) | ((r & 15u) ? 0u : FH)); } while (0)
#define DEC(r) do { r = (uint8_t)(r - 1u); f = (uint8_t)((f & FC) | FN | (r ? 0u : FZ) | ((r & 15u) == 15u ? FH : 0u)); } while (0)
#define ADDHL(v) do { uint32_t hl_ = HL, v_ = (v), r_ = hl_ + v_; \
    f = (uint8_t)((f & FZ) | (((hl_ & 0xFFFu) + (v_ & 0xFFFu)) > 0xFFFu ? FH : 0u) | (r_ > 0xFFFFu ? FC : 0u)); SET_HL(r_); } while (0)

#define LD_ROW(base, dst) \
    case base + 0: dst = b; break; case base + 1: dst = c; break; case base + 2: dst = d; break; \
    case base + 3: dst = e; break; case base + 4: dst = h; break; case base + 5: dst = l; break; \
    case base + 6: dst = RD(HL); break; case base + 7: dst = a; break;
#define ALU_ROW(base, OP) \
    case base + 0: OP(b); break; case base + 1: OP(c); break; case base + 2: OP(d); break; \
    case base + 3: OP(e); break; case base + 4: OP(h); break; case base + 5: OP(l); break; \
    case base + 6: OP(RD(HL)); break; case base + 7: OP(a); break;

static uint32_t cb_op(uint32_t op, uint8_t *r, uint8_t *f)   /* -> 1: a BIT (no write back) */
{
    uint32_t v = *r, cy = (*f >> 4) & 1u, n = (op >> 3) & 7u;
    switch (op >> 6) {
    case 0:
        switch (n) {
        case 0: cy = v >> 7; v = ((v << 1) | cy) & 0xFFu; break;               /* RLC */
        case 1: cy = v & 1u; v = (v >> 1) | (cy << 7); break;                  /* RRC */
        case 2: { uint32_t o = v >> 7; v = ((v << 1) | cy) & 0xFFu; cy = o; break; }   /* RL */
        case 3: { uint32_t o = v & 1u; v = (v >> 1) | (cy << 7); cy = o; break; }      /* RR */
        case 4: cy = v >> 7; v = (v << 1) & 0xFFu; break;                     /* SLA */
        case 5: cy = v & 1u; v = (v >> 1) | (v & 0x80u); break;               /* SRA */
        case 6: cy = 0; v = ((v << 4) | (v >> 4)) & 0xFFu; break;             /* SWAP */
        default: cy = v & 1u; v >>= 1; break;                                 /* SRL */
        }
        *f = (uint8_t)((v ? 0u : FZ) | (cy ? FC : 0u));
        *r = (uint8_t)v;
        return 0;
    case 1:                                                                  /* BIT */
        *f = (uint8_t)((*f & FC) | FH | ((v >> n) & 1u ? 0u : FZ));
        return 1;
    case 2:                                                                  /* RES */
        *r = (uint8_t)(v & ~(1u << n));
        return 0;
    default:                                                                 /* SET */
        *r = (uint8_t)(v | (1u << n));
        return 0;
    }
}

static void cpu_run(gb_t *g)
{
    uint32_t pc = g->pc, sp = g->sp;
    uint8_t a = g->a, f = g->f, b = g->b, c = g->c, d = g->d, e = g->e, h = g->h, l = g->l;
    int32_t clk = g->clock;
    while (clk < g->limit) {
        uint32_t op, pend = g->ie & g->iflag & 0x1Fu;
        if (pend) {
            if (g->halted) {
                g->halted = 0;
                clk += 4;
            }
            if (g->ime) {                               /* the interrupt: 5 M-cycles */
                uint32_t n = 0;
                while (!(pend & (1u << n)))
                    n++;
                g->ime = 0;
                g->ei_delay = 0;
                g->iflag &= (uint8_t)~(1u << n);
                g->clock = clk;
                PUSH(pc);
                pc = 0x40u + 8u * n;
                clk += 20;
                continue;
            }
        }
        if (g->halted || g->locked) {                   /* asleep: on to the next event */
            clk = g->limit;
            break;
        }
        if (g->ei_delay) {
            g->ei_delay = 0;
            g->ime = 1;
        }
        g->clock = clk;
        op = RD(pc);
        if (g->halt_bug)
            g->halt_bug = 0;                            /* (HALT's bug: the byte after it read twice) */
        else
            pc = (pc + 1u) & 0xFFFFu;
        clk += CYC[op];
        switch (op) {
        case 0x00: break;
        case 0x01: { uint32_t v; IMM16(v); SET_BC(v); break; }
        case 0x02: WR(BC, a); break;
        case 0x03: SET_BC(BC + 1u); break;
        case 0x04: INC(b); break;
        case 0x05: DEC(b); break;
        case 0x06: b = IMM8(); break;
        case 0x07: { uint32_t cy = a >> 7; a = (uint8_t)((a << 1) | cy); f = (uint8_t)(cy ? FC : 0u); break; }
        case 0x08: { uint32_t v; IMM16(v); WR(v, sp); WR(v + 1u, sp >> 8); break; }
        case 0x09: ADDHL(BC); break;
        case 0x0A: a = RD(BC); break;
        case 0x0B: SET_BC(BC - 1u); break;
        case 0x0C: INC(c); break;
        case 0x0D: DEC(c); break;
        case 0x0E: c = IMM8(); break;
        case 0x0F: { uint32_t cy = a & 1u; a = (uint8_t)((a >> 1) | (cy << 7)); f = (uint8_t)(cy ? FC : 0u); break; }
        case 0x10:                                      /* STOP: asleep until a button */
            pc = (pc + 1u) & 0xFFFFu;
            g->stopped = 1;
            g->halted = 1;
            break;
        case 0x11: { uint32_t v; IMM16(v); SET_DE(v); break; }
        case 0x12: WR(DE, a); break;
        case 0x13: SET_DE(DE + 1u); break;
        case 0x14: INC(d); break;
        case 0x15: DEC(d); break;
        case 0x16: d = IMM8(); break;
        case 0x17: { uint32_t cy = a >> 7; a = (uint8_t)((a << 1) | ((f >> 4) & 1u)); f = (uint8_t)(cy ? FC : 0u); break; }
        case 0x18: { int8_t o = (int8_t)IMM8(); pc = (pc + (uint32_t)(int32_t)o) & 0xFFFFu; break; }
        case 0x19: ADDHL(DE); break;
        case 0x1A: a = RD(DE); break;
        case 0x1B: SET_DE(DE - 1u); break;
        case 0x1C: INC(e); break;
        case 0x1D: DEC(e); break;
        case 0x1E: e = IMM8(); break;
        case 0x1F: { uint32_t cy = a & 1u; a = (uint8_t)((a >> 1) | ((f & FC) << 3)); f = (uint8_t)(cy ? FC : 0u); break; }
        case 0x20: { int8_t o = (int8_t)IMM8(); if (!(f & FZ)) { pc = (pc + (uint32_t)(int32_t)o) & 0xFFFFu; clk += 4; } break; }
        case 0x21: { uint32_t v; IMM16(v); SET_HL(v); break; }
        case 0x22: WR(HL, a); SET_HL(HL + 1u); break;
        case 0x23: SET_HL(HL + 1u); break;
        case 0x24: INC(h); break;
        case 0x25: DEC(h); break;
        case 0x26: h = IMM8(); break;
        case 0x27: {                                    /* DAA */
            uint32_t t = a;
            if (!(f & FN)) {
                if ((f & FC) || t > 0x99u) {
                    t += 0x60u;
                    f |= FC;
                }
                if ((f & FH) || (t & 0x0Fu) > 9u)
                    t += 6u;
            } else {
                if (f & FC)
                    t -= 0x60u;
                if (f & FH)
                    t -= 6u;
            }
            a = (uint8_t)t;
            f = (uint8_t)((f & (FN | FC)) | (a ? 0u : FZ));
            break;
        }
        case 0x28: { int8_t o = (int8_t)IMM8(); if (f & FZ) { pc = (pc + (uint32_t)(int32_t)o) & 0xFFFFu; clk += 4; } break; }
        case 0x29: ADDHL(HL); break;
        case 0x2A: a = RD(HL); SET_HL(HL + 1u); break;
        case 0x2B: SET_HL(HL - 1u); break;
        case 0x2C: INC(l); break;
        case 0x2D: DEC(l); break;
        case 0x2E: l = IMM8(); break;
        case 0x2F: a = (uint8_t)~a; f |= FN | FH; break;
        case 0x30: { int8_t o = (int8_t)IMM8(); if (!(f & FC)) { pc = (pc + (uint32_t)(int32_t)o) & 0xFFFFu; clk += 4; } break; }
        case 0x31: IMM16(sp); break;
        case 0x32: WR(HL, a); SET_HL(HL - 1u); break;
        case 0x33: sp = (sp + 1u) & 0xFFFFu; break;
        case 0x34: { uint8_t v = RD(HL); INC(v); WR(HL, v); break; }
        case 0x35: { uint8_t v = RD(HL); DEC(v); WR(HL, v); break; }
        case 0x36: { uint8_t v = IMM8(); WR(HL, v); break; }
        case 0x37: f = (uint8_t)((f & FZ) | FC); break;
        case 0x38: { int8_t o = (int8_t)IMM8(); if (f & FC) { pc = (pc + (uint32_t)(int32_t)o) & 0xFFFFu; clk += 4; } break; }
        case 0x39: ADDHL(sp); break;
        case 0x3A: a = RD(HL); SET_HL(HL - 1u); break;
        case 0x3B: sp = (sp - 1u) & 0xFFFFu; break;
        case 0x3C: INC(a); break;
        case 0x3D: DEC(a); break;
        case 0x3E: a = IMM8(); break;
        case 0x3F: f = (uint8_t)((f & FZ) | ((f & FC) ^ FC)); break;
        LD_ROW(0x40, b)
        LD_ROW(0x48, c)
        LD_ROW(0x50, d)
        LD_ROW(0x58, e)
        LD_ROW(0x60, h)
        LD_ROW(0x68, l)
        case 0x70: WR(HL, b); break;
        case 0x71: WR(HL, c); break;
        case 0x72: WR(HL, d); break;
        case 0x73: WR(HL, e); break;
        case 0x74: WR(HL, h); break;
        case 0x75: WR(HL, l); break;
        case 0x76:                                      /* HALT */
            if (g->ime || !(g->ie & g->iflag & 0x1Fu))
                g->halted = 1;
            else
                g->halt_bug = 1;
            break;
        case 0x77: WR(HL, a); break;
        LD_ROW(0x78, a)
        ALU_ROW(0x80, ADD)
        ALU_ROW(0x88, ADC)
        ALU_ROW(0x90, SUB)
        ALU_ROW(0x98, SBC)
        ALU_ROW(0xA0, AND)
        ALU_ROW(0xA8, XOR)
        ALU_ROW(0xB0, OR)
        ALU_ROW(0xB8, CP)
        case 0xC0: if (!(f & FZ)) { POP(pc); clk += 12; } break;
        case 0xC1: { uint32_t v; POP(v); SET_BC(v); break; }
        case 0xC2: { uint32_t v; IMM16(v); if (!(f & FZ)) { pc = v; clk += 4; } break; }
        case 0xC3: { uint32_t v; IMM16(v); pc = v; break; }
        case 0xC4: { uint32_t v; IMM16(v); if (!(f & FZ)) { PUSH(pc); pc = v; clk += 12; } break; }
        case 0xC5: PUSH(BC); break;
        case 0xC6: ADD(IMM8()); break;
        case 0xC7: PUSH(pc); pc = 0x00; break;
        case 0xC8: if (f & FZ) { POP(pc); clk += 12; } break;
        case 0xC9: POP(pc); break;
        case 0xCA: { uint32_t v; IMM16(v); if (f & FZ) { pc = v; clk += 4; } break; }
        case 0xCB: {
            uint32_t op2 = IMM8(), r = op2 & 7u;
            uint8_t v, *p;
            switch (r) {
            case 0: p = &b; break;
            case 1: p = &c; break;
            case 2: p = &d; break;
            case 3: p = &e; break;
            case 4: p = &h; break;
            case 5: p = &l; break;
            case 7: p = &a; break;
            default: p = 0; break;
            }
            if (p) {
                cb_op(op2, p, &f);
                clk += 8;
            } else {
                v = RD(HL);
                if (cb_op(op2, &v, &f)) {
                    clk += 12;
                } else {
                    WR(HL, v);
                    clk += 16;
                }
            }
            break;
        }
        case 0xCC: { uint32_t v; IMM16(v); if (f & FZ) { PUSH(pc); pc = v; clk += 12; } break; }
        case 0xCD: { uint32_t v; IMM16(v); PUSH(pc); pc = v; break; }
        case 0xCE: ADC(IMM8()); break;
        case 0xCF: PUSH(pc); pc = 0x08; break;
        case 0xD0: if (!(f & FC)) { POP(pc); clk += 12; } break;
        case 0xD1: { uint32_t v; POP(v); SET_DE(v); break; }
        case 0xD2: { uint32_t v; IMM16(v); if (!(f & FC)) { pc = v; clk += 4; } break; }
        case 0xD4: { uint32_t v; IMM16(v); if (!(f & FC)) { PUSH(pc); pc = v; clk += 12; } break; }
        case 0xD5: PUSH(DE); break;
        case 0xD6: SUB(IMM8()); break;
        case 0xD7: PUSH(pc); pc = 0x10; break;
        case 0xD8: if (f & FC) { POP(pc); clk += 12; } break;
        case 0xD9: POP(pc); g->ime = 1; break;
        case 0xDA: { uint32_t v; IMM16(v); if (f & FC) { pc = v; clk += 4; } break; }
        case 0xDC: { uint32_t v; IMM16(v); if (f & FC) { PUSH(pc); pc = v; clk += 12; } break; }
        case 0xDE: SBC(IMM8()); break;
        case 0xDF: PUSH(pc); pc = 0x18; break;
        case 0xE0: { uint32_t o = IMM8(); WR(0xFF00u | o, a); break; }
        case 0xE1: { uint32_t v; POP(v); SET_HL(v); break; }
        case 0xE2: WR(0xFF00u | c, a); break;
        case 0xE5: PUSH(HL); break;
        case 0xE6: AND(IMM8()); break;
        case 0xE7: PUSH(pc); pc = 0x20; break;
        case 0xE8: {
            uint32_t o = IMM8(), r = (sp + (uint32_t)(int32_t)(int8_t)o) & 0xFFFFu;
            f = (uint8_t)((((sp & 15u) + (o & 15u)) > 15u ? FH : 0u) | (((sp & 0xFFu) + o) > 0xFFu ? FC : 0u));
            sp = r;
            break;
        }
        case 0xE9: pc = HL; break;
        case 0xEA: { uint32_t v; IMM16(v); WR(v, a); break; }
        case 0xEE: XOR(IMM8()); break;
        case 0xEF: PUSH(pc); pc = 0x28; break;
        case 0xF0: { uint32_t o = IMM8(); a = RD(0xFF00u | o); break; }
        case 0xF1: { uint32_t v; POP(v); a = (uint8_t)(v >> 8); f = (uint8_t)(v & 0xF0u); break; }
        case 0xF2: a = RD(0xFF00u | c); break;
        case 0xF3: g->ime = 0; g->ei_delay = 0; break;
        case 0xF5: PUSH((uint32_t)a << 8 | f); break;
        case 0xF6: OR(IMM8()); break;
        case 0xF7: PUSH(pc); pc = 0x30; break;
        case 0xF8: {
            uint32_t o = IMM8(), r = (sp + (uint32_t)(int32_t)(int8_t)o) & 0xFFFFu;
            f = (uint8_t)((((sp & 15u) + (o & 15u)) > 15u ? FH : 0u) | (((sp & 0xFFu) + o) > 0xFFu ? FC : 0u));
            SET_HL(r);
            break;
        }
        case 0xF9: sp = HL; break;
        case 0xFA: { uint32_t v; IMM16(v); a = RD(v); break; }
        case 0xFB: g->ei_delay = 1; break;
        case 0xFE: CP(IMM8()); break;
        case 0xFF: PUSH(pc); pc = 0x38; break;
        default:                                        /* an opcode the CPU does not have: it locks up */
            g->locked = 1;
            break;
        }
        pc &= 0xFFFFu;
    }
    g->pc = (uint16_t)pc;
    g->sp = (uint16_t)sp;
    g->a = a;
    g->f = f;
    g->b = b;
    g->c = c;
    g->d = d;
    g->e = e;
    g->h = h;
    g->l = l;
    g->clock = clk;
}

static void run_events(gb_t *g)
{
    for (;;) {
        int32_t t = g->ppu_next;
        int which = 0;
        if (g->tim_next < t) {
            t = g->tim_next;
            which = 1;
        }
        if (g->ser_next < t) {
            t = g->ser_next;
            which = 2;
        }
        if (g->fs_next < t) {
            t = g->fs_next;
            which = 3;
        }
        if (t > g->clock)
            break;
        switch (which) {
        case 0:
            ppu_event(g);
            break;
        case 1:
            tima_overflow(g);
            break;
        case 2:
            g->ser_next = NEVER;
            g->io[R_SB] = 0xFF;
            g->io[R_SC] &= 0x7Fu;
            g->iflag |= IRQ_SERIAL;
            break;
        default:
            g->fs_next += 8192;
            apu_fs(g);
            break;
        }
    }
    limit_update(g);
}

void gb_run(gb_t *g, int32_t cycles)
{
    g->run_end = g->clock + cycles;
    limit_update(g);
    while (g->clock < g->run_end) {
        cpu_run(g);
        run_events(g);
    }
    apu_sync(g);
}

/* ------------------------------------------------------------------------------------------- the outside --- */
void gb_set_buttons(gb_t *g, uint32_t held)
{
    uint32_t pressed = held & ~(uint32_t)g->buttons;
    g->buttons = (uint8_t)held;
    if (pressed) {
        g->iflag |= IRQ_JOYPAD;
        if (g->stopped) {
            g->stopped = 0;
            g->halted = 0;
        }
    }
}

void gb_audio(gb_t *g, int16_t *ring, uint32_t frames_pow2, uint32_t rate)
{
    g->abuf = ring;
    g->amask = frames_pow2 - 1u;
    g->smp_step = (uint32_t)(((GB_CLOCK / rate) << 16) + (((GB_CLOCK % rate) << 16) / rate));
    g->smp_frac = 0;
    g->acc_n = 0;
    g->acc_want = sample_len(g);
}

int gb_ram_size(const uint8_t *h0)
{
    static const uint32_t SIZE[6] = {0, 2048, 8192, 32768, 131072, 65536};
    uint8_t t = h0[0x147], r = h0[0x149];
    uint32_t n;
    if (t == 0x05 || t == 0x06)
        return 512;
    n = r < 6u ? SIZE[r] : 0u;
    if (n > 32768u)
        n = 32768u;                                 /* (the largest DMG games keep) */
    if (!n && (t == 0x08 || t == 0x09))
        n = 8192u;
    return (int)n;
}

const char *gb_mapper_name(const gb_t *g)
{
    static const char *const N[] = {"ROM", "MBC1", "MBC2", "MBC3", "MBC5"};
    return g->mbc < 5u ? N[g->mbc] : "?";
}

int gb_load(gb_t *g, gb_bank_fn bank_fn, void *bank_user, uint32_t rom_size, uint8_t *eram)
{
    const uint8_t *h;
    uint8_t t;
    uint32_t i;
    if (rom_size < 0x8000u)
        return GB_E_SIZE;
    g->bank_fn = bank_fn;
    g->bank_user = bank_user;
    g->rom_banks = rom_size / 0x4000u;
    h = bank_fn(bank_user, 0, 0);
    t = h[0x147];
    switch (t) {
    case 0x00: case 0x08: case 0x09: g->mbc = MBC_NONE; break;
    case 0x01: case 0x02: case 0x03: case 0xFF: g->mbc = MBC_1; break;   /* (HuC1 as an MBC1) */
    case 0x05: case 0x06: g->mbc = MBC_2; break;
    case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: g->mbc = MBC_3; break;
    case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: g->mbc = MBC_5; break;
    default: return GB_E_MAPPER;
    }
    g->has_battery = (uint8_t)(t == 0x03 || t == 0x06 || t == 0x09 || t == 0x0F || t == 0x10 || t == 0x13 ||
                               t == 0x1B || t == 0x1E || t == 0xFF);
    g->has_rtc = (uint8_t)(t == 0x0F || t == 0x10);
    g->eram_size = (uint32_t)gb_ram_size(h);
    g->eram = g->eram_size ? eram : 0;
    for (i = 0; i < 16u; i++) {
        char ch = (char)h[0x134 + i];
        g->title[i] = (ch >= 0x20 && ch < 0x7F) ? ch : 0;
        if (!ch || (i >= 11u && (h[0x143] & 0x80u)))
            break;
    }
    g->title[i < 16u ? i : 16u] = 0;
    g->header_sum = h[0x14D];
    g->global_sum = (uint16_t)(h[0x14E] << 8 | h[0x14F]);
    return h[0x143] == 0xC0u ? GB_E_CGB_ONLY : GB_OK;
}

void gb_reset(gb_t *g)
{
    static const uint8_t IO_BOOT[0x30] = {
        0xCF, 0x00, 0x7E, 0xFF, 0x00, 0x00, 0x00, 0xF8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xE1,
        0x80, 0xBF, 0xF3, 0xFF, 0xBF, 0xFF, 0x3F, 0x00, 0xFF, 0xBF, 0x7F, 0xFF, 0x9F, 0xFF, 0xBF, 0xFF,
        0xFF, 0x00, 0x00, 0xBF, 0x77, 0xF3, 0xF1, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    tables_init();
    g->a = 0x01;
    g->f = 0xB0;
    g->b = 0x00;
    g->c = 0x13;
    g->d = 0x00;
    g->e = 0xD8;
    g->h = 0x01;
    g->l = 0x4D;
    g->sp = 0xFFFE;
    g->pc = 0x0100;
    g->ime = g->ei_delay = g->halted = g->halt_bug = g->locked = g->stopped = 0;
    g->ie = 0;
    g->iflag = 0xE1u & 0x1Fu;
    memset(g->vram, 0, sizeof g->vram);
    memset(g->wram, 0, sizeof g->wram);
    memset(g->oam, 0, sizeof g->oam);
    memset(g->hram, 0, sizeof g->hram);
    memset(g->io, 0xFF, sizeof g->io);
    memcpy(g->io, IO_BOOT, sizeof IO_BOOT);
    memset(g->io + R_WAVE, 0, 16);
    g->io[R_LCDC] = 0x91;
    g->io[R_STAT] = 0x00;
    g->io[R_SCY] = g->io[R_SCX] = 0;
    g->io[R_LYC] = 0;
    g->io[R_DMA] = 0xFF;
    g->io[R_BGP] = 0xFC;
    g->io[R_OBP0] = g->io[R_OBP1] = 0xFF;
    g->io[R_WY] = g->io[R_WX] = 0;
    g->io[R_TAC] = 0;
    /* the cartridge */
    g->rom_bank = 1;
    g->rom0_bank = 0;
    g->ram_on = g->ram_bank = g->mbc_mode = 0;
    g->mbc1_lo = 1;
    g->mbc1_hi = 0;
    g->rtc_latch_arm = 0xFF;
    g->rtc_sub = 0;
    /* time */
    g->clock = 0;
    g->run_end = 0;
    g->div_base = -0xABCC;                              /* (DIV 0xAB as the boot ROM leaves it) */
    g->tima_val = 0;
    g->tima_time = 0;
    g->tim_next = NEVER;
    g->ser_next = NEVER;
    g->fs_next = 0xC000 - 0xABCC;
    /* the PPU: line 0 starting */
    g->lcd_on = 1;
    g->ly = 0;
    g->mode = 2;
    g->stat_line = 0;
    g->win_line = 0;
    g->win_y_hit = 0;
    g->line_start = 0;
    g->ppu_next = 80;
    g->frames = 0;
    if (g->fb)
        memset(g->fb, GB_PAL_BG, GB_W * GB_H);
    /* the APU: on, quiet (the boot ROM's chime long over) */
    memset(&g->ch1, 0, sizeof g->ch1);
    memset(&g->ch2, 0, sizeof g->ch2);
    memset(&g->ch3, 0, sizeof g->ch3);
    memset(&g->ch4, 0, sizeof g->ch4);
    g->ch1.dac = 1;
    g->ch1.duty = 2;
    g->ch1.period = g->ch2.period = 8192;
    g->ch1.timer = g->ch2.timer = 8192;
    g->ch3.period = g->ch3.timer = 4096;
    g->ch3.vol_shift = 4;
    g->ch4.period = g->ch4.timer = 8;
    g->ch4.lfsr = 0x7FFF;
    g->apu_on = 1;
    g->fs_step = 0;
    g->apu_clock = 0;
    memset(g->acc, 0, sizeof g->acc);
    g->acc_n = 0;
    g->hp_l = g->hp_r = g->hp_xl = g->hp_xr = 0;
    g->buttons = 0;
    map_all(g);
    limit_update(g);
}

/* ------------------------------------------------------------------------------------------ save states --- */
#define STATE_MAGIC 0x31534247u                     /* "GBS1" */
typedef struct {
    uint32_t magic, size, eram_size;
    uint16_t global_sum;
    uint8_t header_sum, rsv;
} state_head_t;

uint32_t gb_state_size(const gb_t *g)
{
    return (uint32_t)(sizeof(state_head_t) + sizeof(gb_t) + g->eram_size);
}

void gb_state_save(const gb_t *g, uint8_t *dst)
{
    state_head_t h;
    h.magic = STATE_MAGIC;
    h.size = (uint32_t)sizeof(gb_t);
    h.eram_size = g->eram_size;
    h.global_sum = g->global_sum;
    h.header_sum = g->header_sum;
    h.rsv = 0;
    memcpy(dst, &h, sizeof h);
    memcpy(dst + sizeof h, g, sizeof(gb_t));
    if (g->eram_size && g->eram)
        memcpy(dst + sizeof h + sizeof(gb_t), g->eram, g->eram_size);
}

int gb_state_load(gb_t *g, const uint8_t *src, uint32_t n)
{
    state_head_t h;
    /* what is the host's: pointers, callbacks, the ring's place (kept field by field: a gb_t is too big for a stack) */
    gb_bank_fn bank_fn = g->bank_fn;
    void *bank_user = g->bank_user, *user = g->user;
    uint8_t *eram = g->eram, *fb = g->fb, skip = g->skip_render;
    void (*vblank)(gb_t *) = g->vblank;
    void (*serial)(gb_t *, uint8_t) = g->serial;
    int16_t *abuf = g->abuf;
    uint32_t amask = g->amask, awr = g->awr, banks = g->rom_banks, smp_step = g->smp_step;
    if (n < sizeof h)
        return -1;
    memcpy(&h, src, sizeof h);
    if (h.magic != STATE_MAGIC || h.size != sizeof(gb_t) || h.eram_size != g->eram_size ||
        h.global_sum != g->global_sum || h.header_sum != g->header_sum || n < gb_state_size(g))
        return -1;
    memcpy(g, src + sizeof h, sizeof(gb_t));
    g->bank_fn = bank_fn;
    g->bank_user = bank_user;
    g->eram = eram;
    g->fb = fb;
    g->vblank = vblank;
    g->user = user;
    g->serial = serial;
    g->abuf = abuf;
    g->amask = amask;
    g->awr = awr;
    g->skip_render = skip;
    g->rom_banks = banks;
    g->smp_step = smp_step;
    if (g->eram_size && g->eram)
        memcpy(g->eram, src + sizeof h + sizeof(gb_t), g->eram_size);
    g->apu_clock = g->clock;
    map_all(g);
    limit_update(g);
    return 0;
}
