/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 The fm1-gb authors */
/* A Game Boy (DMG) emulator core: the SM83 CPU, the PPU (a scanline renderer), the APU (four channels, box-filtered
 * to the host's sample rate), the timer, the joypad, serial (no partner), OAM DMA and the cartridge mappers (none,
 * MBC1, MBC2, MBC3 with its clock, MBC5). Plain C99, integers only, no allocation and no libc beyond memcpy/memset:
 * the same file runs on the FM-1, in fm1-base's host simulator and in a desktop test harness.
 *
 * The host gives it the cartridge (gb_load: the ROM through a bank callback, so a ROM may be in place in flash or
 * decompressed bank by bank), a frame buffer (160 x 144 bytes, one code a pixel: gb_t.fb), a ring for the sound
 * (stereo int16) and the buttons; it runs the machine in slices (gb_run), and is told of each frame at VBlank. */
#pragma once
#include <stdint.h>

#define GB_W 160
#define GB_H 144
#define GB_CLOCK 4194304u                /* T-cycles a second */
#define GB_FRAME 70224                   /* T-cycles a frame (59.73 Hz) */
#define GB_LINE 456                      /* T-cycles a line */

/* gb_set_buttons: held, a bit each */
enum { GB_RIGHT = 1, GB_LEFT = 2, GB_UP = 4, GB_DOWN = 8, GB_A = 16, GB_B = 32, GB_SELECT = 64, GB_START = 128 };

/* a pixel in gb_t.fb: the palette it went through (GB_PAL_BG, _OBJ0, _OBJ1) times 4, plus its shade 0 (lightest)..3 */
enum { GB_PAL_BG = 0, GB_PAL_OBJ0 = 4, GB_PAL_OBJ1 = 8 };

/* what gb_load found in the header */
enum { GB_OK = 0, GB_E_SIZE = -1, GB_E_MAPPER = -2, GB_E_CGB_ONLY = -3 };
enum { MBC_NONE, MBC_1, MBC_2, MBC_3, MBC_5 };

typedef struct gb gb_t;
/* the ROM's 16 KiB bank n (n < rom_banks), for slot 0 (0x0000..0x3FFF) or slot 1 (0x4000..0x7FFF): the pointer stays
 * valid until the next call for the same slot */
typedef const uint8_t *(*gb_bank_fn)(void *user, uint32_t bank, int slot);

struct gb_sq {                           /* a square channel (1 and 2), the noise (4) shares the envelope and length */
    uint8_t on, dac, duty, pos, vol, env_dir, env_per, env_t, len_on, out;
    int32_t timer, period;
    uint32_t len;
    uint16_t freq;
    /* sweep (channel 1) */
    uint8_t sw_per, sw_t, sw_dir, sw_shift, sw_on, sw_neg;
    uint16_t sw_freq;
};
struct gb_wave {
    uint8_t on, dac, pos, vol_shift, len_on, sample, out;
    int32_t timer, period;
    uint32_t len;
    uint16_t freq;
};
struct gb_noise {
    uint8_t on, dac, vol, env_dir, env_per, env_t, len_on, out, narrow;
    int32_t timer, period;
    uint32_t len;
    uint16_t lfsr;
};

struct gb {
    /* the CPU */
    uint16_t pc, sp;
    uint8_t a, f, b, c, d, e, h, l;
    uint8_t ime, ei_delay, halted, halt_bug, locked, stopped;
    uint8_t ie, iflag;
    /* time: T-cycles since the frame began (every timestamp moves back a frame at each frame's start) */
    int32_t clock;                       /* now */
    int32_t run_end;                     /* gb_run's end */
    int32_t limit;                       /* the CPU's next stop: min(next event, run_end) */
    int32_t ppu_next, tim_next, ser_next, fs_next;
    /* the memory map: 4 KiB pages, read in place (0: the slow path) */
    const uint8_t *rmap[16];
    uint8_t *wmap[16];
    uint8_t vram[0x2000], wram[0x2000], oam[0xA0], hram[0x80], io[0x80];
    /* the cartridge */
    gb_bank_fn bank_fn;
    void *bank_user;
    const uint8_t *rom0, *romx;
    uint32_t rom_banks, rom_bank, rom0_bank;
    uint8_t mbc, ram_on, ram_bank, mbc_mode, mbc1_lo, mbc1_hi, has_rtc, has_battery;
    uint8_t *eram;                       /* the cartridge's RAM (the host's buffer) */
    uint32_t eram_size;                  /* 0, 512 (MBC2), 2 KiB .. 32 KiB */
    uint32_t eram_writes;                /* writes to it since the start: the host saves when this moves */
    uint32_t eram_off_count;             /* times the game switched its RAM off after writing (a save done) */
    uint8_t rtc[5], rtc_latched[5], rtc_latch_arm;   /* MBC3's clock: s, m, h, day low, day high (bit 6 halt) */
    int32_t rtc_sub;                     /* T-cycles into the current second */
    char title[17];
    uint8_t header_sum;
    uint16_t global_sum;
    /* the timer */
    int32_t div_base;                    /* when the 16-bit divider was 0 */
    int32_t tima_time;                   /* tima_val held at this time */
    uint8_t tima_val;
    /* serial */
    void (*serial)(gb_t *g, uint8_t byte);   /* each byte sent (a test ROM's text); 0: none */
    /* joypad */
    uint8_t buttons;
    /* the PPU */
    uint8_t mode, ly, stat_line, win_line, win_y_hit, lcd_on;
    int32_t line_start;
    uint8_t *fb;                         /* the frame being drawn: GB_W x GB_H codes (the host may swap it at VBlank) */
    uint8_t skip_render;                 /* 1: lines are not drawn (a frame the host will not show) */
    void (*vblank)(gb_t *g);             /* each frame done (also while the LCD is off: a blank frame) */
    void *user;
    uint32_t frames;
    /* the APU */
    struct gb_sq ch1, ch2;
    struct gb_wave ch3;
    struct gb_noise ch4;
    uint8_t apu_on, fs_step;
    int32_t apu_clock;                   /* the APU has run up to this time */
    uint32_t smp_frac, smp_step;         /* 16.16: T-cycles a sample */
    int32_t acc[8];                      /* the sample being gathered: channels x (left, right) folded below */
    int32_t acc_n, acc_want;             /* T-cycles gathered, and wanted for this sample */
    int32_t hp_l, hp_r, hp_xl, hp_xr;    /* the output's high-pass (the DMG's capacitor) */
    int16_t *abuf;                       /* the sound: stereo int16 frames into a ring of amask + 1 frames */
    uint32_t amask;
    volatile uint32_t awr;               /* frames written (the reader keeps its own count) */
};

/* the cartridge: rom_size its bytes (a multiple of 16 KiB, as the header says or more), bank_fn its banks; eram the
 * RAM buffer (at least gb_ram_size(header) bytes; 0 when it has none). -> GB_OK or GB_E_* */
int gb_ram_size(const uint8_t *header0);   /* the cartridge RAM's bytes from bank 0 (0x147, 0x149) */
int gb_load(gb_t *g, gb_bank_fn bank_fn, void *bank_user, uint32_t rom_size, uint8_t *eram);
void gb_reset(gb_t *g);                  /* the state after the boot ROM (DMG) */
void gb_audio(gb_t *g, int16_t *ring, uint32_t frames_pow2, uint32_t rate);
void gb_set_buttons(gb_t *g, uint32_t held);
void gb_run(gb_t *g, int32_t cycles);    /* the machine for cycles T-cycles (or a little more: whole instructions) */
const char *gb_mapper_name(const gb_t *g);

/* the state as bytes (a save state): gb_state_size, then gb_state_save into a buffer that size; gb_state_load ->
 * 0 = loaded, -1 another game's or another version's (nothing changed). The cartridge RAM is in it */
uint32_t gb_state_size(const gb_t *g);
void gb_state_save(const gb_t *g, uint8_t *dst);
int gb_state_load(gb_t *g, const uint8_t *src, uint32_t n);
