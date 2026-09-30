// test_oric_devices.c
//
// Unit tests of the Oric disk and tape devices, without any ROM:
//   - devices/oric_dsk.h: MFM_DISK header, track layout, sector scan, CRC;
//   - devices/wd1793.h: write track (format), read/write sector, read address,
//     type I commands and status, multiple sectors, errors, streamed images;
//   - devices/oric_td.h: .tap signal (decoded back into bytes), leader and
//     pause, motor rules, padding, streamed reading;
//   - chips/mos6522via.h: IRQ released when the IER disables its source;
//   - systems/oric.h: Microdisc and Jasmin registers and memory maps,
//     interface detection of a MFM_DISK image, printer acknowledge.
//
// Build: see platforms/pc/systems/oric/CMakeLists.txt (target oric_devices_test, ctest).
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

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Test ROMs (no real ROM needed): 16 KB of NOP with the reset vector at $C000
static uint8_t oric_rom[0x4000];
static uint8_t test_microdisc_rom[0x2000];
static uint8_t test_jasmin_rom[0x800];
static uint8_t test_boot_rom[0x200];
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
#include "devices/oric_dsk.h"
#include "devices/wd1793.h"
#include "systems/oric.h"

static int tests_run, tests_failed;

#define CHECK(cond)                                                             \
    do {                                                                        \
        tests_run++;                                                            \
        if (!(cond)) {                                                          \
            tests_failed++;                                                     \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
        }                                                                       \
    } while (0)

/*-- WD1793 driven like a CPU -------------------------------------------------*/

#define TRACKS 3
#define SIDES  2
#define SECTORS 17
#define IMG_SIZE (ORIC_DSK_HEADER_SIZE + TRACKS * SIDES * ORIC_DSK_TRACK_SIZE)

static wd1793_t fdc;

// Run the controller until it is idle (or timeout); returns false on timeout
static bool run_until_idle(void) {
    for (int i = 0; i < 1000000; i++) {
        if (fdc.state == WD1793_IDLE) return true;
        wd1793_tick(&fdc);
    }
    return false;
}

static bool wait_drq_or_idle(void) {
    for (int i = 0; i < 100000; i++) {
        if (fdc.drq || fdc.state == WD1793_IDLE) return true;
        wd1793_tick(&fdc);
    }
    return false;
}

static uint8_t command(uint8_t cmd) {
    wd1793_write(&fdc, 0, cmd);
    run_until_idle();
    return wd1793_read(&fdc, 0);
}

// Physical interleave of the test format: sector numbers in track order
static int phys_sector(int i) { return (i * 5) % SECTORS + 1; }

static uint8_t data_byte(int track, int side, int sector, int i) {
    return (uint8_t)(track * 31 + side * 97 + sector * 7 + i);
}

// Format the current track with the write track command, as Sedoric does
static bool format_track(int track, int side) {
    uint8_t buf[ORIC_DSK_TRACK_SIZE];
    int n = 0;
#define PUT(v, count)                                        \
    for (int k = 0; k < (count); k++) buf[n++] = (uint8_t)(v);
    PUT(0x4E, 40);
    for (int i = 0; i < SECTORS; i++) {
        int s = phys_sector(i);
        PUT(0x00, 12);
        PUT(0xF5, 3);
        PUT(0xFE, 1);
        PUT(track, 1);
        PUT(side, 1);
        PUT(s, 1);
        PUT(0x01, 1);
        PUT(0xF7, 1);   // 2 CRC bytes
        PUT(0x4E, 22);
        PUT(0x00, 12);
        PUT(0xF5, 3);
        PUT(0xFB, 1);
        for (int k = 0; k < 256; k++) {
            uint8_t v = data_byte(track, side, s, k);
            if (v >= 0xF5 && v <= 0xF7) v = 0x55;   // Control bytes cannot be written as data
            buf[n++] = v;
        }
        PUT(0xF7, 1);
        PUT(0x4E, 24);
    }
#undef PUT
    // The F7 bytes write 2 bytes: fill the rest of the track with gap bytes
    wd1793_write(&fdc, 0, 0xF4);
    int written = 0, i = 0;
    for (int guard = 0; guard < 10000000 && fdc.state != WD1793_IDLE; guard++) {
        if (fdc.drq) {
            uint8_t v = i < n ? buf[i++] : 0x4E;
            written += (v == 0xF7) ? 2 : 1;
            wd1793_write(&fdc, 3, v);
        }
        wd1793_tick(&fdc);
    }
    (void)written;
    return fdc.state == WD1793_IDLE && !(fdc.status & (WD1793_ST_WPROT | WD1793_ST_SEEK_RNF));
}

static uint8_t expected(int track, int side, int s, int k) {
    uint8_t v = data_byte(track, side, s, k);
    return (v >= 0xF5 && v <= 0xF7) ? 0x55 : v;
}

// Read one sector into buf; returns the final status
static uint8_t read_sector(int track, int side, int sector, uint8_t* buf, int* count) {
    (void)side;
    wd1793_write(&fdc, 1, (uint8_t)track);
    wd1793_write(&fdc, 2, (uint8_t)sector);
    wd1793_write(&fdc, 0, 0x80);
    *count = 0;
    while (wait_drq_or_idle() && fdc.state != WD1793_IDLE) {
        if (fdc.drq) {
            uint8_t v = wd1793_read(&fdc, 3);
            if (*count < 256) buf[*count] = v;
            (*count)++;
        } else {
            wd1793_tick(&fdc);
        }
    }
    return wd1793_read(&fdc, 0);
}

static uint8_t* make_blank_image(int tracks, int sides) {
    uint32_t size = ORIC_DSK_HEADER_SIZE + (uint32_t)(tracks * sides) * ORIC_DSK_TRACK_SIZE;
    uint8_t* img = calloc(1, size);
    memcpy(img, "MFM_DISK", 8);
    img[8] = (uint8_t)sides;
    img[12] = (uint8_t)tracks;
    img[16] = 1;
    for (uint32_t i = ORIC_DSK_HEADER_SIZE; i < size; i++) img[i] = 0x4E;
    return img;
}

static void seek(int track) {
    wd1793_write(&fdc, 3, (uint8_t)track);
    command(0x10);
}

static void format_image(void) {
    for (int side = 0; side < SIDES; side++) {
        wd1793_select(&fdc, 0, side);
        for (int t = 0; t < TRACKS; t++) {
            seek(t);
            CHECK(format_track(t, side));
        }
    }
    wd1793_select(&fdc, 0, 0);
    command(0x00);
}

static void test_dsk_header(void) {
    uint8_t h[ORIC_DSK_HEADER_SIZE] = {0};
    int s = 0, t = 0;
    CHECK(!oric_dsk_parse_header(h, sizeof(h), &s, &t));
    memcpy(h, "MFM_DISK", 8);
    h[8] = 2;
    h[12] = 42;
    h[16] = 1;
    CHECK(oric_dsk_parse_header(h, sizeof(h), &s, &t) && s == 2 && t == 42);
    CHECK(!oric_dsk_parse_header(h, 100, &s, &t));   // Too short
    h[16] = 2;
    CHECK(!oric_dsk_parse_header(h, sizeof(h), &s, &t));   // Geometry 2 not supported
    h[16] = 1;
    h[8] = 3;
    CHECK(!oric_dsk_parse_header(h, sizeof(h), &s, &t));
    // Track layout: side 0 tracks first
    oric_dsk_t d = {.tracks = 42, .sides = 2};
    CHECK(oric_dsk_track_offset(&d, 0, 0) == 256);
    CHECK(oric_dsk_track_offset(&d, 1, 0) == 256 + 6400);
    CHECK(oric_dsk_track_offset(&d, 0, 1) == 256 + 42 * 6400);
    // CRC-16/CCITT of A1 A1 A1 from FFFF (value used by the WD179x)
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < 3; i++) crc = oric_dsk_crc(crc, 0xA1);
    CHECK(crc == 0xCDB4);
}

static uint8_t* image;

static void test_format_and_read(void) {
    image = make_blank_image(TRACKS, SIDES);
    wd1793_init(&fdc);
    CHECK(wd1793_insert_mem(&fdc, 0, image, IMG_SIZE, false));
    format_image();
    CHECK(fdc.disk[0].modified);

    // Every sector reads back with its data
    uint8_t buf[256];
    int errors = 0;
    for (int side = 0; side < SIDES; side++) {
        wd1793_select(&fdc, 0, side);
        for (int t = 0; t < TRACKS; t++) {
            seek(t);
            for (int s = 1; s <= SECTORS; s++) {
                int count;
                uint8_t st = read_sector(t, side, s, buf, &count);
                if (st != 0 || count != 256) errors++;
                for (int k = 0; k < 256; k++) {
                    if (buf[k] != expected(t, side, s, k)) {
                        errors++;
                        break;
                    }
                }
            }
        }
    }
    CHECK(errors == 0);

    // The formatted track holds valid ID and data CRCs
    wd1793_select(&fdc, 0, 1);
    seek(2);
    CHECK(oric_dsk_load_track(&fdc.cache, &fdc.disk[0], 2, 1));
    CHECK(fdc.cache.num_sectors == SECTORS);
    int crc_errors = 0;
    for (int i = 0; i < fdc.cache.num_sectors; i++) {
        const uint8_t* p = fdc.cache.raw + fdc.cache.sectors[i].id;
        uint16_t crc = 0xFFFF;
        for (int k = 0; k < 3; k++) crc = oric_dsk_crc(crc, 0xA1);
        for (int k = 0; k < 5; k++) crc = oric_dsk_crc(crc, p[k]);
        if (p[5] != (crc >> 8) || p[6] != (crc & 0xFF)) crc_errors++;
        const uint8_t* dp = fdc.cache.raw + fdc.cache.sectors[i].data;
        crc = 0xFFFF;
        for (int k = 0; k < 3; k++) crc = oric_dsk_crc(crc, 0xA1);
        for (int k = 0; k < 257; k++) crc = oric_dsk_crc(crc, dp[k]);
        if (dp[257] != (crc >> 8) || dp[258] != (crc & 0xFF)) crc_errors++;
    }
    CHECK(crc_errors == 0);
    // Sector order on the track follows the interleave
    CHECK(fdc.cache.raw[fdc.cache.sectors[1].id + 3] == phys_sector(1));
    wd1793_select(&fdc, 0, 0);
}

static void test_type1(void) {
    uint8_t st = command(0x00);   // Restore
    CHECK(fdc.track == 0 && fdc.head[0] == 0);
    CHECK(st & WD1793_ST_TRACK0_LOST);
    CHECK(!(st & WD1793_ST_BUSY));
    CHECK(fdc.intrq == false);   // Cleared by the status read
    wd1793_write(&fdc, 3, 2);
    st = command(0x14);          // Seek with verify
    CHECK(fdc.track == 2 && fdc.head[0] == 2 && !(st & WD1793_ST_SEEK_RNF) && !(st & WD1793_ST_TRACK0_LOST));
    st = command(0x54);          // Step in with update and verify: track 3 is not on the disk
    CHECK(fdc.track == 3 && fdc.head[0] == 3 && (st & WD1793_ST_SEEK_RNF));
    st = command(0x70);          // Step out with update, no verify
    CHECK(fdc.track == 2 && !(st & WD1793_ST_SEEK_RNF));
    st = command(0x20);          // Step (last direction: out), no update
    CHECK(fdc.track == 2 && fdc.head[0] == 1);
    command(0x00);
    // INTRQ raised at the end of a command, cleared by a status read
    wd1793_write(&fdc, 0, 0x00);
    CHECK(fdc.status & WD1793_ST_BUSY);
    run_until_idle();
    CHECK(fdc.intrq);
    wd1793_read(&fdc, 0);
    CHECK(!fdc.intrq);
}

static void test_read_errors(void) {
    uint8_t buf[256];
    int count;
    seek(1);
    // Sector 18 does not exist: record not found
    uint8_t st = read_sector(1, 0, 18, buf, &count);
    CHECK((st & WD1793_ST_SEEK_RNF) && count == 0);
    // Track register not matching the ID fields: record not found
    st = read_sector(0, 0, 1, buf, &count);
    CHECK(st & WD1793_ST_SEEK_RNF);
    // Side compare (C flag): side 1 wanted on side 0
    wd1793_write(&fdc, 1, 1);
    wd1793_write(&fdc, 2, 3);
    st = command(0x8A);
    CHECK(st & WD1793_ST_SEEK_RNF);
    wd1793_write(&fdc, 2, 3);
    wd1793_write(&fdc, 0, 0x82);   // Side compare with side 0: found
    CHECK(wait_drq_or_idle() && fdc.drq);
    wd1793_write(&fdc, 0, 0xD0);   // Force interrupt
    CHECK(fdc.state == WD1793_IDLE && fdc.intrq && !fdc.drq && !(fdc.status & WD1793_ST_BUSY));
    // Busy: "not ready" set during a type II command (FT-DOS relies on it)
    wd1793_write(&fdc, 2, 1);
    wd1793_write(&fdc, 0, 0x80);
    CHECK((wd1793_read(&fdc, 0) & 0x81) == 0x81);
    wd1793_write(&fdc, 0, 0xD0);
    command(0x00);
}

static void test_multiple_and_address(void) {
    seek(2);
    // Read sectors 15 to 17 with one command: stops quietly at the end of the track
    wd1793_write(&fdc, 2, 15);
    wd1793_write(&fdc, 0, 0x90);
    int count = 0, bad = 0;
    while (wait_drq_or_idle() && fdc.state != WD1793_IDLE) {
        if (fdc.drq) {
            int s = 15 + count / 256;
            if (wd1793_read(&fdc, 3) != expected(2, 0, s, count % 256)) bad++;
            count++;
        } else {
            wd1793_tick(&fdc);
        }
    }
    CHECK(count == 3 * 256 && bad == 0);
    CHECK(wd1793_read(&fdc, 0) == 0);
    // Read address: 6 bytes of the next ID field, track copied to the sector register
    wd1793_write(&fdc, 0, 0xC0);
    uint8_t id[6];
    int n = 0;
    while (wait_drq_or_idle() && fdc.state != WD1793_IDLE) {
        if (fdc.drq) {
            if (n < 6) id[n] = wd1793_read(&fdc, 3);
            n++;
        } else {
            wd1793_tick(&fdc);
        }
    }
    CHECK(n == 6 && id[0] == 2 && id[1] == 0 && id[3] == 1 && id[2] >= 1 && id[2] <= SECTORS);
    CHECK(fdc.sector == 2);
    command(0x00);
}

static void test_write_sector(void) {
    seek(1);
    wd1793_write(&fdc, 1, 1);
    wd1793_write(&fdc, 2, 9);
    wd1793_write(&fdc, 0, 0xA0);
    int n = 0;
    while (wait_drq_or_idle() && fdc.state != WD1793_IDLE) {
        if (fdc.drq) {
            wd1793_write(&fdc, 3, (uint8_t)(255 - n));
            n++;
        } else {
            wd1793_tick(&fdc);
        }
    }
    CHECK(n == 256 && wd1793_read(&fdc, 0) == 0);
    uint8_t buf[256];
    int count;
    CHECK(read_sector(1, 0, 9, buf, &count) == 0 && count == 256);
    int bad = 0;
    for (int k = 0; k < 256; k++) {
        if (buf[k] != (uint8_t)(255 - k)) bad++;
    }
    CHECK(bad == 0);
    // Neighbour sectors unchanged
    CHECK(read_sector(1, 0, 8, buf, &count) == 0 && buf[0] == expected(1, 0, 8, 0) && buf[255] == expected(1, 0, 8, 255));
    // Write protected disk
    fdc.disk[0].write_protected = true;
    wd1793_write(&fdc, 2, 9);
    uint8_t st = command(0xA0);
    CHECK(st & WD1793_ST_WPROT);
    fdc.disk[0].write_protected = false;
    // Deleted data mark (a0 = 1): record type bit when read back
    wd1793_write(&fdc, 2, 10);
    wd1793_write(&fdc, 0, 0xA1);
    while (wait_drq_or_idle() && fdc.state != WD1793_IDLE) {
        if (fdc.drq) wd1793_write(&fdc, 3, 0); else wd1793_tick(&fdc);
    }
    wd1793_read(&fdc, 0);
    CHECK(read_sector(1, 0, 10, buf, &count) & WD1793_ST_HEAD_REC);
    command(0x00);
}

static void test_no_disk(void) {
    wd1793_select(&fdc, 1, 0);   // Drive 1: empty
    uint8_t st = command(0x00);
    CHECK((st & WD1793_ST_NOT_READY) && (st & WD1793_ST_SEEK_RNF));
    uint8_t buf[256];
    int count;
    st = read_sector(0, 0, 1, buf, &count);
    CHECK((st & WD1793_ST_NOT_READY) && (st & WD1793_ST_SEEK_RNF) && count == 0);
    wd1793_select(&fdc, 0, 0);
}

// Streamed access (file on a USB drive): tracks read on demand, written back on flush
static uint8_t* stream_img;
static int stream_reads, stream_writes;
static bool s_read(void* ctx, uint32_t off, uint8_t* buf, uint32_t len) {
    (void)ctx;
    stream_reads++;
    memcpy(buf, stream_img + off, len);
    return true;
}
static bool s_write(void* ctx, uint32_t off, const uint8_t* buf, uint32_t len) {
    (void)ctx;
    stream_writes++;
    memcpy(stream_img + off, buf, len);
    return true;
}

static void test_streamed(void) {
    stream_img = malloc(IMG_SIZE);
    memcpy(stream_img, image, IMG_SIZE);
    wd1793_init(&fdc);
    CHECK(wd1793_insert_streamed(&fdc, 0, stream_img, IMG_SIZE, s_read, s_write, 0));
    command(0x00);
    uint8_t buf[256];
    int count;
    CHECK(read_sector(0, 0, 5, buf, &count) == 0 && count == 256 && buf[7] == expected(0, 0, 5, 7));
    CHECK(read_sector(0, 0, 6, buf, &count) == 0 && stream_reads == 1);   // Same track: cached
    // Write a sector: written back when another track is loaded
    wd1793_write(&fdc, 2, 2);
    wd1793_write(&fdc, 0, 0xA0);
    while (wait_drq_or_idle() && fdc.state != WD1793_IDLE) {
        if (fdc.drq) wd1793_write(&fdc, 3, 0x3C); else wd1793_tick(&fdc);
    }
    wd1793_read(&fdc, 0);
    CHECK(stream_writes == 0);
    seek(1);
    CHECK(read_sector(1, 0, 1, buf, &count) == 0);
    CHECK(stream_writes == 1);
    oric_dsk_track_t t = {0};
    oric_dsk_t d;
    CHECK(oric_dsk_open_mem(&d, stream_img, IMG_SIZE, true));
    CHECK(oric_dsk_load_track(&t, &d, 0, 0));
    int found = 0;
    for (int i = 0; i < t.num_sectors; i++) {
        if (t.raw[t.sectors[i].id + 3] == 2) {
            found = t.raw[t.sectors[i].data + 1] == 0x3C && t.raw[t.sectors[i].data + 256] == 0x3C;
        }
    }
    CHECK(found);
    // Streamed image without write callback: write protected
    wd1793_init(&fdc);
    CHECK(wd1793_insert_streamed(&fdc, 0, stream_img, IMG_SIZE, s_read, 0, 0));
    CHECK(fdc.disk[0].write_protected);
    // Truncated image: missing tracks are unformatted
    wd1793_init(&fdc);
    CHECK(wd1793_insert_mem(&fdc, 0, image, IMG_SIZE - ORIC_DSK_TRACK_SIZE, false));
    wd1793_select(&fdc, 0, 1);
    seek(2);
    CHECK(read_sector(2, 1, 1, buf, &count) & WD1793_ST_SEEK_RNF);
    free(stream_img);
}

/*-- Tape ---------------------------------------------------------------------*/

// Receiver: measures the time between rising edges (1 unit = bit 1, 2 units = bit 0)
// and rebuilds the bytes from 14-bit frames: 1, 0, 8 data bits, parity, 1, 1, 1
typedef struct {
    int last_level, units_since_rise;
    uint32_t shift;   // Last bits received (bit 0 = most recent)
    int nbits;
    uint8_t bytes[20000];
    int nbytes, parity_errors;
    int pause_seen;   // Runs of more than 3 ones seen
    int ones_run;
} rx_t;

static void rx_bit(rx_t* rx, int bit) {
    rx->shift = (rx->shift << 1) | (uint32_t)bit;
    rx->nbits++;
    rx->ones_run = bit ? rx->ones_run + 1 : 0;
    if (rx->ones_run == 7) rx->pause_seen++;
    // A frame ends with 1 1 1 and starts with 1 0: look for 1110 xxxxxxxx p 111 once aligned
    if (rx->nbits >= 14) {
        uint32_t f = rx->shift & 0x3FFF;   // bit 13 = oldest
        if (((f >> 13) & 1) == 1 && ((f >> 12) & 1) == 0 && (f & 7) == 7) {
            uint8_t b = 0;
            int ones = 0;
            for (int i = 0; i < 8; i++) {
                int d = (f >> (11 - i)) & 1;
                b |= (uint8_t)(d << i);
                ones += d;
            }
            int p = (f >> 3) & 1;
            if (p != ((ones & 1) ^ 1)) rx->parity_errors++;
            if (rx->nbytes < (int)sizeof(rx->bytes)) rx->bytes[rx->nbytes++] = b;
            rx->nbits = 0;
        }
    }
}

static void rx_level(rx_t* rx, int level) {
    rx->units_since_rise++;
    if (level && !rx->last_level) {
        if (rx->units_since_rise == 2) rx_bit(rx, 1);
        else if (rx->units_since_rise == 4) rx_bit(rx, 0);
        rx->units_since_rise = 0;
    }
    rx->last_level = level;
}

static oric_td_t td;

static void tape_run(rx_t* rx, int units) {
    for (int i = 0; i < units; i++) {
        oric_td_tick(&td);
        if (rx) rx_level(rx, (td.port & ORIC_TD_PORT_READ) ? 1 : 0);
    }
}

// A tape with two files: "ONE" (5 bytes) and "TWO" (3 bytes)
static uint8_t tap[] = {
    0x16, 0x16, 0x16, 0x24, 0x00, 0x00, 0x80, 0x00, 0x05, 0x04, 0x05, 0x00, 0x00, 'O', 'N', 'E', 0x00,
    0x11, 0x22, 0x33, 0x44, 0x55,
    0x16, 0x16, 0x16, 0x24, 0x00, 0x00, 0x80, 0x00, 0x06, 0x02, 0x06, 0x00, 0x00, 'T', 'W', 'O', 0x00,
    0xA0, 0xB0, 0xC0,
};

static int count_bytes(const rx_t* rx, uint8_t v) {
    int n = 0;
    for (int i = 0; i < rx->nbytes; i++) n += rx->bytes[i] == v;
    return n;
}

static bool contains(const rx_t* rx, const uint8_t* seq, int len) {
    for (int i = 0; i + len <= rx->nbytes; i++) {
        if (memcmp(rx->bytes + i, seq, (size_t)len) == 0) return true;
    }
    return false;
}

static void test_tape(void) {
    static rx_t rx;
    memset(&td, 0, sizeof(td));
    oric_td_init(&td);
    CHECK(oric_td_insert_tap(&td, tap, sizeof(tap)));
    CHECK(oric_td_is_tape_inserted(&td));
    // Motor off: the tape does not move
    tape_run(0, 100);
    CHECK(td.tap_pos == 0);
    // Motor on at the leader: 80 extra 0x16, then the first file and a pause after its name
    memset(&rx, 0, sizeof(rx));
    td.port |= ORIC_TD_PORT_MOTOR;
    for (int i = 0; i < 20000 && !(td.tap_pos == 22 && td.tap_bit == 14 && td.tap_units == 0 && !td.tap_level); i++) {
        tape_run(&rx, 1);
    }
    tape_run(&rx, 2);   // Rising edge after the last bit
    CHECK(rx.parity_errors == 0);
    CHECK(count_bytes(&rx, 0x16) >= 83);
    const uint8_t file1[] = {0x24, 0x00, 0x00, 0x80, 0x00, 0x05, 0x04, 0x05, 0x00, 0x00, 'O', 'N', 'E', 0x00,
                             0x11, 0x22, 0x33, 0x44, 0x55};
    CHECK(contains(&rx, file1, sizeof(file1)));
    CHECK(rx.pause_seen >= 1);
    // Motor stopped after the whole first file: the tape stays after it
    td.port &= ~ORIC_TD_PORT_MOTOR;
    tape_run(0, 1);
    uint32_t pos = td.tap_pos;
    CHECK(pos >= 22);
    // The second file: motor stopped in its middle -> back to where the motor started
    memset(&rx, 0, sizeof(rx));
    td.port |= ORIC_TD_PORT_MOTOR;
    tape_run(&rx, 200);   // Leader only
    td.port &= ~ORIC_TD_PORT_MOTOR;
    tape_run(0, 1);
    CHECK(td.tap_pos == pos);
    td.port |= ORIC_TD_PORT_MOTOR;
    memset(&rx, 0, sizeof(rx));
    tape_run(&rx, 8000);
    const uint8_t file2[] = {'T', 'W', 'O', 0x00, 0xA0, 0xB0, 0xC0};
    CHECK(contains(&rx, file2, sizeof(file2)));
    CHECK(oric_td_tap_ended(&td));
    // Rewind (also done by a reset)
    oric_td_reset(&td);
    CHECK(td.tap_pos == 0 && !oric_td_tap_ended(&td) && oric_td_is_tape_inserted(&td));
    oric_td_remove_tape(&td);
    CHECK(!oric_td_is_tape_inserted(&td));
}

// Truncated last file (end address counted exclusive): completed with 0x00
static void test_tape_padding(void) {
    static rx_t rx;
    static const uint8_t t[] = {0x16, 0x16, 0x16, 0x24, 0x00, 0x00, 0x80, 0x00, 0x05, 0x03, 0x05, 0x00, 0x00, 'P', 0x00,
                                0x77, 0x88, 0x99};   // 4 bytes announced, 3 present
    memset(&td, 0, sizeof(td));
    oric_td_init(&td);
    oric_td_insert_tap(&td, t, sizeof(t));
    memset(&rx, 0, sizeof(rx));
    td.port |= ORIC_TD_PORT_MOTOR;
    tape_run(&rx, 8000);
    const uint8_t data[] = {'P', 0x00, 0x77, 0x88, 0x99, 0x00};
    CHECK(contains(&rx, data, sizeof(data)));
    CHECK(oric_td_tap_ended(&td));
}

// Streamed .tap gives the same signal as the in-memory one
static uint32_t t_read(void* ctx, uint32_t off, uint8_t* buf, uint32_t len) {
    (void)ctx;
    if (off >= sizeof(tap)) return 0;
    if (len > sizeof(tap) - off) len = (uint32_t)(sizeof(tap) - off);
    if (len > 7) len = 7;   // Small reads: several buffer refills
    memcpy(buf, tap + off, len);
    return len;
}

static void test_tape_streamed(void) {
    static oric_td_t a, b;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    oric_td_init(&a);
    oric_td_init(&b);
    oric_td_insert_tap(&a, tap, sizeof(tap));
    oric_td_insert_tap_streamed(&b, sizeof(tap), t_read, 0);
    a.port |= ORIC_TD_PORT_MOTOR;
    b.port |= ORIC_TD_PORT_MOTOR;
    int diff = 0;
    for (int i = 0; i < 12000; i++) {
        oric_td_tick(&a);
        oric_td_tick(&b);
        if ((a.port & ORIC_TD_PORT_READ) != (b.port & ORIC_TD_PORT_READ)) diff++;
    }
    CHECK(diff == 0);
    CHECK(oric_td_tap_ended(&a) && oric_td_tap_ended(&b));
}

/*-- VIA ----------------------------------------------------------------------*/

static void test_via_ier(void) {
    mos6522via_t via;
    mos6522via_init(&via);
    mos6522via_write(&via, MOS6522VIA_REG_ACR, 0x40);   // T1 continuous
    mos6522via_write(&via, MOS6522VIA_REG_T1CL, 100);
    mos6522via_write(&via, MOS6522VIA_REG_T1CH, 0);
    mos6522via_write(&via, MOS6522VIA_REG_IER, 0x80 | MOS6522VIA_IRQ_T1);
    bool irq = false;
    for (int i = 0; i < 200 && !irq; i++) irq = mos6522via_tick(&via, 1);
    CHECK(irq);
    // Disabling T1 in the IER releases IRQ (IRQ = IFR & IER); the flag stays in the IFR
    mos6522via_write(&via, MOS6522VIA_REG_IER, 0x7F);
    CHECK(!mos6522via_tick(&via, 1));
    CHECK(mos6522via_read(&via, MOS6522VIA_REG_IFR) & MOS6522VIA_IRQ_T1);
    CHECK(!(mos6522via_read(&via, MOS6522VIA_REG_IFR) & 0x80));
}

/*-- Oric interfaces ----------------------------------------------------------*/

static oric_t sys;

static uint8_t bus_read(uint16_t addr) {
    sys.cpu.addr = addr;
    sys.cpu.rw = true;
    _oric_mem_rw(&sys, addr, true);
    return sys.cpu.data;
}

static void bus_write(uint16_t addr, uint8_t v) {
    sys.cpu.addr = addr;
    sys.cpu.rw = false;
    sys.cpu.data = v;
    _oric_mem_rw(&sys, addr, false);
}

static void printer_cb(uint8_t c, void* user) { *(int*)user = c; }

static void test_oric_interfaces(void) {
    oric_rom[0x3FFC] = 0x00;
    oric_rom[0x3FFD] = 0xC0;
    oric_rom[0x0000] = 0xAB;   // Marker at $C000
    test_microdisc_rom[0x0000] = 0xCD;   // Marker at $E000
    test_jasmin_rom[0x0000] = 0xEF;      // Marker at $F800
    static int printed = -1;
    oric_desc_t desc = {
        .td_enabled = true,
        .fdc_type = ORIC_FDC_MICRODISC,
        .printer_cb = printer_cb,
        .printer_user_data = &printed,
        .roms = {
            .rom = {.ptr = oric_rom, .size = sizeof(oric_rom)},
            .boot_rom = {.ptr = test_boot_rom, .size = sizeof(test_boot_rom)},
            .microdisc_rom = {.ptr = test_microdisc_rom, .size = sizeof(test_microdisc_rom)},
            .jasmin_rom = {.ptr = test_jasmin_rom, .size = sizeof(test_jasmin_rom)},
        },
    };
    oric_init(&sys, &desc);
    CHECK(sys.fdc_type == ORIC_FDC_MICRODISC);
    // Power-on RAM pattern: 128 x $00, 128 x $FF per page
    CHECK(sys.ram[0x1000] == 0x00 && sys.ram[0x1080] == 0xFF && sys.overlay_ram[0x80] == 0xFF);
    // After a reset: BASIC ROM disabled, EPROM at $E000, overlay RAM at $C000-$DFFF
    CHECK(bus_read(0xE000) == 0xCD);
    bus_write(0xC000, 0x5A);
    CHECK(bus_read(0xC000) == 0x5A);
    bus_write(0xE000, 0x11);   // Ignored: EPROM
    CHECK(bus_read(0xE000) == 0xCD);
    // Control register: bit 1 = 1 -> BASIC ROM, writes ignored
    bus_write(0x0314, 0x82);
    CHECK(bus_read(0xC000) == 0xAB);
    bus_write(0xC000, 0x00);
    bus_write(0x0314, 0x80);   // ROM disabled, EPROM disabled: overlay RAM everywhere
    CHECK(bus_read(0xC000) == 0x5A && bus_read(0xE000) == sys.overlay_ram[0x2000]);
    // Drive and side selection, INTRQ and DRQ lines
    bus_write(0x0314, 0x80 | 0x40 | 0x10 | 0x01);
    CHECK(sys.wd.drive == 2 && sys.wd.side == 1);
    CHECK((bus_read(0x0314) & 0x80) == 0x80 && (bus_read(0x0318) & 0x80) == 0x80);
    bus_write(0x0310, 0x00);   // Restore (no disk)
    for (int i = 0; i < 100; i++) oric_tick(&sys);
    CHECK((bus_read(0x0314) & 0x80) == 0x00);   // /INTRQ low
    CHECK(sys.cpu.irq);                          // INTRQ enabled on IRQ
    bus_read(0x0310);
    CHECK((bus_read(0x0314) & 0x80) == 0x80);
    // Other addresses of page 3 reach the VIA
    bus_write(0x0303, 0x5F);   // DDRA
    CHECK(sys.via.pa.ddr == 0x5F);

    // Jasmin
    CHECK(oric_set_fdc(&sys, ORIC_FDC_JASMIN));
    CHECK(bus_read(0xC000) == 0xAB);   // BASIC ROM after a reset
    bus_write(0x03FB, 1);              // ROMDIS: Jasmin ROM at $F800
    CHECK(bus_read(0xF800) == 0xEF && bus_read(0x03FB) == 1);
    bus_write(0xF800, 0x00);
    CHECK(bus_read(0xF800) == 0xEF);
    bus_write(0xC100, 0x42);           // Overlay RAM below $F800
    CHECK(bus_read(0xC100) == 0x42);
    bus_write(0x03FA, 1);              // Overlay RAM everywhere
    CHECK(bus_read(0xF800) == sys.overlay_ram[0x3800] && bus_read(0x03FA) == 1);
    bus_write(0x03F8, 1);
    bus_write(0x03FD, 0);
    CHECK(sys.wd.side == 1 && sys.wd.drive == 1 && bus_read(0x03F8) == 1);
    // DRQ drives IRQ
    sys.wd.drq = true;
    oric_tick(&sys);
    CHECK(sys.cpu.irq);
    sys.wd.drq = false;

    // Interface of an image: FT-DOS boot sector -> Jasmin, else Microdisc
    uint8_t* img = make_blank_image(1, 1);
    wd1793_init(&fdc);
    wd1793_insert_mem(&fdc, 0, img, ORIC_DSK_HEADER_SIZE + ORIC_DSK_TRACK_SIZE, false);
    CHECK(format_track(0, 0));
    oric_dsk_t d;
    oric_dsk_open_mem(&d, img, ORIC_DSK_HEADER_SIZE + ORIC_DSK_TRACK_SIZE, true);
    CHECK(oric_dsk_interface(&sys, &d) == ORIC_FDC_MICRODISC);
    CHECK(oric_dsk_load_track(&fdc.cache, &fdc.disk[0], 0, 0));
    for (int i = 0; i < fdc.cache.num_sectors; i++) {
        if (fdc.cache.raw[fdc.cache.sectors[i].id + 3] == 1) {
            memcpy(fdc.cache.raw + fdc.cache.sectors[i].data + 1, _oric_ftdos_boot[0], 12);
        }
    }
    CHECK(oric_dsk_interface(&sys, &d) == ORIC_FDC_JASMIN);
    free(img);

    // Printer: falling edge of PB4 (output) sends port A, then CA1 acknowledges
    CHECK(oric_set_fdc(&sys, ORIC_FDC_NONE));
    bus_write(0x0302, 0xFF);   // DDRB
    bus_write(0x0303, 0xFF);   // DDRA
    bus_write(0x0300, 0x10);
    for (int i = 0; i < 8; i++) oric_tick(&sys);
    bus_write(0x0301, 'X');
    bus_write(0x0300, 0x00);
    for (int i = 0; i < 8; i++) oric_tick(&sys);
    CHECK(printed == 'X');
    bus_write(0x0300, 0x10);
    for (int i = 0; i < 100; i++) oric_tick(&sys);
    CHECK(bus_read(0x030D) & 0x02);   // CA1 flag
    // Tape relay: PB6 driven as an output only
    bus_write(0x0302, 0x00);          // DDRB: inputs
    for (int i = 0; i < 8; i++) oric_tick(&sys);
    CHECK(!oric_td_is_motor_on(&sys.td));
    bus_write(0x0302, 0xFF);
    bus_write(0x0300, 0x40);
    for (int i = 0; i < 8; i++) oric_tick(&sys);
    CHECK(oric_td_is_motor_on(&sys.td));
    oric_discard(&sys);
}

int main(void) {
    test_dsk_header();
    test_format_and_read();
    test_type1();
    test_read_errors();
    test_multiple_and_address();
    test_write_sector();
    test_no_disk();
    test_streamed();
    test_tape();
    test_tape_padding();
    test_tape_streamed();
    test_via_ier();
    test_oric_interfaces();
    free(image);
    printf("%d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
