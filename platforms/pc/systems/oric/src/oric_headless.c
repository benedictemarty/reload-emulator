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
//   -F        fast tape loading (BASIC 1.1 patched, devices/oric_tape_turbo.h)
//   -R DIR    CSAVE: write the recorded files as DIR/NAME.TAP (devices/oric_tape_rec.h)
//   -N HOST[:PORT]  files over the network: -0..-3 and -T are paths on the TNFS
//             server HOST (devices/neo_tnfs.h), disks read and written as streams
//             (as on the board), not loaded in memory
//   -S TTY    TNFS through an ESP8266 AT Wi-Fi modem on the serial port TTY
//             (Neo6502picowifi on /dev/ttyACM0, or neo_esp_modem_pc) instead of
//             UDP from the PC; needs -N SERVER (AT+CIPSTART="UDP"); Wi-Fi network
//             from the environment (NEO_WIFI_SSID, NEO_WIFI_PASS) or the modem's own
//   -D TTY    with -S and -N: TNFS through the modem's second serial port TTY
//             (Neo6502picowifi /dev/ttyACM1: one length-prefixed datagram per frame),
//             the server given by AT$TNFS on the AT port, instead of AT UDP
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
#ifdef ORIC_NMOS
#include "chips/mos6502cpu.h"   // NMOS 6502 of the original Oric (cmake -DORIC_PC_NMOS=ON)
#else
#include "chips/w65c02cpu.h"    // W65C02, as on the Neo6502 board (default)
#endif
#ifdef ORIC_VIA6522
#include "chips/via6522.h"   // Trial of the common VIA (Neo6502Vic20)
#else
#include "chips/mos6522via.h"
#endif
#include "chips/ay38910psg.h"
#include "chips/kbd.h"
#include "chips/mem.h"
#include "chips/clk.h"
#include "devices/oric_tape.h"
#include "devices/oric_tape_rec.h"
#include "devices/oric_tape_turbo.h"
#include "devices/disk2_fdd.h"
#include "devices/disk2_fdc.h"
#include "devices/oric_fdc_rom.h"
#include "devices/oric_dsk.h"
#include "devices/wd1793.h"
#include "systems/oric.h"
#define NEO_STORAGE_IMPL
#include "devices/neo_storage.h"
#include "../../../neo_storage_posix.h"
#define NEO_TNFS_IMPL
#include "devices/neo_tnfs.h"
#include "../../../neo_tnfs_udp.h"
#define NEO_ESP_AT_IMPL
#include "devices/neo_esp_at.h"
#define NEO_DGRAM_SERIAL_IMPL
#include "devices/neo_dgram_serial.h"
#include "../../../neo_serial_posix.h"

// Files through neo_storage, as on the board: volume 0 = paths as given (-T),
// volume 1 = the CSAVE directory (-R)
#define VOL_FILES 0
#define VOL_REC   1

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

// CSAVE (-R DIR): one DIR/NAME.TAP file per recording
static neo_file_t rec_file;
static bool rec_open(void* ctx, const char* name) {
    (void)ctx;
    neo_file_close(&rec_file);
    if (!neo_file_open(&rec_file, VOL_REC, name, NEO_WRITE | NEO_CREATE)) return false;
    printf("tape: recording %s\n", name);
    return true;
}
static void rec_write(void* ctx, const uint8_t* data, uint32_t len) {
    (void)ctx;
    if (neo_file_is_open(&rec_file)) neo_file_append(&rec_file, data, len);
}
static void rec_close(void* ctx) {
    (void)ctx;
    neo_file_close(&rec_file);
}
static const oric_tape_rec_out_t rec_out = {rec_open, rec_write, rec_close, 0};

int main(int argc, char** argv) {
    int frames = 100, wait_frames = 60, hold_frames = 3;
    const char* fdc_name = "none";
    const char* disk_file[4] = {0};
    const char* tape_file = 0;
    const char* rec_dir = 0;
    const char* tnfs_host = 0;
    const char* serial_tty = 0;
    const char* dgram_tty = 0;
    bool fast = false;
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
        else if (!strcmp(a, "-R") && v) { rec_dir = v; i++; }
        else if (!strcmp(a, "-N") && v) { tnfs_host = v; i++; }
        else if (!strcmp(a, "-S") && v) { serial_tty = v; i++; }
        else if (!strcmp(a, "-D") && v) { dgram_tty = v; i++; }
        else if (!strcmp(a, "-F")) fast = true;
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

    // Volume of the files: the local file system, or a TNFS server (-N)
    static neo_tnfs_t tnfs;
    static neo_tnfs_udp_t udp;
    static neo_esp_t esp;
    static neo_dgram_t dgram;
    int serial_fd = -1, dgram_fd = -1;
    if (serial_tty) {
        // Wi-Fi modem on a serial port (Neo6502picowifi, or neo_esp_modem_pc)
        if (!tnfs_host) {
            fprintf(stderr, "oric_headless: -S needs -N SERVER\n");
            return 2;
        }
        serial_fd = neo_serial_open(serial_tty);
        if (serial_fd < 0) {
            fprintf(stderr, "oric_headless: cannot open %s\n", serial_tty);
            return 2;
        }
        neo_esp_init(&esp, neo_serial_write, neo_serial_read_byte, (void*)(intptr_t)serial_fd);
        char host[256];
        snprintf(host, sizeof(host), "%s", tnfs_host);
        char* colon = strchr(host, ':');
        const uint16_t port = colon ? (uint16_t)atoi(colon + 1) : NEO_TNFS_PORT;
        if (colon) *colon = 0;
        if (dgram_tty) {
            // Second serial port of the modem: server set by AT$TNFS, datagrams in frames
            dgram_fd = neo_serial_open(dgram_tty);
            if (dgram_fd < 0 || !neo_esp_set_tnfs(&esp, host, port, getenv("NEO_WIFI_SSID"), getenv("NEO_WIFI_PASS"))) {
                fprintf(stderr, "oric_headless: TNFS port %s: modem not configured for %s\n", dgram_tty, tnfs_host);
                return 2;
            }
            neo_dgram_init(&dgram, neo_serial_write, neo_serial_read_byte, (void*)(intptr_t)dgram_fd);
            neo_tnfs_init(&tnfs, neo_dgram_xfer, &dgram);
        } else {
            if (!neo_esp_open_udp(&esp, host, port, getenv("NEO_WIFI_SSID"), getenv("NEO_WIFI_PASS"))) {
                fprintf(stderr, "oric_headless: Wi-Fi modem on %s: no UDP link to %s\n", serial_tty, tnfs_host);
                return 2;
            }
            neo_tnfs_init(&tnfs, neo_esp_xfer, &esp);
        }
        tnfs_host = dgram_tty ? dgram_tty : serial_tty;   // The files are on the network from here on
        tnfs.sleep = neo_tnfs_udp_sleep;
    } else if (tnfs_host) {
        char host[256];
        snprintf(host, sizeof(host), "%s", tnfs_host);
        char* colon = strchr(host, ':');
        const uint16_t port = colon ? (uint16_t)atoi(colon + 1) : NEO_TNFS_PORT;
        if (colon) *colon = 0;
        if (!neo_tnfs_udp_open(&udp, host, port)) {
            fprintf(stderr, "oric_headless: TNFS server %s unknown\n", tnfs_host);
            return 2;
        }
        neo_tnfs_init(&tnfs, neo_tnfs_udp_xfer, &udp);
        tnfs.sleep = neo_tnfs_udp_sleep;
    }
    if (tnfs_host) {
        if (!neo_tnfs_mount(&tnfs, "/", 0, 0)) {
            fprintf(stderr, "oric_headless: TNFS mount on %s failed (error %02X)\n", tnfs_host, tnfs.error);
            return 2;
        }
        printf("tnfs: mounted %s (server version %d.%d)\n", tnfs_host, tnfs.version >> 8, tnfs.version & 0xFF);
        neo_storage_set(VOL_FILES, "Réseau", &neo_storage_tnfs_ops, &tnfs);
    } else {
        neo_storage_set(VOL_FILES, "Fichiers", &neo_storage_posix_ops, "");
    }

    uint8_t* disk_data[4] = {0};
    uint32_t disk_size[4] = {0};
    static neo_file_t disk_stream[4];
    for (int d = 0; d < 4; d++) {
        if (!disk_file[d]) continue;
        if (tnfs_host) {
            // Streamed, as on the board: tracks read and written through the volume
            neo_file_t* f = &disk_stream[d];
            bool ro = false;
            if (!neo_file_open(f, VOL_FILES, disk_file[d], NEO_READ | NEO_WRITE)) {
                ro = neo_file_open(f, VOL_FILES, disk_file[d], NEO_READ);
            }
            uint8_t header[ORIC_DSK_HEADER_SIZE];
            if (!neo_file_is_open(f) || !neo_file_read(f, 0, header, sizeof(header)) ||
                !wd1793_insert_streamed(&oric.wd, d, header, f->size, neo_file_read_cb, ro ? 0 : neo_file_write_cb, f)) {
                fprintf(stderr, "oric_headless: %s: cannot read a MFM_DISK image over TNFS\n", disk_file[d]);
                return 2;
            }
            continue;
        }
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
    else if (!strcmp(fdc_name, "auto") && disk_file[0]) fdc = oric_dsk_interface(&oric, &oric.wd.disk[0]);
    if (fdc != ORIC_FDC_NONE && !oric_set_fdc(&oric, fdc)) {
        fprintf(stderr, "oric_headless: interface %s unavailable (ROM missing)\n", fdc_name);
        return 3;
    }
    oric_reset(&oric);

    // Tape read as a stream, as on the board (neo_file_read_cb)
    static neo_file_t tap_file;
    if (tape_file) {
        if (!neo_file_open(&tap_file, VOL_FILES, tape_file, NEO_READ) ||
            !oric_insert_tape(&oric, tap_file.size, neo_file_read_cb, &tap_file)) {
            fprintf(stderr, "oric_headless: cannot read %s\n", tape_file);
            return 2;
        }
    }
    if (fast && !oric_tape_fast(&oric, true)) {
        fprintf(stderr, "oric_headless: fast tape loading needs the BASIC 1.1 ROM\n");
    }
    if (rec_dir) {
        neo_storage_set(VOL_REC, "CSAVE", &neo_storage_posix_ops, (void*)rec_dir);
        oric_tape_recorder(&oric, &rec_out);
    }

    // Keys to type (Oric key codes: letters unshifted are capitals)
    char keys[512];
    int nkeys = 0;
    for (const char* p = text; p && *p && nkeys < (int)sizeof(keys); p++) {
        if (p[0] == '\\' && p[1] == 'n') { keys[nkeys++] = 0x0D; p++; }
        else keys[nkeys++] = *p;
    }

    // A shifted character: SHIFT alone one frame before the key, released with it
    const uint32_t shift_bit = 1u << (KBD_MAX_COLUMNS + KBD_MAX_LINES);
    int k = 0, held = 0, timer = 0;
    bool shift_held = false;
    for (int f = 0; f < frames; f++) {
        if (timer > 0) {
            timer--;
        } else if (held) {
            kbd_key_up(&oric.kbd, held);
            if (shift_held) kbd_key_up(&oric.kbd, ORIC_KEY_SHIFT);
            held = 0;
            shift_held = false;
            timer = hold_frames;
        } else if (f >= wait_frames && k < nkeys) {
            if (keys[k] == '~') {
                k++;
                timer = 50;   // Pause
            } else if ((oric.kbd.key_masks[(uint8_t)keys[k]] & shift_bit) && !shift_held) {
                kbd_key_down(&oric.kbd, ORIC_KEY_SHIFT);
                shift_held = true;
                timer = 1;
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
               oric.wd.head[0], oric.wd.status, (unsigned)oric.tape.pos,
#ifdef ORIC_VIA6522
               oric.via.ier, oric.via.ifr, oric.via.acr, oric.via.t1_latch);
#else
               oric.via.intr.ier, oric.via.intr.ifr, oric.via.acr, oric.via.t1.latch);
#endif
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
    if (tnfs_host) {
        for (int d = 0; d < 4; d++) {
            if (neo_file_is_open(&disk_stream[d])) {
                wd1793_eject(&oric.wd, d);   // Cached track written back first
                neo_file_close(&disk_stream[d]);
            }
        }
        neo_file_close(&tap_file);
        printf("tnfs: %u request(s), %u resent\n", (unsigned)tnfs.sent, (unsigned)tnfs.resent);
        neo_tnfs_umount(&tnfs);
        if (serial_fd >= 0) {
            if (dgram_fd >= 0) {
                printf("tnfs port: %u frame(s) received, %u stale, %u resync\n", (unsigned)dgram.received,
                       (unsigned)dgram.stale, (unsigned)dgram.resync);
                close(dgram_fd);
            } else {
                printf("modem: %u datagram(s) received, %u dropped, %u send failure(s)\n", (unsigned)esp.datagrams,
                       (unsigned)esp.dropped, (unsigned)esp.send_fail);
            }
            close(serial_fd);
        } else {
            neo_tnfs_udp_close(&udp);
        }
    }
    return 0;
}
