// bbc_headless.c
//
// Headless BBC Micro runner for automated tests: no window, no audio.
//
//   bbc_headless [-f frames] [-t text] [-p out.ppm] [-r ram.bin] [-s] [-0 disc.ssd] [-b]
//
//   -f N      run N frames of 20 ms (default 100)
//   -t TEXT   type TEXT after -w frames (default 50; \n = RETURN), one key per 2 frames;
//             a '~' in TEXT pauses typing for 50 frames
//   -h N      hold each key N frames (default 3)
//   -p FILE   write the 640x256 framebuffer as a binary PPM
//   -r FILE   write the 32 KB RAM
//   -s        print the 40x25 MODE 7 screen ($7C00) as ASCII
//   -d        disable the DFS ROM (bank 14 empty)
//   -a FILE   write the sound output as a 22050 Hz 8-bit mono WAV
//   -M        print a summary of the MOS entry points called (OSBYTE/OSWORD by A, VDU codes)
//   -T FILE   log every MOS call (entry, A, X, Y, PC of caller) to FILE
//   -0 FILE   insert FILE (.ssd or .dsd) in drive 0
//   -W FILE   write the (possibly modified) drive 0 image to FILE at the end
//   -b        hold SHIFT during the first 40 frames (SHIFT+BREAK auto-boot)
//
// ## zlib/libpng license
//
// Copyright (c) 2026 bmarty
// This software is provided 'as-is', without any express or implied warranty.
// In no event will the authors be held liable for any damages arising from the
// use of this software.
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
//     1. The origin of this software must not be misrepresented; you must not
//     claim that you wrote the original software. If you use this software in a
//     product, an acknowledgment in the product documentation would be
//     appreciated but is not required.
//     2. Altered source versions must be plainly marked as such, and must not
//     be misrepresented as being the original software.
//     3. This notice may not be removed or altered from any source
//     distribution.

#define CHIPS_IMPL

#define __in_flash()
#define __not_in_flash()

#define RGBA8(r, g, b) (0xFF000000 | ((r) << 16) | ((g) << 8) | (b))

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "roms/bbc_roms.h"

#include "chips/chips_common.h"
#include "chips/mos6502cpu.h"
#include "chips/mos6522via.h"
#include "chips/mem.h"
#include "chips/clk.h"
#include "devices/wd1770.h"
#include "systems/bbc.h"
#include "systems/bbc_keys.h"

static bbc_t bbc;

/*-- MOS call tracing (US-01: which MOS services a program uses) ------------*/
static bool mos_summary;
static FILE* mos_log;
static bool mos_stop;
static uint32_t mos_calls[0x100];       // Per entry point low byte ($B9..$F7)
static uint32_t osbyte_calls[0x100];
static uint32_t osword_calls[0x100];
static uint32_t vdu_calls[0x100];
static uint32_t oscli_calls;

static const char* mos_name(uint8_t lo) {
    switch (lo) {
        case 0xB9: return "OSRDRM"; case 0xBF: return "OSEVEN"; case 0xC2: return "GSINIT"; case 0xC5: return "GSREAD";
        case 0xC8: return "NVRDCH"; case 0xCB: return "NVWRCH"; case 0xCE: return "OSFIND"; case 0xD1: return "OSGBPB";
        case 0xD4: return "OSBPUT"; case 0xD7: return "OSBGET"; case 0xDA: return "OSARGS"; case 0xDD: return "OSFILE";
        case 0xE0: return "OSRDCH"; case 0xE3: return "OSASCI"; case 0xE7: return "OSNEWL"; case 0xEE: return "OSWRCH";
        case 0xF1: return "OSWORD"; case 0xF4: return "OSBYTE"; case 0xF7: return "OSCLI";
        default: return 0;
    }
}

static void mos_debug_cb(void* user_data, uint64_t pins) {
    (void)user_data; (void)pins;
    if (!bbc.cpu.sync) return;
    uint16_t pc = bbc.cpu.PC;
    if (pc < 0xFFB9 || pc > 0xFFF7) return;
    const char* name = mos_name((uint8_t)pc);
    if (!name) return;
    uint8_t a = bbc.cpu.A, x = bbc.cpu.X, y = bbc.cpu.Y;
    mos_calls[(uint8_t)pc]++;
    if (pc == 0xFFF4) osbyte_calls[a]++;
    else if (pc == 0xFFF1) osword_calls[a]++;
    else if (pc == 0xFFEE || pc == 0xFFE3) vdu_calls[a]++;
    else if (pc == 0xFFF7) oscli_calls++;
    if (mos_log) {
        // Caller: return address on the stack (JSR pushes PC-1)
        uint16_t sp = (uint16_t)(0x100 | ((bbc.cpu.S + 1) & 0xFF));
        uint16_t ret = (uint16_t)((bbc.ram[sp] | (bbc.ram[(uint16_t)(sp + 1)] << 8)) + 1);
        fprintf(mos_log, "%10u %s A=%02X X=%02X Y=%02X from %04X", bbc.system_ticks, name, a, x, y, (uint16_t)(ret - 3));
        if (pc == 0xFFF7) {
            uint16_t p = (uint16_t)(x | (y << 8));
            fputs("  \"", mos_log);
            for (int i = 0; i < 40 && bbc.ram[(uint16_t)(p + i)] != 13; i++) fputc(bbc.ram[(uint16_t)(p + i)], mos_log);
            fputc('"', mos_log);
        }
        fputc('\n', mos_log);
    }
}

static void mos_print_summary(void) {
    printf("--- appels MOS ---\n");
    for (int lo = 0xB9; lo <= 0xF7; lo++) {
        if (mos_calls[lo]) printf("  %-7s $FF%02X : %u\n", mos_name((uint8_t)lo), lo, mos_calls[lo]);
    }
    printf("  OSBYTE :");
    for (int a = 0; a < 256; a++) if (osbyte_calls[a]) printf(" &%02X(%u)", a, osbyte_calls[a]);
    printf("\n  OSWORD :");
    for (int a = 0; a < 256; a++) if (osword_calls[a]) printf(" &%02X(%u)", a, osword_calls[a]);
    printf("\n  VDU    :");
    for (int a = 0; a < 32; a++) if (vdu_calls[a]) printf(" %d(%u)", a, vdu_calls[a]);
    printf("  (>=32 : %u)\n", (unsigned)({ uint32_t t = 0; for (int a = 32; a < 256; a++) t += vdu_calls[a]; t; }));
}

static FILE* wav;
static uint32_t wav_samples;

static void audio_callback(const uint8_t sample, void* user_data) {
    (void)user_data;
    if (wav) {
        fputc(sample, wav);
        wav_samples++;
    }
}

static void wav_header(FILE* f, uint32_t n) {
    uint32_t rate = 22050, bytes = n;
    uint8_t h[44] = {'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ', 16,0,0,0, 1,0, 1,0, 0,0,0,0, 0,0,0,0, 1,0, 8,0, 'd','a','t','a', 0,0,0,0};
    uint32_t riff = 36 + bytes;
    memcpy(h + 4, &riff, 4); memcpy(h + 24, &rate, 4); memcpy(h + 28, &rate, 4); memcpy(h + 40, &bytes, 4);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
}

static void write_ppm(const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", BBC_SCREEN_WIDTH, BBC_SCREEN_HEIGHT);
    for (int y = 0; y < BBC_SCREEN_HEIGHT; y++) {
        for (int x = 0; x < BBC_SCREEN_WIDTH; x += 2) {
            uint8_t b = bbc.fb[y * (BBC_SCREEN_WIDTH / 2) + x / 2];
            uint8_t px[2] = {(uint8_t)(b >> 4), (uint8_t)(b & 0x0F)};
            for (int i = 0; i < 2; i++) {
                uint32_t c = bbc_palette[px[i] & 7];
                uint8_t rgb[3] = {(uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c};
                fwrite(rgb, 1, 3, f);
            }
        }
    }
    fclose(f);
}

static void print_mode7(void) {
    for (int row = 0; row < 25; row++) {
        char line[41];
        for (int col = 0; col < 40; col++) {
            uint8_t c = bbc.ram[0x7C00 + row * 40 + col] & 0x7F;
            line[col] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
        }
        line[40] = 0;
        printf("%s\n", line);
    }
}

int main(int argc, char** argv) {
    int frames = 100;
    const char* text = NULL;
    const char* ppm = NULL;
    const char* ram = NULL;
    bool show = false;
    bool dfs = true;
    const char* disc = NULL;
    const char* disc_out = NULL;
    bool boot = false;
    const char* wav_path = NULL;
    const char* mos_log_path = NULL;
    int wait_frames = 50;
    int pause_until = 0;
    int hold_frames = 3;
    int held = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) text = argv[++i];
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) ppm = argv[++i];
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) ram = argv[++i];
        else if (!strcmp(argv[i], "-s")) show = true;
        else if (!strcmp(argv[i], "-d")) dfs = false;
        else if (!strcmp(argv[i], "-0") && i + 1 < argc) disc = argv[++i];
        else if (!strcmp(argv[i], "-W") && i + 1 < argc) disc_out = argv[++i];
        else if (!strcmp(argv[i], "-b")) boot = true;
        else if (!strcmp(argv[i], "-a") && i + 1 < argc) wav_path = argv[++i];
        else if (!strcmp(argv[i], "-M")) mos_summary = true;
        else if (!strcmp(argv[i], "-T") && i + 1 < argc) mos_log_path = argv[++i];
        else if (!strcmp(argv[i], "-w") && i + 1 < argc) wait_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-h") && i + 1 < argc) hold_frames = atoi(argv[++i]);
        else {
            fprintf(stderr, "usage: %s [-f frames] [-t text] [-p out.ppm] [-r ram.bin] [-s] [-d] [-0 disc.ssd] [-b]\n", argv[0]);
            return 2;
        }
    }

    bbc_desc_t desc = {
        .audio = {.callback = {.func = audio_callback}, .sample_rate = 22050},
        .roms = {
            .os = {.ptr = bbc_os_rom, .size = sizeof(bbc_os_rom)},
        },
    };
    if (mos_log_path) {
        mos_log = fopen(mos_log_path, "w");
    }
    if (mos_summary || mos_log) {
        desc.debug.callback.func = mos_debug_cb;
        desc.debug.stopped = &mos_stop;
    }
    if (wav_path) {
        wav = fopen(wav_path, "wb");
        if (wav) fseek(wav, 44, SEEK_SET);
    }
    desc.roms.banks[15] = (chips_range_t){.ptr = bbc_basic_rom, .size = sizeof(bbc_basic_rom)};
    if (dfs) {
        desc.roms.banks[14] = (chips_range_t){.ptr = bbc_dfs_rom, .size = sizeof(bbc_dfs_rom)};
    }
    // Sideways RAM in banks 4-7 (4 x 16 KB)
    static uint8_t swr[4 * 0x4000];
    desc.ram_banks = 0x00F0;
    desc.swr = (chips_range_t){.ptr = swr, .size = sizeof(swr)};
    bbc_init(&bbc, &desc);
    bbc_reset(&bbc);

    static uint8_t disc_data[2 * 80 * 10 * 256];
    size_t disc_size = 0;
    if (disc) {
        FILE* f = fopen(disc, "rb");
        if (!f) {
            perror(disc);
            return 1;
        }
        size_t n = fread(disc_data, 1, sizeof(disc_data), f);
        fclose(f);
        disc_size = n;
        size_t len = strlen(disc);
        int sides = (len > 4 && !strcmp(disc + len - 4, ".dsd")) ? 2 : 1;
        bbc_insert_disc(&bbc, 0, disc_data, n, sides, false);
    }
    if (boot) {
        bbc_key_down(&bbc, BBC_KEY_SHIFT);
    }

    // Key injection state: each character is held for one frame, released the next
    const char* tp = text;
    int pending_key = -1;
    bool pending_shift = false;

    for (int frame = 0; frame < frames; frame++) {
        if (boot && frame == 40) {
            bbc_key_up(&bbc, BBC_KEY_SHIFT);
        }
        // One key event per frame: press on one frame, release on the next
        if (frame >= wait_frames && frame >= pause_until && tp) {
            if (pending_key >= 0) {
                if (++held >= hold_frames) {
                    bbc_key_up(&bbc, (uint8_t)pending_key);
                    if (pending_shift) bbc_key_up(&bbc, BBC_KEY_SHIFT);
                    pending_key = -1;
                    held = 0;
                }
            } else if (*tp == '~') {
                tp++;
                pause_until = frame + 50;
            } else if (*tp) {
                int key = bbc_key_from_ascii((uint8_t)*tp, &pending_shift);
                tp++;
                if (key >= 0) {
                    if (pending_shift) bbc_key_down(&bbc, BBC_KEY_SHIFT);
                    bbc_key_down(&bbc, (uint8_t)key);
                    pending_key = key;
                }
            } else {
                tp = NULL;
            }
        }
        bbc_exec(&bbc, 20000);
    }

    if (disc_out && disc_size) {
        FILE* f = fopen(disc_out, "wb");
        if (f) {
            fwrite(disc_data, 1, disc_size, f);
            fclose(f);
        }
    }
    if (wav) {
        wav_header(wav, wav_samples);
        fclose(wav);
    }
    if (mos_log) fclose(mos_log);
    if (mos_summary) mos_print_summary();
    if (show) print_mode7();
    if (ppm) write_ppm(ppm);
    if (ram) {
        FILE* f = fopen(ram, "wb");
        if (f) {
            fwrite(bbc.ram, 1, sizeof(bbc.ram), f);
            fclose(f);
        }
    }
    printf("frames=%d ticks=%u pc=%04X ula=%02X romsel=%d ic32=%02X crtc=", frames, bbc.system_ticks, bbc.cpu.PC,
           bbc.ula_ctrl, bbc.romsel, bbc.ic32);
    for (int i = 0; i < 16; i++) printf("%02X ", bbc.crtc_reg[i]);
    printf("\n");
    return 0;
}
