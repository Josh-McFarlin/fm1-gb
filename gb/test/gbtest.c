/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 The fm1-gb authors */
/* The core on a computer, for test ROMs and timing:
 *   gbtest ROM [-f FRAMES] [-o OUT.pgm] [-w OUT.wav] [-s] [-q] [-b FRAME:BUTTONS ...] [-t]
 * -s prints what the ROM sends over serial (Blargg's tests), -q stops once the serial text says Passed / Failed, -t
 * prints the host time a frame. Exit 0; with -m (Mooneye), 0 only when the registers hold the pass pattern. */
#include "gb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint8_t *rom;
static uint32_t rom_size;
static char serial_text[65536];
static uint32_t serial_n;

static const uint8_t *bank(void *u, uint32_t n, int slot)
{
    (void)u;
    (void)slot;
    return rom + n * 0x4000u;
}

static void on_serial(gb_t *g, uint8_t b)
{
    (void)g;
    if (serial_n < sizeof serial_text - 1)
        serial_text[serial_n++] = (char)b;
    serial_text[serial_n] = 0;
}

static gb_t G;
static uint8_t fb[GB_W * GB_H], eram[32768];
static int16_t ring[1 << 16];

static void write_pgm(const char *path)
{
    static const uint8_t SHADE[4] = {255, 170, 85, 0};
    FILE *f = fopen(path, "wb");
    uint32_t i;
    if (!f)
        return;
    fprintf(f, "P5\n%d %d\n255\n", GB_W, GB_H);
    for (i = 0; i < GB_W * GB_H; i++)
        fputc(SHADE[fb[i] & 3u], f);
    fclose(f);
}

static void write_wav(const char *path, const int16_t *s, uint32_t frames)
{
    FILE *f = fopen(path, "wb");
    uint32_t bytes = frames * 4u, v;
    if (!f)
        return;
    fwrite("RIFF", 1, 4, f);
    v = 36u + bytes; fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    v = 16; fwrite(&v, 4, 1, f);
    v = 0x00020001u; fwrite(&v, 4, 1, f);
    v = 44100; fwrite(&v, 4, 1, f);
    v = 44100u * 4u; fwrite(&v, 4, 1, f);
    v = 0x00100004u; fwrite(&v, 4, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&bytes, 4, 1, f);
    fwrite(s, 4, frames, f);
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *out = 0, *wav = 0;
    int frames = 600, show_serial = 0, quit_serial = 0, mooneye = 0, timing = 0, i, rc;
    uint32_t presses[64][2], npress = 0, rd = 0, wav_n = 0, wav_cap = 0;
    int16_t *wav_buf = 0;
    FILE *f;
    long n;
    clock_t t0;
    if (argc < 2) {
        fprintf(stderr, "usage: gbtest ROM [-f FRAMES] [-o OUT.pgm] [-w OUT.wav] [-s] [-q] [-m] [-t] [-b FRAME:HEX]\n");
        return 2;
    }
    for (i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-f") && i + 1 < argc)
            frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-o") && i + 1 < argc)
            out = argv[++i];
        else if (!strcmp(argv[i], "-w") && i + 1 < argc)
            wav = argv[++i];
        else if (!strcmp(argv[i], "-s"))
            show_serial = 1;
        else if (!strcmp(argv[i], "-q"))
            quit_serial = 1;
        else if (!strcmp(argv[i], "-m"))
            mooneye = 1;
        else if (!strcmp(argv[i], "-t"))
            timing = 1;
        else if (!strcmp(argv[i], "-b") && i + 1 < argc && npress < 64) {
            unsigned fr, bt;
            if (sscanf(argv[++i], "%u:%x", &fr, &bt) == 2) {
                presses[npress][0] = fr;
                presses[npress][1] = bt;
                npress++;
            }
        }
    }
    f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    rom_size = (uint32_t)((n + 0x3FFF) & ~0x3FFF);
    if (rom_size < 0x8000u)
        rom_size = 0x8000u;
    rom = malloc(rom_size);
    memset(rom, 0xFF, rom_size);
    if (fread(rom, 1, (size_t)n, f) != (size_t)n)
        return 2;
    fclose(f);
    G.fb = fb;
    G.serial = on_serial;
    rc = gb_load(&G, bank, 0, rom_size, eram);
    if (rc != GB_OK)
        fprintf(stderr, "gb_load: %d\n", rc);
    gb_audio(&G, ring, 1u << 15, 44100);
    gb_reset(&G);
    if (wav) {
        wav_cap = (uint32_t)frames * 800u;
        wav_buf = malloc(wav_cap * 4u);
    }
    t0 = clock();
    for (i = 0; i < frames; i++) {
        uint32_t k, start = G.frames;
        for (k = 0; k < npress; k++)
            if (presses[k][0] == (uint32_t)i)
                gb_set_buttons(&G, presses[k][1]);
        while (G.frames == start)
            gb_run(&G, GB_LINE);
        while (rd != G.awr) {
            uint32_t r = rd & G.amask;
            if (wav_buf && wav_n < wav_cap) {
                wav_buf[2u * wav_n] = ring[2u * r];
                wav_buf[2u * wav_n + 1u] = ring[2u * r + 1u];
                wav_n++;
            }
            rd++;
        }
        if (quit_serial && (strstr(serial_text, "Passed") || strstr(serial_text, "Failed")))
            break;
        if (quit_serial && eram[1] == 0xDE && eram[2] == 0xB0 && eram[3] == 0x61 && eram[0] != 0x80)
            break;
    }
    if (timing)
        printf("%d frames, %.3f ms a frame (host)\n", i, (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC / (i ? i : 1));
    if (show_serial)
        printf("%s\n", serial_text);
    if (show_serial && eram[1] == 0xDE && eram[2] == 0xB0 && eram[3] == 0x61)   /* Blargg's result in the cartridge RAM */
        printf("[status %02X] %s\n", eram[0], (const char *)eram + 4);
    if (out)
        write_pgm(out);
    if (wav_buf)
        write_wav(wav, wav_buf, wav_n);
    if (mooneye) {
        int pass = G.b == 3 && G.c == 5 && G.d == 8 && G.e == 13 && G.h == 21 && G.l == 34;
        printf("%s %s\n", pass ? "PASS" : "FAIL", argv[1]);
        return pass ? 0 : 1;
    }
    return 0;
}
