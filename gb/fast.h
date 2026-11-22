/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 The fm1-gb authors */
/* The app's hot loops, compiled apart at -O2 (fm1.toml o2): the Game Boy's lines to the LCD's pixels, a strip's hash
 * (has it changed since it was sent), and LZ4 for a ROM's compressed banks. */
#pragma once
#include <stdint.h>

/* the LCD's pixels are RGB565 with the high byte first (as the SPI sends them): a uint16_t with its bytes swapped */
static inline uint16_t fast_swap(uint32_t c) { return (uint16_t)(((c >> 8) & 0xFFu) | ((c & 0xFFu) << 8)); }

/* a palette for the conversions: the 16 codes of gb.h (GB_PAL_* | shade), and their pairs' averages (smooth 1.5x) */
typedef struct {
    uint16_t px[16];                     /* each code's colour, bytes swapped */
    uint16_t mix[256];                   /* the average of codes a (high nibble) and b, swapped */
    uint16_t raw[256];                   /* the same, not swapped (for the average of four) */
} fast_pal_t;
void fast_pal_make(fast_pal_t *p, const uint16_t rgb565[16]);

/* rows source rows of 160 codes (rows even) to rows * 3 / 2 LCD rows of 240 pixels: smooth (the in-between pixels
 * the average of their neighbours) or sharp (each source pixel 2 or 1 wide and high) */
void fast_rows_15(uint16_t *dst, const uint8_t *src, uint32_t rows, const fast_pal_t *p, int smooth);
/* rows source rows to as many LCD rows of 160 pixels */
void fast_rows_1(uint16_t *dst, const uint8_t *src, uint32_t rows, const fast_pal_t *p);
/* a hash of n bytes (n a multiple of 4, p 4-aligned) */
uint32_t fast_hash(const uint8_t *p, uint32_t n);
/* LZ4's block format into dst (dst_n its room) -> the bytes made, -1 a broken block */
int32_t fast_lz4(uint8_t *dst, uint32_t dst_n, const uint8_t *src, uint32_t src_n);
