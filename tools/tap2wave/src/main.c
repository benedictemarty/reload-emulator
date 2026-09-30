// tap2wave: convert an Oric .tap image into a WAVE image for the tape drive
// of reload-emulator (src/devices/oric_td.h)
//
// WAVE image: size in bytes (uint32 LE), then one bit per 208 us (the tape
// drive tick), MSB first, 1 = high level.
//
// The signal is the one the tape drive builds from a .tap image (Oricutron's
// signal, see oric_td.h): the generator of oric_td.h is sampled, with the
// motor stopped and restarted between files so that each file gets its long
// leader and the pause after its header. The former encoding of this tool
// (bit 0 = 208 + 416 us, no leader) did not load with the BASIC 1.1 ROM.
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

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

#include "devices/oric_td.h"

// Max size of the tape image
#define MAX_TAP_IMAGE_SIZE (512 * 1024)
// Max size of the wave image
#define MAX_WAVE_IMAGE_SIZE (8 * 1024 * 1024)

static uint8_t tap_image[MAX_TAP_IMAGE_SIZE];
static uint8_t wave_image[MAX_WAVE_IMAGE_SIZE];

static int convert_tap_to_wave(const char* tap_file, const char* wave_file) {
    FILE* in = fopen(tap_file, "rb");
    if (!in) {
        fprintf(stderr, "Failed to open file for reading: %s\n", tap_file);
        return 1;
    }
    size_t tap_size = fread(tap_image, 1, sizeof(tap_image), in);
    if (!feof(in) || tap_size == 0) {
        fprintf(stderr, "Invalid TAP image size: %s\n", tap_file);
        fclose(in);
        return 1;
    }
    fclose(in);

    static oric_td_t td;
    oric_td_init(&td);
    oric_td_insert_tap(&td, tap_image, (uint32_t)tap_size);
    td.port |= ORIC_TD_PORT_MOTOR;

    uint32_t bits = 0;
    while (!oric_td_tap_ended(&td)) {
        // Between two files: motor stopped then restarted (long leader and pause for the next one)
        if (td.tap_bit >= 14 && td.tap_units == 0 && !td.tap_level && td.tap_hdr_end == 0 && td.tap_delay == 0 &&
            td.tap_pos >= td.tap_file_end && td.tap_pos < td.tap_size) {
            td.port &= ~ORIC_TD_PORT_MOTOR;
            oric_td_tick(&td);
            td.port |= ORIC_TD_PORT_MOTOR;
        }
        oric_td_tick(&td);
        if (bits / 8 >= sizeof(wave_image)) {
            fprintf(stderr, "WAVE image too large\n");
            return 1;
        }
        if (td.port & ORIC_TD_PORT_READ) {
            wave_image[bits / 8] |= (uint8_t)(0x80 >> (bits & 7));
        }
        bits++;
    }
    uint32_t wave_size = (bits + 7) / 8;

    FILE* out = fopen(wave_file, "wb");
    if (!out) {
        fprintf(stderr, "Failed to open file for writing: %s\n", wave_file);
        return 1;
    }
    const uint8_t header[4] = {(uint8_t)wave_size, (uint8_t)(wave_size >> 8), (uint8_t)(wave_size >> 16),
                               (uint8_t)(wave_size >> 24)};
    fwrite(header, 1, 4, out);
    fwrite(wave_image, 1, wave_size, out);
    fclose(out);
    printf("%s: %u bytes of signal (%.1f s)\n", wave_file, (unsigned)wave_size, bits * 208e-6);
    return 0;
}

static void print_usage(const char* argv0) {
    fprintf(stderr,
            "Usage: %s [-i TAP_file] [-o WAVE_file]\n"
            "\t-h show this help\n",
            argv0);
    exit(1);
}

int main(int argc, char* const argv[]) {
    char *infile = NULL, *outfile = NULL;
    int opt;

    while ((opt = getopt(argc, argv, "i:o:h")) != -1) {
        switch (opt) {
            case 'i':
                infile = strdup(optarg);
                break;
            case 'o':
                outfile = strdup(optarg);
                break;
            case 'h':
            default:
                print_usage(argv[0]);
                break;
        }
    }

    if (!infile || !outfile) {
        print_usage(argv[0]);
    }
    return convert_tap_to_wave(infile, outfile);
}
