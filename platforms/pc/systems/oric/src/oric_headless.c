// oric_headless.c
//
// Headless Oric Atmos runner for automated tests: no window, no audio.
//
//   oric_headless [-f frames] [-c fdc] [-0 disk.dsk] [-T tape.tap] [-t text] [-w frames] [-s] [-p out.ppm]
//
//   -f N      run N frames of 20 ms (default 100)
//   -c FDC    disk interface: none (default), pravetz, microdisc, jasmin, auto (from the disk in drive 0)
//   -0 FILE   insert FILE (MFM_DISK .dsk) in drive 0 (-1, -2, -3 for the other drives)
//   -W FILE   write the (possibly modified) drive 0 image to FILE at the end
//   -T FILE   insert FILE (.tap) in the tape drive
//   -V FILE   insert FILE (WAVE image made by tools/tap2wave) in the tape drive
//   -t TEXT   type TEXT after -w frames (default 60; \n = RETURN, ~ = pause of 50 frames)
//   -w N      frames before typing
//   -h N      hold each key N frames, then release it N frames (default 3)
//   -s        print the 40x28 text screen ($BB80) as ASCII at the end
//   -p FILE   write the 240x224 framebuffer as a binary PPM
//   -r FILE   write the 64 KB RAM (48 KB, then the 16 KB overlay RAM)
//   -i        print the disk interface state at the end
//   -P        print the bytes sent to the printer at the end
//   -H        print a hash (FNV-1a 32) of the framebuffer at the end
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

#include "roms/oric_roms.h"
#if __has_include("roms/oric_microdisc_rom.h")
#include "roms/oric_microdisc_rom.h"
#define HAVE_MICRODISC_ROM 1
#endif
#if __has_include("roms/oric_jasmin_rom.h")
#include "roms/oric_jasmin_rom.h"
#define HAVE_JASMIN_ROM 1
#endif

// No embedded images in the test runner
static uint8_t* oric_nib_images[1];

#include "chips/chips_common.h"
#include "chips/mos6502cpu.h"
#include "chips/mos6522via.h"
#include "chips/ay38910psg.h"
#include "chips/kbd.h"
#include "chips/mem.h"
#include "chips/clk.h"
#include "devices/oric_td.h"
#include "devices/disk2_fdd.h"
#include "devices/disk2_fdc.h"
#include "devices/oric_fdc_rom.h"
#include "devices/oric_dsk.h"
#include "devices/wd1793.h"
#include "systems/oric.h"

static oric_t oric;

// -P: bytes sent to the printer port, printed on stdout at the end
static char printer_out[4096];
static int printer_len;
static void printer_cb(uint8_t c, void* user_data) {
    (void)user_data;
    if (printer_len < (int)sizeof(printer_out) - 1) printer_out[printer_len++] = (char)c;
}

static uint8_t* load_file(const char* path, uint32_t* size) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "oric_headless: cannot open %s\n", path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* data = malloc(n > 0 ? (size_t)n : 1);
    if (!data || fread(data, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "oric_headless: cannot read %s\n", path);
        exit(2);
    }
    fclose(f);
    *size = (uint32_t)n;
    return data;
}

static void print_screen(void) {
    for (int y = 0; y < 28; y++) {
        char line[41];
        for (int x = 0; x < 40; x++) {
            uint8_t c = oric.ram[0xBB80 + y * 40 + x] & 0x7F;
            line[x] = (c >= 0x20 && c < 0x7F) ? (char)c : ' ';
        }
        int n = 40;
        while (n > 0 && line[n - 1] == ' ') n--;
        line[n] = 0;
        printf("%s\n", line);
    }
}

static void write_ppm(const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", ORIC_SCREEN_WIDTH, ORIC_SCREEN_HEIGHT);
    for (int i = 0; i < ORIC_SCREEN_WIDTH * ORIC_SCREEN_HEIGHT; i++) {
        uint8_t px = oric.fb[i / 2];
        uint8_t c = (i & 1) ? (px & 0x0F) : (px >> 4);
        uint32_t rgb = oric_palette[c & 7];
        uint8_t out[3] = {(uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb};
        fwrite(out, 1, 3, f);
    }
    fclose(f);
}

int main(int argc, char** argv) {
    int frames = 100, wait_frames = 60, hold_frames = 3;
    const char* fdc_name = "none";
    const char* disk_file[4] = {0};
    const char* tape_file = 0;
    const char* wave_file = 0;
    const char* text = 0;
    const char* ppm_file = 0;
    const char* ram_file = 0;
    const char* write_file = 0;
    bool show_screen = false, show_iface = false, show_printer = false, show_hash = false;

    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        const char* v = (i + 1 < argc) ? argv[i + 1] : 0;
        if (!strcmp(a, "-f") && v) { frames = atoi(v); i++; }
        else if (!strcmp(a, "-c") && v) { fdc_name = v; i++; }
        else if (a[0] == '-' && a[1] >= '0' && a[1] <= '3' && !a[2] && v) { disk_file[a[1] - '0'] = v; i++; }
        else if (!strcmp(a, "-W") && v) { write_file = v; i++; }
        else if (!strcmp(a, "-T") && v) { tape_file = v; i++; }
        else if (!strcmp(a, "-V") && v) { wave_file = v; i++; }
        else if (!strcmp(a, "-t") && v) { text = v; i++; }
        else if (!strcmp(a, "-w") && v) { wait_frames = atoi(v); i++; }
        else if (!strcmp(a, "-h") && v) { hold_frames = atoi(v); i++; }
        else if (!strcmp(a, "-s")) { show_screen = true; }
        else if (!strcmp(a, "-i")) { show_iface = true; }
        else if (!strcmp(a, "-P")) { show_printer = true; }
        else if (!strcmp(a, "-H")) { show_hash = true; }
        else if (!strcmp(a, "-p") && v) { ppm_file = v; i++; }
        else if (!strcmp(a, "-r") && v) { ram_file = v; i++; }
        else {
            fprintf(stderr, "usage: see oric_headless.c\n");
            return 2;
        }
    }

    oric_desc_t desc = {
        .td_enabled = true,
        .printer_cb = printer_cb,
        .roms = {
            .rom = {.ptr = oric_rom, .size = sizeof(oric_rom)},
            .boot_rom = {.ptr = oric_fdc_rom, .size = sizeof(oric_fdc_rom)},
#ifdef HAVE_MICRODISC_ROM
            .microdisc_rom = {.ptr = oric_microdisc_rom, .size = sizeof(oric_microdisc_rom)},
#endif
#ifdef HAVE_JASMIN_ROM
            .jasmin_rom = {.ptr = oric_jasmin_rom, .size = sizeof(oric_jasmin_rom)},
#endif
        },
    };
    oric_init(&oric, &desc);

    uint8_t* disk_data[4] = {0};
    uint32_t disk_size[4] = {0};
    for (int d = 0; d < 4; d++) {
        if (!disk_file[d]) continue;
        disk_data[d] = load_file(disk_file[d], &disk_size[d]);
        if (!oric_insert_dsk(&oric, d, disk_data[d], disk_size[d], false)) {
            fprintf(stderr, "oric_headless: %s is not a MFM_DISK image\n", disk_file[d]);
            return 2;
        }
    }
    oric_fdc_type_t fdc = ORIC_FDC_NONE;
    if (!strcmp(fdc_name, "pravetz")) fdc = ORIC_FDC_PRAVETZ;
    else if (!strcmp(fdc_name, "microdisc")) fdc = ORIC_FDC_MICRODISC;
    else if (!strcmp(fdc_name, "jasmin")) fdc = ORIC_FDC_JASMIN;
    else if (!strcmp(fdc_name, "auto") && disk_data[0]) fdc = oric_dsk_interface(&oric, &oric.wd.disk[0]);
    if (fdc != ORIC_FDC_NONE && !oric_set_fdc(&oric, fdc)) {
        fprintf(stderr, "oric_headless: interface %s unavailable (ROM missing)\n", fdc_name);
        return 3;
    }
    oric_reset(&oric);

    uint32_t tap_size = 0;
    uint8_t* tap = 0;
    if (tape_file) {
        tap = load_file(tape_file, &tap_size);
        oric_insert_tap(&oric, tap, tap_size);
    }
    uint32_t wave_size = 0;
    uint8_t* wave = 0;
    if (wave_file) {
        wave = load_file(wave_file, &wave_size);
        oric_td_insert_tape(&oric.td, wave);
    }

    // Keys to type (Oric key codes: letters unshifted are capitals)
    char keys[512];
    int nkeys = 0;
    for (const char* p = text; p && *p && nkeys < (int)sizeof(keys); p++) {
        if (p[0] == '\\' && p[1] == 'n') { keys[nkeys++] = 0x0D; p++; }
        else keys[nkeys++] = *p;
    }

    int k = 0, held = 0, timer = 0;
    for (int f = 0; f < frames; f++) {
        if (timer > 0) {
            timer--;
        } else if (held) {
            kbd_key_up(&oric.kbd, held);
            held = 0;
            timer = hold_frames;
        } else if (f >= wait_frames && k < nkeys) {
            if (keys[k] == '~') {
                k++;
                timer = 50;   // Pause
            } else {
                held = (uint8_t)keys[k++];
                kbd_key_down(&oric.kbd, held);
                timer = hold_frames;
            }
        }
        oric_exec(&oric, 20000);
    }
    oric.screen_dirty = true;
    oric_screen_update(&oric);

    if (show_screen) print_screen();
    if (show_iface) {
        static const char* names[] = {"none", "pravetz", "microdisc", "jasmin"};
        printf("pc=%04X md_ctrl=%02X irq=%d iflag=%d fdc=%s romdis=%d diskrom=%d olay=%d jasmin_rom=%d track=%d head0=%d status=%02X tape_pos=%u via_ier=%02X via_ifr=%02X acr=%02X t1_latch=%04X\n",
               oric.cpu.PC, oric.md_ctrl, oric.cpu.irq, oric.cpu.iflag, names[oric.fdc_type], oric.romdis, oric.diskrom, oric.jasmin_olay, oric.jasmin_rom_on, oric.wd.track,
               oric.wd.head[0], oric.wd.status, (unsigned)oric.td.tap_pos, oric.via.intr.ier, oric.via.intr.ifr, oric.via.acr,
               oric.via.t1.latch);
    }
    if (show_printer) {
        printf("printer: %d byte(s): ", printer_len);
        for (int i = 0; i < printer_len; i++) {
            char c = printer_out[i];
            putchar((c >= 32 && c < 127) ? c : '.');
        }
        putchar('\n');
    }
    if (show_hash) {
        uint32_t h = 2166136261u;
        for (size_t i = 0; i < sizeof(oric.fb); i++) h = (h ^ oric.fb[i]) * 16777619u;
        printf("fb_hash=%08X\n", h);
    }
    if (ppm_file) write_ppm(ppm_file);
    if (ram_file) {
        FILE* f = fopen(ram_file, "wb");
        if (f) {
            fwrite(oric.ram, 1, sizeof(oric.ram), f);
            fwrite(oric.overlay_ram, 1, sizeof(oric.overlay_ram), f);
            fclose(f);
        }
    }
    wd1793_flush(&oric.wd);
    if (write_file && disk_data[0]) {
        FILE* f = fopen(write_file, "wb");
        if (!f || fwrite(disk_data[0], 1, disk_size[0], f) != disk_size[0]) {
            fprintf(stderr, "oric_headless: cannot write %s\n", write_file);
            return 2;
        }
        fclose(f);
    }
    return 0;
}
