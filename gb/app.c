/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 The fm1-gb authors */
/* FM-GB: a Game Boy (DMG) on the FM-1. Part of the unity build (device/unity.c, host/plat_host.c), after the
 * platform: gfx.c, lcd.c and announce.c are in scope. The emulator itself is core/gb.c; the hot loops fast.c.
 *
 * The game runs in the main loop, as far ahead of the sound as the ring below holds: the audio interrupt plays the
 * ring, and the emulator runs until the ring is full enough again, so the game's speed is the sound's clock. Each
 * frame done at VBlank is sent to the LCD a strip at a time (DMA, while the game runs on), only the strips that
 * changed; a frame that comes while the last one is still going out is dropped (the LCD's SPI takes ~19 ms for a
 * whole 1.5x frame).
 *
 * The keys are the pad (lit dim on the keybed):
 *   G#3 A#3 (F#3)           UP          F4  SELECT  (or SEL)
 *   G3 (F3) LEFT, A3 DOWN, B3 (C4) RIGHT        G4  START   (or PLAY)
 *   D5 D#5 E5               B           F5 F#5 G5  A
 * HOME the menu, FX held fast forward, SAVE a quick save state, PRESETS the palette, ALGORITHM the screen. */
#include "core/gb.h"
#include "fast.h"

#define K(k) (1u << (k))
#define BTN(b) (1u << (b))

static gb_t gb;
#include "rom.c"
#include "save.c"

/* ---------------------------------------------------------------------------------------------- settings --- */
#define SET_MAGIC 0x31474D46u                       /* "FMG1" */
typedef struct {
    uint32_t magic;
    uint8_t palette;                                /* PALS[] */
    uint8_t scale;                                  /* SCALE_* */
    uint8_t meter;                                  /* the speed meter in the status bar */
    uint8_t rsv[5];
} settings_t;
static settings_t set;
enum { SCALE_SMOOTH, SCALE_SHARP, SCALE_1X, NSCALE };
static const char *const SCALE_NAME[NSCALE] = {"SMOOTH", "SHARP", "1:1"};

typedef struct {
    const char *name;
    uint32_t c[12];                                 /* 0xRRGGBB: the background's 4 shades, OBJ0's, OBJ1's */
} gbpal_t;
#define SAME4(a, b, c, d) a, b, c, d, a, b, c, d, a, b, c, d
static const gbpal_t PALS[] = {
    {"DMG", {SAME4(0x9BBC0Fu, 0x8BAC0Fu, 0x306230u, 0x0F380Fu)}},
    {"POCKET", {SAME4(0xC4CFA1u, 0x8B956Du, 0x4D533Cu, 0x1F1F1Fu)}},
    {"GRAY", {SAME4(0xFFFFFFu, 0xAAAAAAu, 0x555555u, 0x000000u)}},
    {"LIGHT", {SAME4(0x00B581u, 0x009A71u, 0x00694Au, 0x004F3Bu)}},
    {"AMBER", {SAME4(0xFFD58Cu, 0xD9903Au, 0x8A4A12u, 0x2E1606u)}},
    {"COLOR", {0xFFFFFFu, 0xFFAD63u, 0x843100u, 0x000000u, 0xFFFFFFu, 0xFF8484u, 0x943A3Au, 0x000000u,
               0xFFFFFFu, 0x63A5FFu, 0x0000FFu, 0x000000u}},
    {"NIGHT", {SAME4(0x203040u, 0x406080u, 0x80B0D0u, 0xE0F0FFu)}},
};
#define NPALS (sizeof PALS / sizeof PALS[0])

static void settings_defaults(void)
{
    memset(&set, 0, sizeof set);
    set.magic = SET_MAGIC;
}
static void settings_load(void)
{
    if (plat_store_load(OBJ_SETTINGS, &set, sizeof set) != (int)sizeof set || set.magic != SET_MAGIC ||
        set.palette >= NPALS || set.scale >= NSCALE || set.meter > 1u)
        settings_defaults();
}
static uint8_t set_dirty;
static uint32_t set_changed_ms;                     /* (kept a few seconds after the last change: one flash write) */

/* ------------------------------------------------------------------------------------------------- sound --- */
#define AUD_RING 4096u                              /* frames (a power of two) */
#define AUD_TARGET 1536u                            /* what the emulator keeps ahead: 35 ms */
static int16_t aud_ring[2u * AUD_RING];
static volatile uint32_t aud_rd;                    /* frames played (gb.awr: written) */
static volatile uint8_t aud_quiet = 1;              /* 1: the render plays silence and drops what is there */
static volatile uint32_t aud_under;                 /* frames the ring ran short */

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    uint32_t i, rd = aud_rd, wr = gb.awr, have = wr - rd;
    int32_t g = (int32_t)master_q12;
    if (aud_quiet || have > AUD_RING) {             /* (paused; or the writer lapped the reader: start again) */
        rd = wr;
        have = 0;
    }
    for (i = 0; i < n; i++) {
        int32_t l = 0, r = 0;
        if (have) {
            uint32_t k = rd & (AUD_RING - 1u);
            l = aud_ring[2u * k];
            r = aud_ring[2u * k + 1u];
            rd++;
            have--;
        } else if (!aud_quiet) {
            aud_under++;
        }
        lr[2u * i] = (l * g) >> 4;                  /* 16-bit at gain 4096: the render's 24 bits */
        lr[2u * i + 1u] = (r * g) >> 4;
    }
    aud_rd = rd;
}

void app_panic(void)                                /* silence, and the save written (an update rewrites the flash) */
{
    aud_quiet = 1;
    if (rom_state == ROM_OK)
        sav_flush();
}

/* ----------------------------------------------------------------------------------------------- display --- */
/* the frame buffers (codes, gb.h), the LCD strips (DMA sources, so in RAM, two: one fills while the other goes out) */
static uint8_t fbuf[2][GB_W * GB_H] PLAT_POOL;
#define STRIP_ROWS 8u                               /* Game Boy rows a strip */
#define NSTRIPS (GB_H / STRIP_ROWS)
static uint16_t sbuf[2][240u * STRIP_ROWS * 3u / 2u] PLAT_POOL;
#define VIEW_H 216u                                 /* the game's rows on the screen at 1.5x; the status bar below */
static fast_pal_t vpal;
static uint8_t emu_behind;                          /* the last run ended on its time, the sound not far enough ahead */
static struct {
    uint8_t busy;                                   /* a frame is going out */
    uint8_t strip, k;                               /* the next strip; the strip buffer to fill */
    uint8_t *front;                                 /* the frame going out */
    uint32_t dirty;                                 /* its strips that changed (bit a strip) */
    uint32_t hash[NSTRIPS];                         /* each strip as last sent */
    uint32_t t_sent, us_sent;                       /* the last strip's DMA: when it started, how long it takes */
    uint32_t shown, dropped, strips;                /* counts */
    uint8_t blank;                                  /* the game's area to be cleared before the next frame */
} disp;

static void pal_apply(void)
{
    uint16_t c[16];
    uint32_t i;
    for (i = 0; i < 16u; i++) {
        uint32_t x = i < 12u ? PALS[set.palette].c[i] : 0u;
        c[i] = RGB((x >> 16) & 0xFFu, (x >> 8) & 0xFFu, x & 0xFFu);
    }
    fast_pal_make(&vpal, c);
}

static void disp_invalidate(void)                   /* every strip sent again (a palette, a scale, the menu gone) */
{
    uint32_t i;
    for (i = 0; i < NSTRIPS; i++)
        disp.hash[i] = 0x5EED5EEDu ^ i;
}

static void on_vblank(gb_t *g)                      /* the core: a frame is done */
{
    uint32_t i, h;
    if (g->skip_render || disp.busy) {
        disp.dropped++;
        g->skip_render = 0;
        return;
    }
    disp.front = g->fb;
    g->fb = g->fb == fbuf[0] ? fbuf[1] : fbuf[0];
    disp.dirty = 0;
    for (i = 0; i < NSTRIPS; i++) {
        h = fast_hash(disp.front + i * STRIP_ROWS * GB_W, STRIP_ROWS * GB_W) ^ set.palette ^ (uint32_t)set.scale << 8;
        if (h != disp.hash[i]) {
            disp.hash[i] = h;
            disp.dirty |= 1u << i;
        }
    }
    disp.strip = 0;
    disp.busy = disp.dirty != 0;
    disp.shown++;
    /* the strips this frame sends against a frame's time: when they take longer, the next frame could not be shown
     * anyway (still busy at its VBlank), so it is not drawn either: the CPU goes to the game */
    {
        uint32_t n = 0, d = disp.dirty, us;
        while (d) {
            n += d & 1u;
            d >>= 1;
        }
        us = n * (set.scale == SCALE_1X ? 160u * STRIP_ROWS * 2u : 240u * STRIP_ROWS * 3u) / 5u;
        g->skip_render = us > 15000u || emu_behind;   /* (or the game runs behind the sound: drawing it is the
                                                       * first thing to go) */
    }
}

static void pump(void)                              /* the next strip out, once the last one's DMA is done */
{
    while (disp.busy) {
        uint32_t s = disp.strip, bytes, x, y, w, rows;
        if (s >= NSTRIPS) {
            disp.busy = 0;
            return;
        }
        if (!(disp.dirty >> s & 1u)) {
            disp.strip++;
            continue;
        }
#ifndef PLAT_HOST                                  /* (the host's LCD takes a strip at once) */
        if (plat_ticks24() - disp.t_sent < disp.us_sent * PLAT_TICKS_PER_US)
            return;                                 /* (still going out: come back) */
#endif
        if (disp.blank) {
            lcd_fill(0, 0, 240, VIEW_H, C_BLACK);
            disp.blank = 0;
        }
        if (set.scale == SCALE_1X) {
            fast_rows_1(sbuf[disp.k], disp.front + s * STRIP_ROWS * GB_W, STRIP_ROWS, &vpal);
            x = 40;
            y = 36u + s * STRIP_ROWS;
            w = GB_W;
            rows = STRIP_ROWS;
        } else {
            fast_rows_15(sbuf[disp.k], disp.front + s * STRIP_ROWS * GB_W, STRIP_ROWS, &vpal,
                         set.scale == SCALE_SMOOTH);
            x = 0;
            y = s * STRIP_ROWS * 3u / 2u;
            w = 240;
            rows = STRIP_ROWS * 3u / 2u;
        }
        lcd_blit(x, y, w, rows, sbuf[disp.k]);
        bytes = w * rows * 2u;
        disp.t_sent = plat_ticks24();
        disp.us_sent = bytes / 5u;                  /* (the SPI moves ~5.5 bytes a microsecond) */
        disp.k ^= 1u;
        disp.strip++;
        disp.strips++;
        return;
    }
}

/* ------------------------------------------------------------------------------------------- status bar --- */
static struct {
    char msg[2][24];
    uint32_t until;                                 /* the toast shows until then */
    uint8_t redraw;
    uint32_t fps_t, fps_frames, fps_shown, fps;     /* the speed meter */
    uint32_t emu_us, emu_us_shown;
} bar;

static void text_cpy(char *d, const char *s, uint32_t n)
{
    uint32_t i;
    for (i = 0; i + 1u < n && s && s[i]; i++)
        d[i] = s[i];
    d[i] = 0;
}
static void text_cat(char *d, const char *s)
{
    while (*d)
        d++;
    while (*s)
        *d++ = *s++;
    *d = 0;
}
static void int_text(int32_t v, char *b)
{
    char t[12];
    int n = 0;
    uint32_t u = (uint32_t)(v < 0 ? -v : v);
    do
        t[n++] = (char)('0' + u % 10u);
    while ((u /= 10u) && n < 11);
    if (v < 0)
        *b++ = '-';
    while (n)
        *b++ = t[--n];
    *b = 0;
}

void app_say(const char *a, const char *b)
{
    char t[52];
    text_cpy(bar.msg[0], a, sizeof bar.msg[0]);
    text_cpy(bar.msg[1], b, sizeof bar.msg[1]);
    bar.until = plat_ms() + 1500u;
    bar.redraw = 1;
    if (ann_on()) {
        t[0] = 0;
        text_cat(t, bar.msg[0]);
        if (bar.msg[1][0]) {
            text_cat(t, " ");
            text_cat(t, bar.msg[1]);
        }
        ann_say(t, 0, ANN_NOW);
    }
}

static void bar_draw(void)                          /* the 24 rows under the game (at 1x: its own black margin) */
{
    uint32_t now = plat_ms();
    cv_begin(240, 24, C_BLACK);
    if (now < bar.until) {
        int32_t x = cv_text(8, 4, &FONT_B, bar.msg[0], C_WHITE);
        cv_text(x + 8, 4, &FONT_B, bar.msg[1], C_HI);
    } else if (set.meter) {
        char t[40], n[12];
        t[0] = 0;
        int_text((int32_t)bar.fps_shown, n);
        text_cat(t, n);
        text_cat(t, " FPS  ");
        int_text((int32_t)bar.emu_us_shown / 100, n);
        text_cat(t, n);
        text_cat(t, "% CPU");
        cv_text(8, 5, &FONT_XS, t, C_GRAY);
        if (sw.on)
            cv_text(180, 5, &FONT_XS, "SAVING", C_HI);
    } else if (sw.on) {
        cv_text(180, 5, &FONT_XS, "SAVING", C_DIM);
    }
    cv_blit(0, VIEW_H);
}

/* --------------------------------------------------------------------------------------------- the game --- */
static uint8_t running;                             /* the game plays (no menu, a ROM that loaded) */
static int load_rc;                                 /* gb_load's */
static uint8_t eram_buf[32768] PLAT_POOL;
static uint8_t ffwd;                                /* FX held */

/* save states: in RAM (and in the flash, after the battery save's copies, when they fit) */
#define STATE_MAGIC 0x54534D46u                        /* "FMST" */
static uint8_t state_buf[sizeof(gb_t) + 64u + 32768u] PLAT_POOL;
static uint8_t state_have;                             /* state_buf holds one */
static uint32_t state_base, state_n;                      /* its flash place (0: none fits) */
typedef struct { uint32_t magic, len, crc, game, rsv[3], hcrc; } state_head_t;

static void state_init(void)
{
    uint32_t lo = sav_len ? SAV_LO + 2u * sav_n : SAV_LO, len = gb_state_size(&gb);
    const state_head_t *h;
    state_n = (32u + len + 0xFFFu) & ~0xFFFu;
    state_base = lo + state_n <= SAV_HI && len <= sizeof state_buf ? lo : 0u;
    state_have = 0;
    if (!state_base)
        return;
    h = (const state_head_t *)(const void *)plat_data(state_base);
    if (h && h->magic == STATE_MAGIC && h->hcrc == crc32_add(0, (const uint8_t *)h, 28u) && h->len == len &&
        h->game == sav_game && crc32_add(0, plat_data(state_base + 32u), len) == h->crc) {
        memcpy(state_buf, plat_data(state_base + 32u), len);
        state_have = 1;
    }
}

static int state_save(void)                            /* -> 0 kept in the flash, 1 in RAM only, -1 failed */
{
    uint32_t len = gb_state_size(&gb), off;
    state_head_t h;
    if (len > sizeof state_buf)
        return -1;
    gb_state_save(&gb, state_buf);
    state_have = 1;
    if (!state_base)
        return 1;
    aud_quiet = 1;                                  /* (the game stops for the erases) */
    for (off = 0; off < state_n; off += 0x1000u)
        if (plat_data_erase(state_base + off))
            return 1;
    if (plat_data_write(state_base + 32u, state_buf, len))
        return 1;
    memset(&h, 0, sizeof h);
    h.magic = STATE_MAGIC;
    h.len = len;
    h.crc = crc32_add(0, state_buf, len);
    h.game = sav_game;
    h.hcrc = crc32_add(0, (const uint8_t *)&h, 28u);
    return plat_data_write(state_base, &h, sizeof h) ? 1 : 0;
}

static int state_load(void)
{
    if (!state_have || gb_state_load(&gb, state_buf, gb_state_size(&gb)))
        return -1;
    gb.fb = fbuf[0];
    disp.busy = 0;
    disp_invalidate();
    aud_quiet = 1;
    return 0;
}

static void game_start(void)
{
    if (rom_state != ROM_OK)
        return;
    gb.fb = fbuf[0];
    gb.vblank = on_vblank;
    load_rc = gb_load(&gb, rom_bank, 0, rom_size, eram_buf);
    if (load_rc != GB_OK && load_rc != GB_E_CGB_ONLY)
        return;
    gb_audio(&gb, aud_ring, AUD_RING, 44100u);
    gb_reset(&gb);
    memset(eram_buf, 0xFF, sizeof eram_buf);
    sav_init();
    state_init();
    running = load_rc == GB_OK;
}

static void emulate(uint32_t budget_us)             /* the game run until the sound is far enough ahead */
{
    uint32_t t0 = plat_ticks24(), frames0 = gb.frames;
    if (!running)
        return;
    if (gb.awr - aud_rd >= AUD_TARGET)
        emu_behind = 0;
    if (aud_quiet) {                                /* (back from a pause: the ring starts over) */
        aud_rd = gb.awr;
        aud_quiet = 0;
    }
    for (;;) {
        if (ffwd) {
            if (gb.frames - frames0 >= 4u)          /* fast forward: up to 4 frames a call, the sound dropped */
                break;
            aud_rd = gb.awr;
        } else if (gb.awr - aud_rd >= AUD_TARGET + (sw.on ? 1024u : 0u)) {
            break;                                  /* (a save's erase coming: a little more ahead) */
        }
        if (plat_ticks24() - t0 >= budget_us * PLAT_TICKS_PER_US) {
            emu_behind = (uint8_t)(!ffwd && gb.awr - aud_rd < AUD_TARGET / 2u);
            break;
        }
        gb_run(&gb, GB_LINE * 2);
        pump();
    }
    bar.emu_us += (plat_ticks24() - t0) / PLAT_TICKS_PER_US;
}

/* -------------------------------------------------------------------------------------------------- input --- */
static uint32_t pad_of(uint32_t keys, uint32_t btn)
{
    uint32_t p = 0;
    if (keys & (K(0) | K(2)))
        p |= GB_LEFT;
    if (keys & K(4))
        p |= GB_DOWN;
    if (keys & (K(6) | K(7)))
        p |= GB_RIGHT;
    if (keys & (K(1) | K(3) | K(5)))
        p |= GB_UP;
    if (keys & (K(21) | K(22) | K(23)))
        p |= GB_B;
    if (keys & (K(24) | K(25) | K(26)))
        p |= GB_A;
    if ((keys & K(12)) || (btn & BTN(B_SEL)))
        p |= GB_SELECT;
    if ((keys & K(14)) || (btn & BTN(B_PLAY)))
        p |= GB_START;
    if ((p & (GB_LEFT | GB_RIGHT)) == (GB_LEFT | GB_RIGHT))
        p &= ~(uint32_t)(GB_LEFT | GB_RIGHT);       /* (a real pad can't: some games break) */
    if ((p & (GB_UP | GB_DOWN)) == (GB_UP | GB_DOWN))
        p &= ~(uint32_t)(GB_UP | GB_DOWN);
    return p;
}
#define PAD_KEYS (K(0) | K(1) | K(2) | K(3) | K(4) | K(5) | K(6) | K(7) | K(12) | K(14) | K(21) | K(22) | K(23) | \
                  K(24) | K(25) | K(26))

static struct {
    uint32_t keys, btn, pad;
    uint32_t held_over;                             /* pad bits held as the menu closed: not the game's until let go */
    uint8_t menu;                                   /* MENU_* */
    int8_t item;
    uint8_t confirm;                                /* RESET asked once */
    uint8_t redraw;
    uint32_t repeat_t;                              /* a held direction in the menu repeats */
} ui;
enum { MENU_OFF, MENU_MAIN, MENU_HELP };

static void menu_open(void);
static void menu_close(void);
static void menu_input(uint32_t pad, uint32_t pressed_pad, uint32_t pressed_btn);

static void palette_step(int32_t d)
{
    set.palette = (uint8_t)((set.palette + (uint32_t)((int32_t)NPALS + d)) % NPALS);
    set_dirty = 1;
    set_changed_ms = plat_ms();
    pal_apply();
    disp_invalidate();
}
static void scale_step(int32_t d)
{
    set.scale = (uint8_t)((set.scale + (uint32_t)((int32_t)NSCALE + d)) % NSCALE);
    set_dirty = 1;
    set_changed_ms = plat_ms();
    disp_invalidate();
    disp.blank = 1;                                 /* (1:1 leaves a border: the area cleared first) */
}

static void quick_save(void)
{
    int rc = state_save();
    app_say("STATE", rc == 0 ? "SAVED" : rc == 1 ? "SAVED (RAM)" : "NOT SAVED");
}

static void input(void)
{
    uint32_t keys = plat_keys(), btn = plat_buttons(), pad = pad_of(keys, btn);
    uint32_t pb = btn & ~ui.btn, pp = pad & ~ui.pad, dim = PAD_KEYS;
    int32_t e;
    if (ui.menu) {
        menu_input(pad, pp, pb);
    } else if (running) {
        ui.held_over &= pad;
        gb_set_buttons(&gb, pad & ~ui.held_over);
        ffwd = (uint8_t)((btn >> B_FX) & 1u);
        if (pb & BTN(B_HOME))
            menu_open();
        else if (pb & BTN(B_SAVE))
            quick_save();
        else if (pb & BTN(B_OCTDN))
            palette_step(-1);
        else if (pb & BTN(B_OCTUP))
            palette_step(1);
        if ((e = plat_enc(EN_PRESET)) != 0) {
            palette_step(e > 0 ? 1 : -1);
            app_say("PALETTE", PALS[set.palette].name);
        }
        if ((e = plat_enc(EN_ALGO)) != 0) {
            scale_step(e > 0 ? 1 : -1);
            app_say("SCREEN", SCALE_NAME[set.scale]);
        }
    } else if (pb & BTN(B_HOME)) {
        menu_open();
    }
    ui.keys = keys;
    ui.btn = btn;
    ui.pad = pad;
    plat_lights((btn & (BTN(B_HOME) | BTN(B_PLAY) | BTN(B_SEL) | BTN(B_FX) | BTN(B_SAVE))) | (ui.menu ? BTN(B_HOME) : 0u),
                keys & dim, BTN(B_HOME) | BTN(B_PLAY) | BTN(B_SEL) | BTN(B_FX) | BTN(B_SAVE), dim, 1);
}

void app_input_only(void)
{
    input();
    pump();
    if (running && !ui.menu)
        emulate(2000);                              /* (between frames too: small steps, the sound kept fed) */
}

/* --------------------------------------------------------------------------------------------- the menu --- */
enum { MI_RESUME, MI_PALETTE, MI_SCREEN, MI_METER, MI_SAVE, MI_LOAD, MI_RESET, MI_HELP, NMI };
static const char *const MI_NAME[NMI] = {"RESUME", "PALETTE", "SCREEN", "SPEED METER", "SAVE STATE", "LOAD STATE",
                                         "RESET GAME", "CONTROLS"};

static void band(int32_t top)                       /* a 48-row band of the screen, drawn in screen coordinates */
{
    cv_begin(240, 48, C_BLACK);
    cv_oy = -top;
}

static const char *menu_value(int i, char *t)
{
    t[0] = 0;
    if (i == MI_PALETTE)
        return PALS[set.palette].name;
    if (i == MI_SCREEN)
        return SCALE_NAME[set.scale];
    if (i == MI_METER)
        return set.meter ? "ON" : "OFF";
    if (i == MI_LOAD)
        return state_have ? "" : "NONE";
    if (i == MI_SAVE)
        return state_base ? "" : "RAM ONLY";
    if (i == MI_RESET && ui.confirm)
        return "A AGAIN";
    return t;
}

static void menu_draw_main(void)
{
    int32_t b, i;
    char t[48];
    for (b = 0; b < 5; b++) {
        band(b * 48);
        cv_rect(0, 0, 240, 28, C_LINE);
        cv_text(8, 6, &FONT_B, APP_NAME, C_HI);
        text_cpy(t, rom_state == ROM_OK && gb.title[0] ? gb.title : rom_state == ROM_OK ? rom_name : "NO GAME", 20);
        cv_text(232 - text_w(&FONT_S, t), 6, &FONT_S, t, C_WHITE);
        for (i = 0; i < NMI; i++) {
            int32_t y = 34 + i * 22;
            const char *v = menu_value(i, t);
            int sel = i == ui.item;
            if (sel)
                cv_rect(4, y - 2, 232, 21, C_DIM);
            cv_text(14, y, &FONT_B, MI_NAME[i], sel ? C_WHITE : C_GRAY);
            if (v[0])
                cv_text(226 - text_w(&FONT_B, v), y, &FONT_B, v, sel ? C_HI : C_AMB);
        }
        cv_text(8, 218, &FONT_XS, "A / PLAY: CHOOSE    B / HOME: BACK", C_GRAY);
        cv_blit(0, (uint32_t)(b * 48));
    }
}

static void menu_draw_help(void)
{
    static const char *const L[][2] = {
        {"D-PAD", "G#3 / A#3 UP   G3 LEFT"},
        {"", "A3 DOWN   B3 RIGHT"},
        {"B", "D5  D#5  E5"},
        {"A", "F5  F#5  G5"},
        {"SELECT", "F4  or  SEL"},
        {"START", "G4  or  PLAY"},
        {"HOME", "THIS MENU"},
        {"FX (HOLD)", "FAST FORWARD"},
        {"SAVE", "QUICK SAVE STATE"},
        {"PRESETS", "PALETTE"},
        {"ALGORITHM", "SCREEN"},
        {"MASTER", "VOLUME"},
    };
    int32_t b, i;
    for (b = 0; b < 5; b++) {
        band(b * 48);
        cv_rect(0, 0, 240, 28, C_LINE);
        cv_text(8, 6, &FONT_B, "CONTROLS", C_HI);
        cv_text(150, 8, &FONT_XS, "KEYS LIGHT UP", C_GRAY);
        for (i = 0; i < (int32_t)(sizeof L / sizeof L[0]); i++) {
            int32_t y = 34 + i * 15;
            cv_text(8, y, &FONT_XS, L[i][0], C_HI);
            cv_text(84, y, &FONT_XS, L[i][1], C_WHITE);
        }
        cv_text(8, 218, &FONT_XS, "B / HOME: BACK", C_GRAY);
        cv_blit(0, (uint32_t)(b * 48));
    }
}

static void menu_say(void)
{
    char t[64], v[16];
    if (!ann_on())
        return;
    t[0] = 0;
    text_cat(t, MI_NAME[(int)ui.item]);
    text_cpy(v, menu_value(ui.item, v), sizeof v);
    if (v[0]) {
        text_cat(t, " ");
        text_cat(t, v);
    }
    ann_say(t, 0, ANN_NOW);
}

static void menu_open(void)
{
    ui.menu = MENU_MAIN;
    ui.item = 0;
    ui.confirm = 0;
    ui.redraw = 1;
    aud_quiet = 1;
    if (running)
        gb_set_buttons(&gb, 0);
    ffwd = 0;
    disp.busy = 0;                                  /* (whatever was going out: the menu covers it) */
    if (running && sav_flush())
        app_say("GAME", "SAVED");
    menu_say();
}

static void menu_close(void)
{
    ui.menu = MENU_OFF;
    ui.held_over = ui.pad | pad_of(plat_keys(), plat_buttons());
    if (set_dirty) {
        if (plat_store_save(OBJ_SETTINGS, &set, sizeof set) == 0)
            set_dirty = 0;
    }
    lcd_fill(0, 0, 240, 240, C_BLACK);
    disp_invalidate();
    disp.busy = 0;
    disp.blank = 0;
    bar.redraw = 1;
    ui.redraw = 1;                                  /* (no game: its message again) */
}

static void menu_choose(void)
{
    switch (ui.item) {
    case MI_RESUME:
        menu_close();
        break;
    case MI_PALETTE:
        palette_step(1);
        break;
    case MI_SCREEN:
        scale_step(1);
        break;
    case MI_METER:
        set.meter ^= 1u;
        set_dirty = 1;
        break;
    case MI_SAVE:
        if (running) {
            menu_close();
            quick_save();
        }
        break;
    case MI_LOAD:
        if (running) {
            int ok = state_load() == 0;
            menu_close();
            app_say("STATE", ok ? "LOADED" : "NONE");
        }
        break;
    case MI_RESET:
        if (!ui.confirm) {
            ui.confirm = 1;
        } else if (running) {
            sav_flush();
            gb_reset(&gb);
            gb.fb = fbuf[0];
            menu_close();
            app_say("GAME", "RESET");
        }
        break;
    default:
        ui.menu = MENU_HELP;
        break;
    }
    ui.redraw = 1;
    menu_say();
}

static void menu_input(uint32_t pad, uint32_t pp, uint32_t pb)
{
    int32_t e = plat_enc(EN_SELECT), v = plat_enc(EN_ALGO) + plat_enc(EN_PRESET);
    uint32_t now = plat_ms();
    if (pad & (GB_UP | GB_DOWN)) {                  /* a held direction repeats */
        if (pp & (GB_UP | GB_DOWN))
            ui.repeat_t = now + 400u;
        else if ((int32_t)(now - ui.repeat_t) >= 0) {
            ui.repeat_t = now + 120u;
            pp |= pad & (GB_UP | GB_DOWN);
        }
    }
    if (ui.menu == MENU_HELP) {
        if ((pp & (GB_B | GB_A)) || (pb & (BTN(B_HOME) | BTN(B_PLAY)))) {
            ui.menu = MENU_MAIN;
            ui.redraw = 1;
        }
        return;
    }
    if (pp & GB_UP)
        e--;
    if (pp & GB_DOWN)
        e++;
    if (pp & GB_LEFT)
        v--;
    if (pp & GB_RIGHT)
        v++;
    if (e) {
        ui.item = (int8_t)(((ui.item + e) % NMI + NMI) % NMI);
        ui.confirm = 0;
        ui.redraw = 1;
        menu_say();
    }
    if (v && (ui.item == MI_PALETTE || ui.item == MI_SCREEN || ui.item == MI_METER)) {
        if (ui.item == MI_PALETTE)
            palette_step(v > 0 ? 1 : -1);
        else if (ui.item == MI_SCREEN)
            scale_step(v > 0 ? 1 : -1);
        else {
            set.meter ^= 1u;
            set_dirty = 1;
        }
        ui.redraw = 1;
        menu_say();
    }
    if ((pp & (GB_A | GB_START)) || (pb & BTN(B_PLAY)))
        menu_choose();
    else if ((pp & GB_B) || (pb & BTN(B_HOME)))
        menu_close();
}

/* ------------------------------------------------------------------------------------- no game, or a bad one --- */
static void message_draw(const char *a, const char *b, const char *c)
{
    int32_t i;
    for (i = 0; i < 5; i++) {
        band(i * 48);
        cv_text((240 - text_w(&FONT_M, APP_NAME)) / 2, 40, &FONT_M, APP_NAME, C_HI);
        cv_text((240 - text_w(&FONT_B, a)) / 2, 100, &FONT_B, a, C_WHITE);
        cv_text((240 - text_w(&FONT_XS, b)) / 2, 130, &FONT_XS, b, C_GRAY);
        cv_text((240 - text_w(&FONT_XS, c)) / 2, 148, &FONT_XS, c, C_GRAY);
        cv_blit(0, (uint32_t)(i * 48));
    }
}

static void no_game_draw(void)
{
    if (rom_state == ROM_NONE)
        message_draw("NO GAME IN THIS FIRMWARE", "INSTALL IT WITH YOUR OWN ROM:", "TOOLS/FM1GB.PY OR THE INSTALL PAGE");
    else if (rom_state != ROM_OK)
        message_draw("THE GAME IS DAMAGED", "INSTALL IT AGAIN", "");
    else if (load_rc == GB_E_CGB_ONLY)
        message_draw("A GAME BOY COLOR GAME", "THIS PLAYS ORIGINAL GAME BOY GAMES", "(AND COLOR GAMES THAT RUN ON ONE)");
    else if (load_rc == GB_E_MAPPER)
        message_draw("THIS CARTRIDGE TYPE", "IS NOT SUPPORTED", "(MBC1, MBC2, MBC3, MBC5 ARE)");
    else
        message_draw("THE GAME WOULD NOT START", "", "");
}

/* ----------------------------------------------------------------------------------------- the platform --- */
void app_init(void)
{
    settings_load();
    pal_apply();
    rom_find();
    game_start();
}

void app_ui_init(void)
{
    lcd_fill(0, 0, 240, 240, C_BLACK);
    palette_set(4);                                 /* the menu's own colours: gfx.c's MONO */
    {
        static const uint16_t P[5] = {RGB(40, 48, 24), RGB(70, 84, 40), RGB(150, 160, 130), RGB(139, 172, 15),
                                      RGB(155, 188, 15)};
        uint32_t i;
        for (i = 0; i < 5u; i++)
            pal[i] = P[i];
    }
    disp_invalidate();
    disp.busy = 0;
    ui.redraw = 1;
    bar.redraw = 1;
    if (!running)
        no_game_draw();
}

void app_frame(void)
{
    uint32_t now = plat_ms();
    input();
    ann_tick(now);
    if (ui.menu) {
        if (ui.redraw) {
            ui.redraw = 0;
            if (ui.menu == MENU_HELP)
                menu_draw_help();
            else
                menu_draw_main();
        }
        if (running)
            sav_poll(now);
        return;
    }
    if (!running) {
        if (ui.redraw) {
            ui.redraw = 0;
            no_game_draw();
        }
        return;
    }
    emulate(12000);
    sav_poll(now);
    if (set_dirty && now - set_changed_ms >= 4000u && !sw.on) {   /* a palette or screen changed in play: kept */
        if (plat_store_save(OBJ_SETTINGS, &set, sizeof set) == 0)
            set_dirty = 0;
        else
            set_changed_ms = now;
    }
    if (now - bar.fps_t >= 1000u) {                 /* the speed meter, once a second */
        bar.fps_shown = disp.shown - bar.fps_frames;
        bar.fps_frames = disp.shown;
        bar.emu_us_shown = bar.emu_us * 100u / (now - bar.fps_t ? now - bar.fps_t : 1u) / 10u;
        bar.emu_us = 0;
        bar.fps_t = now;
        if (set.meter)
            bar.redraw = 1;
    }
    if (bar.until && now >= bar.until) {
        bar.until = 0;
        bar.redraw = 1;
    }
    {
        static uint8_t was_saving;
        if (sw.on != was_saving) {
            was_saving = sw.on;
            bar.redraw = 1;
        }
    }
    if (bar.redraw) {
        bar.redraw = 0;
        bar_draw();
    }
    pump();
}

#ifdef PLAT_HOST
int app_expect(const char *what, int *got)
{
    if (!strcmp(what, "running"))
        *got = running;
    else if (!strcmp(what, "frames"))
        *got = (int)gb.frames;
    else if (!strcmp(what, "shown"))
        *got = (int)disp.shown;
    else if (!strcmp(what, "under"))
        *got = (int)aud_under;
    else if (!strcmp(what, "saves"))
        *got = (int)sav_count;
    else if (!strcmp(what, "save_bytes"))
        *got = (int)sav_len;
    else if (!strcmp(what, "menu"))
        *got = ui.menu;
    else if (!strcmp(what, "palette"))
        *got = set.palette;
    else if (!strcmp(what, "scale"))
        *got = set.scale;
    else if (!strcmp(what, "state"))
        *got = state_have;
    else if (!strcmp(what, "rom_misses"))
        *got = (int)rom_misses;
    else if (!strcmp(what, "rom_bad"))
        *got = (int)rom_bad;
    else if (!strcmp(what, "rom_state"))
        *got = rom_state;
    else if (!strcmp(what, "pc"))
        *got = gb.pc;
    else if (!strcmp(what, "gbx"))                  /* (the test cartridge's block and counter: gb/tools/mkdemo.py) */
        *got = gb.wram[0];
    else if (!strcmp(what, "count"))
        *got = gb.wram[3];
    else
        return 0;
    return 1;
}

int app_host_cmd(const char *cmd, const char *a, const char *b, const char *c, const char *d)
{
    (void)b, (void)c, (void)d;
    if (!strcmp(cmd, "rom")) {                      /* rom FILE: that game, from its start (a power-on) */
        crc_init();
        rom_state = rom_load_file(a);
        running = 0;
        game_start();
        app_ui_init();
        return 1;
    }
    if (!strcmp(cmd, "gbpeek")) {                   /* gbpeek ADDR: a byte of the Game Boy's memory, printed */
        uint32_t addr = (uint32_t)strtoul(a, 0, 16) & 0xFFFFu;
        const uint8_t *p = gb.rmap[addr >> 12];
        printf("gb %04X = %02X\n", addr, p ? p[addr & 0xFFFu] : 0xFFu);
        return 1;
    }
    return 0;
}
#endif
