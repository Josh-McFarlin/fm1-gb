/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 The fm1-gb authors */
/* The app's hot loops (fast.h), at -O2. */
#include "fast.h"

#define AVG(x, y) (((x) & (y)) + ((((x) ^ (y)) & 0xF7DEu) >> 1))   /* two RGB565 colours' average, per field */

void fast_pal_make(fast_pal_t *p, const uint16_t rgb565[16])
{
    uint32_t a, b;
    for (a = 0; a < 16u; a++)
        p->px[a] = fast_swap(rgb565[a]);
    for (a = 0; a < 16u; a++)
        for (b = 0; b < 16u; b++) {
            uint32_t m = AVG((uint32_t)rgb565[a], (uint32_t)rgb565[b]) & 0xFFFFu;
            p->raw[a * 16u + b] = (uint16_t)m;
            p->mix[a * 16u + b] = fast_swap(m);
        }
}

void fast_rows_15(uint16_t *dst, const uint8_t *src, uint32_t rows, const fast_pal_t *p, int smooth)
{
    const uint16_t *px = p->px, *mix = p->mix, *raw = p->raw;
    uint32_t y, i;
    for (y = 0; y < rows; y += 2u, src += 320, dst += 720) {
        const uint8_t *r0 = src, *r1 = src + 160;
        uint16_t *d0 = dst, *d1 = dst + 240, *d2 = dst + 480;
        if (smooth) {
            for (i = 0; i < 80u; i++, r0 += 2, r1 += 2, d0 += 3, d1 += 3, d2 += 3) {
                uint32_t a = r0[0], b = r0[1], c = r1[0], d = r1[1];
                uint32_t ab = a << 4 | b, cd = c << 4 | d;
                d0[0] = px[a];
                d0[1] = mix[ab];
                d0[2] = px[b];
                d1[0] = mix[a << 4 | c];
                d1[1] = fast_swap(AVG((uint32_t)raw[ab], (uint32_t)raw[cd]));
                d1[2] = mix[b << 4 | d];
                d2[0] = px[c];
                d2[1] = mix[cd];
                d2[2] = px[d];
            }
        } else {
            for (i = 0; i < 80u; i++, r0 += 2, r1 += 2, d0 += 3, d1 += 3, d2 += 3) {
                uint16_t a = px[r0[0]], b = px[r0[1]], c = px[r1[0]], d = px[r1[1]];
                d0[0] = a;
                d0[1] = a;
                d0[2] = b;
                d1[0] = a;
                d1[1] = a;
                d1[2] = b;
                d2[0] = c;
                d2[1] = c;
                d2[2] = d;
            }
        }
    }
}

void fast_rows_1(uint16_t *dst, const uint8_t *src, uint32_t rows, const fast_pal_t *p)
{
    const uint16_t *px = p->px;
    uint32_t n = rows * 160u, i;
    for (i = 0; i < n; i += 4u) {
        dst[i] = px[src[i]];
        dst[i + 1u] = px[src[i + 1u]];
        dst[i + 2u] = px[src[i + 2u]];
        dst[i + 3u] = px[src[i + 3u]];
    }
}

uint32_t fast_hash(const uint8_t *p, uint32_t n)
{
    const uint32_t *w = (const uint32_t *)(const void *)p;
    uint32_t h = 2166136261u, i;
    for (i = 0; i < n / 4u; i++)
        h = (h ^ w[i]) * 16777619u;
    return h;
}

int32_t fast_lz4(uint8_t *dst, uint32_t dst_n, const uint8_t *src, uint32_t src_n)
{
    const uint8_t *s = src, *s_end = src + src_n;
    uint8_t *d = dst, *d_end = dst + dst_n;
    while (s < s_end) {
        uint32_t tok = *s++, n = tok >> 4, off;
        const uint8_t *m;
        if (n == 15u) {
            uint32_t b;
            do {
                if (s >= s_end)
                    return -1;
                b = *s++;
                n += b;
            } while (b == 255u);
        }
        if (n > (uint32_t)(s_end - s) || n > (uint32_t)(d_end - d))
            return -1;
        while (n--)
            *d++ = *s++;
        if (s >= s_end)
            break;                                      /* the last sequence: literals only */
        if (s_end - s < 2)
            return -1;
        off = (uint32_t)s[0] | (uint32_t)s[1] << 8;
        s += 2;
        if (!off || off > (uint32_t)(d - dst))
            return -1;
        n = (tok & 15u) + 4u;
        if ((tok & 15u) == 15u) {
            uint32_t b;
            do {
                if (s >= s_end)
                    return -1;
                b = *s++;
                n += b;
            } while (b == 255u);
        }
        if (n > (uint32_t)(d_end - d))
            return -1;
        m = d - off;
        while (n--)
            *d++ = *m++;                                /* (byte by byte: an overlapping match repeats) */
    }
    return (int32_t)(d - dst);
}
