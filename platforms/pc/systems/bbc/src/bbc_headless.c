// bbc_headless.c
//
// Headless BBC Micro runner for automated tests: no window, no audio.
//
//   bbc_headless [-f frames] [-t text] [-p out.ppm] [-r ram.bin] [-s]
//
//   -f N      run N frames of 20 ms (default 100)
//   -t TEXT   type TEXT after 1 s (\n = RETURN), one key per 2 frames
//   -p FILE   write the 640x256 framebuffer as a binary PPM
//   -r FILE   write the 32 KB RAM
//   -s        print the 40x25 MODE 7 screen ($7C00) as ASCII
//   -d        disable the DFS ROM (bank 15 empty)
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
#include "systems/bbc.h"
#include "systems/bbc_keys.h"

static bbc_t bbc;

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
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) text = argv[++i];
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) ppm = argv[++i];
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) ram = argv[++i];
        else if (!strcmp(argv[i], "-s")) show = true;
        else if (!strcmp(argv[i], "-d")) dfs = false;
        else {
            fprintf(stderr, "usage: %s [-f frames] [-t text] [-p out.ppm] [-r ram.bin] [-s] [-d]\n", argv[0]);
            return 2;
        }
    }

    bbc_desc_t desc = {
        .roms = {
            .os = {.ptr = bbc_os_rom, .size = sizeof(bbc_os_rom)},
        },
    };
    desc.roms.banks[15] = (chips_range_t){.ptr = bbc_basic_rom, .size = sizeof(bbc_basic_rom)};
    if (dfs) {
        desc.roms.banks[14] = (chips_range_t){.ptr = bbc_dfs_rom, .size = sizeof(bbc_dfs_rom)};
    }
    bbc_init(&bbc, &desc);
    bbc_reset(&bbc);

    // Key injection state: each character is held for one frame, released the next
    const char* tp = text;
    int pending_key = -1;
    bool pending_shift = false;

    for (int frame = 0; frame < frames; frame++) {
        // One key event per frame: press on one frame, release on the next
        if (frame >= 50 && tp) {
            if (pending_key >= 0) {
                bbc_key_up(&bbc, (uint8_t)pending_key);
                if (pending_shift) bbc_key_up(&bbc, BBC_KEY_SHIFT);
                pending_key = -1;
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
