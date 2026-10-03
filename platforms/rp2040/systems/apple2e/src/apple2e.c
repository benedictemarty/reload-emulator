// apple2e.c
//
// ## zlib/libpng license
//
// Copyright (c) 2023 Veselin Sladkov
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

#define MEM_PAGE_SHIFT (9U)

#define RGBA8(r, g, b) (0xFF000000 | (r << 16) | (g << 8) | (b))

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <pico/platform.h>
#include "pico/stdlib.h"

#ifdef OLIMEX_NEO6502
#include "roms/apple2ee_roms.h"
#else
#include "roms/apple2e_roms.h"
#endif
#include "images/apple2_images.h"
// #include "images/apple2_nib_images.h"

#include "tusb.h"
#include "ff.h"

// Code run every emulated cycle goes to RAM (as the BBC and the Oric do): from
// XIP flash, the bus driver and the memory decoding miss the 16 KB cache
#define APPLE2E_HOT    __attribute__((section(".time_critical.apple2e")))
#define CHIPS_HOT    __attribute__((section(".time_critical.apple2e")))
#define WDC65C02_HOT __attribute__((section(".time_critical.apple2e")))

#include "chips/chips_common.h"
#ifdef OLIMEX_NEO6502
#include "chips/wdc65C02cpu.h"
#else
#include "chips/mos6502cpu.h"
#endif
#include "chips/beeper.h"
#include "chips/kbd.h"
#include "chips/mem.h"
#include "chips/clk.h"
#include "devices/apple2_lc.h"
#include "devices/disk2_fdd.h"
#include "devices/disk2_fdc.h"
#include "devices/apple2_fdc_rom.h"
#ifdef APPLE_DIAG
volatile unsigned diag_hdd_reads;   // Counted by prodos_hdd.h (PRODOS_HDD_COUNT_READS)
#endif
#define NEO_STORAGE_IMPL
#include "devices/neo_storage.h"   // Hard disk image on the USB drive (FatFs driver of the socle)
#include "devices/prodos_hdd.h"
#include "devices/prodos_hdc.h"
#include "devices/prodos_hdc_rom.h"
#include "systems/apple2e.h"

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/vreg.h"
#include "hardware/interp.h"
#include "pico/multicore.h"

#include "tmds_encode.h"
#include "neo_multiboot.h"

#include "common_dvi_pin_configs.h"
#include "dvi.h"
#include "dvi_serialiser.h"

#include "audio.h"

typedef struct {
    uint32_t version;
    apple2e_t apple2e;
} apple2e_snapshot_t;

typedef struct {
    apple2e_t apple2e;
    uint32_t frame_time_us;
    uint32_t ticks;
    // double emu_time_ms;
} state_t;

static state_t __not_in_flash() state;

// Audio streaming callback
static void audio_callback(const uint8_t sample, void *user_data) {
    (void)user_data;
    audio_push_sample(sample);
}

// Get apple2e_desc_t struct based on joystick type
apple2e_desc_t apple2e_desc(void) {
    return (apple2e_desc_t){
        .fdc_enabled = false,
        .hdc_enabled = true,
        .hdc_internal_flash = false,
        .audio =
            {
                .callback = {.func = audio_callback},
                .sample_rate = 44100,
            },
        .roms =
            {
                .rom = {.ptr = apple2e_rom, .size = sizeof(apple2e_rom)},
                .character_rom = {.ptr = apple2e_character_rom, .size = sizeof(apple2e_character_rom)},
                .fdc_rom = {.ptr = apple2_fdc_rom, .size = sizeof(apple2_fdc_rom)},
                .hdc_rom = {.ptr = prodos_hdc_rom, .size = sizeof(prodos_hdc_rom)},
            },
    };
}

void app_init(void) {
    apple2e_desc_t desc = apple2e_desc();
    apple2e_init(&state.apple2e, &desc);
}

#if defined(OLIMEX_NEO6502) && defined(APPLE_VIDEO_640)
// Standard 640x480 at 60 Hz (CEA / VESA: 800 x 525 in all, 25.2 MHz pixel, both syncs
// negative), TMDS bit clock 252 MHz: for capture cards that reject 800x480 (MacroSilicon
// MS2109); the 560x384 picture fits, 40 columns on each side. System clock 252 MHz
#define FRAME_WIDTH  640
#define FRAME_HEIGHT 480
#define VREG_VSEL    VREG_VOLTAGE_1_20
#define DVI_TIMING   dvi_timing_640x480p_60hz
#elif defined(OLIMEX_NEO6502)
// TMDS bit clock 295.2 MHz, DVDD 1.2V: the 800x480 mode of the Oric and BBC builds,
// accepted by bmarty's screen (800x600 at 400 MHz / 1.3V before, never tried on the
// board); the 560x384 picture fits
#define FRAME_WIDTH  800
#define FRAME_HEIGHT 480
#define VREG_VSEL    VREG_VOLTAGE_1_20
#define DVI_TIMING   dvi_timing_800x480p_60hz
#else
// TMDS bit clock 400 MHz
// DVDD 1.3V
#define FRAME_WIDTH  800
#define FRAME_HEIGHT 600
#define VREG_VSEL    VREG_VOLTAGE_1_30
#define DVI_TIMING   dvi_timing_800x600p_60hz
#endif  // OLIMEX_NEO6502

#ifdef APPLE_TMDS_PALETTE
uint32_t __not_in_flash() tmds_palette[PALETTE_SIZE * 6];
#endif
uint32_t __not_in_flash() empty_tmdsbuf[3 * FRAME_WIDTH / DVI_SYMBOLS_PER_WORD];

uint8_t __not_in_flash() __attribute__((aligned(4))) scanbuf[FRAME_WIDTH];

struct dvi_inst dvi0;

#ifdef APPLE_TMDS_PALETTE
void tmds_palette_init() { tmds_setup_palette24_symbols(apple2e_palette, tmds_palette, PALETTE_SIZE); }
#endif

void kbd_raw_key_down(int code) {
    if (code == (NEO_MULTIBOOT_RETURN_KEY | 0x100)) neo_multiboot_return();   // Pause: back to the Neo6502 firmware
    if (isascii(code)) {
        if (isupper(code)) {
            code = tolower(code);
        } else if (islower(code)) {
            code = toupper(code);
        }
    }

    if (code == 0x14F) {
        // Arrow right
        code = 0x15;
    } else if (code == 0x150) {
        // Arrow left
        code = 0x08;
    } else if (code == 0x151) {
        // Arrow down
        code = 0x0A;
    } else if (code == 0x152) {
        // Arrow up
        code = 0x0B;
    }

    apple2e_t *sys = &state.apple2e;

    switch (code) {
        case 0x13A:  // F1
        case 0x13B:  // F2
        case 0x13C:  // F3
        case 0x13D:  // F4
        case 0x13E:  // F5
        case 0x13F:  // F6
        case 0x140:  // F7
        case 0x141:  // F8
        case 0x142:  // F9
        {
            if (sys->fdc.valid) {
                uint8_t index = code - 0x13A;
                if (CHIPS_ARRAY_SIZE(apple2_nib_images) > index) {
                    disk2_fdd_insert_disk(&sys->fdc.fdd[0], apple2_nib_images[index]);
                }
            }
            break;
        }

        case 0x145:  // F12
            apple2e_reset(sys);
            break;

        case 0x1E3:  // GUI LEFT
            sys->kbd_open_apple_pressed = true;
            break;

        case 0x1E7:  // GUI RIGHT
            sys->kbd_solid_apple_pressed = true;
            break;

        default:
            if (code < 128) {
                sys->kbd_last_key = code | 0x80;
            }
            break;
    }
    // printf("Key down: %d\n", code);
}

void kbd_raw_key_up(int code) {
    if (isascii(code)) {
        if (isupper(code)) {
            code = tolower(code);
        } else if (islower(code)) {
            code = toupper(code);
        }
    }

    apple2e_t *sys = &state.apple2e;

    switch (code) {
        case 0x1E3:  // GUI LEFT
            sys->kbd_open_apple_pressed = false;
            break;

        case 0x1E7:  // GUI RIGHT
            sys->kbd_solid_apple_pressed = false;
            break;

        default:
            break;
    }
    // printf("Key up: %d\n", code);
}

void gamepad_state_update(uint8_t index, uint8_t hat_state, uint32_t button_state) {
    apple2e_t *sys = &state.apple2e;

    sys->paddl0 = 0x80;
    sys->paddl1 = 0x80;
    sys->paddl2 = 0x80;
    sys->paddl3 = 0x80;

    switch (hat_state) {

        case GAMEPAD_HAT_CENTERED:
            break;

        case GAMEPAD_HAT_UP:
            if (index == 0) {
                sys->paddl1 = 0x00;
            } else {
                sys->paddl3 = 0x00;
            }
            break;

        case GAMEPAD_HAT_UP_RIGHT:
            if (index == 0) {
                sys->paddl0 = 0xFF;
                sys->paddl1 = 0x00;
            } else {
                sys->paddl2 = 0xFF;
                sys->paddl3 = 0x00;
            }
            break;

        case GAMEPAD_HAT_RIGHT:
            if (index == 0) {
                sys->paddl0 = 0xFF;
            } else {
                sys->paddl2 = 0xFF;
            }
            break;

        case GAMEPAD_HAT_DOWN_RIGHT:
            if (index == 0) {
                sys->paddl0 = 0xFF;
                sys->paddl1 = 0xFF;
            } else {
                sys->paddl2 = 0xFF;
                sys->paddl3 = 0xFF;
            }
            break;

        case GAMEPAD_HAT_DOWN:
            if (index == 0) {
                sys->paddl1 = 0xFF;
            } else {
                sys->paddl3 = 0xFF;
            }
            break;

        case GAMEPAD_HAT_DOWN_LEFT:
            if (index == 0) {
                sys->paddl0 = 0x00;
                sys->paddl1 = 0xFF;
            } else {
                sys->paddl2 = 0x00;
                sys->paddl3 = 0xFF;
            }
            break;

        case GAMEPAD_HAT_LEFT:
            if (index == 0) {
                sys->paddl0 = 0x00;
            } else {
                sys->paddl2 = 0x00;
            }
            break;

        case GAMEPAD_HAT_UP_LEFT:
            if (index == 0) {
                sys->paddl0 = 0x00;
                sys->paddl1 = 0x00;
            } else {
                sys->paddl2 = 0x00;
                sys->paddl3 = 0x00;
            }
            break;

        default:
            break;
    }

    sys->butn0 = false;
    sys->butn1 = false;
    sys->butn2 = false;

    if (button_state & GAMEPAD_BUTTON_A) {
        if (index == 0) {
            sys->butn0 = true;
        } else {
            sys->butn2 = true;
        }
    }
    if (button_state & GAMEPAD_BUTTON_B) {
        if (index == 0) {
            sys->butn1 = true;
        }
    }
    // printf("Gamepad state update: %d %d %d\n", index, hat_state, button_state);
}

extern void apple2e_render_scanline(const uint32_t *pixbuf, uint32_t *scanbuf, size_t n_pix);
extern void copy_tmdsbuf(uint32_t *dest, const uint32_t *src);

static inline void __not_in_flash_func(render_scanline)(const uint32_t *pixbuf, uint32_t *scanbuf, size_t n_pix) {
    interp_config c;

    c = interp_default_config();
    interp_config_set_cross_result(&c, true);
    interp_config_set_shift(&c, 0);
    interp_config_set_mask(&c, 0, 3);
    interp_config_set_signed(&c, false);
    interp_set_config(interp0, 0, &c);

    c = interp_default_config();
    interp_config_set_cross_result(&c, false);
    interp_config_set_shift(&c, 4);
    interp_config_set_mask(&c, 0, 31);
    interp_config_set_signed(&c, false);
    interp_set_config(interp0, 1, &c);

    apple2e_render_scanline(pixbuf, scanbuf, n_pix);
}

#ifdef APPLE_DIAG
// Bench over SWD (-DAPPLE_DIAG, tools/oric_carte.py taper): key queue as oric_diag (codes
// as hid_app.c gives them, each key held then released for 4 frames), frame counter,
// late scanlines of PicoDVI summed by core 1 (dvi0.late_scanline_ctr is a debt that goes
// down again: only its rises are added, sampled at every line)
volatile uint16_t diag_keyq[256];
volatile uint32_t diag_keyq_head, diag_keyq_tail, diag_frames, diag_exec_us, diag_exec_max;
volatile uint32_t diag_late_total, diag_late_frames, core1_frames;
volatile uint32_t diag_enc_us_max, diag_line_us_max, diag_enc_us_sum, diag_enc_n;   // Core 1: encoding of a line, whole line (render + encoding)
// Hard disk: insertion after a late mount (0 none, 1 done, 2 failed), its blocks; blocks read
volatile uint32_t diag_hdd_insert, diag_hdd_blocks, diag_msc_ready_ms;   // + time of the drive mounting after power-on
static uint32_t diag_late_prev;
static inline void __not_in_flash_func(diag_late_sample)(void) {
    const uint32_t late = dvi0.late_scanline_ctr;
    if (late > diag_late_prev) {
        diag_late_total += late - diag_late_prev;
        diag_late_frames = core1_frames;
    }
    diag_late_prev = late;
}
#define DIAG_LATE_SAMPLE() diag_late_sample()
#else
#define DIAG_LATE_SAMPLE() ((void)0)
#endif

#define APPLE2E_EMPTY_LINES   ((FRAME_HEIGHT - APPLE2E_SCREEN_HEIGHT * 2) / 4)
#define APPLE2E_EMPTY_COLUMNS ((FRAME_WIDTH - APPLE2E_SCREEN_WIDTH) / 2)

#ifndef APPLE_TMDS_PALETTE
// Line encoding: 2 levels per TMDS channel (RGB 2-2-2) with the symbols of PicoDVI's
// tmds_encode_2bpp, DC balanced by construction (even pixels imbalance -4, odd +4). The
// palette encoder of PicoDVI (tmds_encode_palette_data, kept with -DAPPLE_TMDS_PALETTE)
// drifts: on a black line it alternates 0x100 / 0x3FF, +2 a pair, +800 a line, never
// brought back to 0; monitors accept it, a MacroSilicon MS2109 capture card stays black
// (measured 2026-10-02). Two pixels a word: even symbol | odd symbol << 10
#define APPLE_TMDS_BLACK 0x7F103u   // Level 0, even then odd
static const uint16_t tmds2_even[4] = {0x103, 0x130, 0x230, 0x203};
static const uint16_t tmds2_odd[4] = {0x1FC, 0x1CF, 0x2CF, 0x2FC};
// [channel 0 blue, 1 green, 2 red][palette index]
// In scratch Y and the encoder in scratch X: memory banks of their own, out of the main
// RAM that core 0 hammers (in main RAM, encoding a line took up to 74 us: late lines)
// [palette index][blue, green, red, pad]: 16 bytes a colour (utils.S, apple_encode_pairs)
uint32_t __scratch_y("apple_sym") apple_sym_even[PALETTE_SIZE][4];
uint32_t __scratch_y("apple_sym") apple_sym_odd[PALETTE_SIZE][4];
extern void apple_encode_pairs(const uint8_t *pix, uint32_t *out, uint32_t pairs, uint32_t stride);
static void rgb222_init(void) {
    for (int i = 0; i < PALETTE_SIZE; i++) {
        const uint32_t c = apple2e_palette[i];
        const uint32_t level[3] = {(c >> 6) & 3, (c >> 14) & 3, (c >> 22) & 3};   // Blue, green, red
        for (int ch = 0; ch < 3; ch++) {
            apple_sym_even[i][ch] = tmds2_even[level[ch]];
            apple_sym_odd[i][ch] = (uint32_t)tmds2_odd[level[ch]] << 10;
        }
    }
}
// row: the Apple frame buffer row (280 bytes, two pixels a byte), read directly (no
// render_scanline pass); the borders are black
static void __scratch_x("encode_line") __attribute__((noinline)) encode_line(uint32_t *tmdsbuf, const uint8_t *row) {
    const int words = FRAME_WIDTH / 2, first = APPLE2E_EMPTY_COLUMNS / 2, last = first + APPLE2E_SCREEN_WIDTH / 2;
    uint32_t *b = tmdsbuf, *g = tmdsbuf + words, *r = tmdsbuf + 2 * words;
    for (int k = 0; k < first; k++) b[k] = g[k] = r[k] = APPLE_TMDS_BLACK;
    for (int k = last; k < words; k++) b[k] = g[k] = r[k] = APPLE_TMDS_BLACK;
    apple_encode_pairs(row, b + first, last - first, words * 4);
}
// Line y of the Apple picture into a TMDS buffer
#define RENDER_LINE(buf, y) encode_line((buf), &state.apple2e.fb[(y) * 280])
#else
#define RENDER_LINE(buf, y)                                                                                   \
    do {                                                                                                      \
        render_scanline((const uint32_t *)(&state.apple2e.fb[(y) * 280]),                                     \
                        (uint32_t *)(&scanbuf[APPLE2E_EMPTY_COLUMNS]), 280);                                  \
        tmds_encode_palette_data((const uint32_t *)scanbuf, tmds_palette, (buf), FRAME_WIDTH, PALETTE_BITS); \
    } while (0)
#endif

static inline void __not_in_flash_func(render_empty_scanlines)() {
    for (int y = 0; y < APPLE2E_EMPTY_LINES; y += 2) {
        uint32_t *tmdsbuf;
        queue_remove_blocking_u32(&dvi0.q_tmds_free, &tmdsbuf);
        copy_tmdsbuf(tmdsbuf, empty_tmdsbuf);
        queue_add_blocking_u32(&dvi0.q_tmds_valid, &tmdsbuf);
        DIAG_LATE_SAMPLE();

        queue_remove_blocking_u32(&dvi0.q_tmds_free, &tmdsbuf);
        copy_tmdsbuf(tmdsbuf, empty_tmdsbuf);
        queue_add_blocking_u32(&dvi0.q_tmds_valid, &tmdsbuf);
        DIAG_LATE_SAMPLE();
    }
}

static inline void __not_in_flash_func(render_frame)() {
    for (int y = 0; y < APPLE2E_SCREEN_HEIGHT; y += 2) {
        uint32_t *tmdsbuf;
        queue_remove_blocking_u32(&dvi0.q_tmds_free, &tmdsbuf);
#ifdef APPLE_DIAG
        const uint32_t t0 = time_us_32();
#endif
#ifdef APPLE_DIAG
        const uint32_t t1 = t0;
#endif
        RENDER_LINE(tmdsbuf, y);
#ifdef APPLE_DIAG
        const uint32_t t2 = time_us_32();
        if (t2 - t1 > diag_enc_us_max) diag_enc_us_max = t2 - t1;
        diag_enc_us_sum += t2 - t1;
        diag_enc_n++;
        if (t2 - t0 > diag_line_us_max) diag_line_us_max = t2 - t0;
#endif
        queue_add_blocking_u32(&dvi0.q_tmds_valid, &tmdsbuf);
        DIAG_LATE_SAMPLE();

        queue_remove_blocking_u32(&dvi0.q_tmds_free, &tmdsbuf);
        RENDER_LINE(tmdsbuf, y + 1);
        queue_add_blocking_u32(&dvi0.q_tmds_valid, &tmdsbuf);
        DIAG_LATE_SAMPLE();
    }
}

void __not_in_flash_func(core1_main()) {
    audio_init(_AUDIO_PIN, 44100);

    dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
    dvi_start(&dvi0);

    while (1) {
#ifdef APPLE_DIAG
        core1_frames++;
#endif
        render_empty_scanlines();
        render_frame();
        render_empty_scanlines();
    }

    __builtin_unreachable();
}

extern const neo_storage_ops_t neo_storage_fatfs_ops;   // neo_storage_fatfs.c (socle)
extern bool msc_inquiry_complete;
extern void msc_poll(void);

// Waits for the USB key (APPLE_MSC_WAIT_US): without a key (or without the image), the
// slot 7 ROM finds no disk and the Autostart ROM goes on to BASIC
// USB drive at power-on: the drive tried on the board was ready 3.2 s after start-up (the
// former 3 s wait missed it: slot 7 found no disk, BASIC). 6 s, twice that; without a
// drive the machine goes to BASIC after them
#ifndef APPLE_MSC_WAIT_US
#define APPLE_MSC_WAIT_US 6000000
#endif
void wait_for_msc_ready(void) {
    uint32_t start = time_us_32();
    while (!msc_inquiry_complete && time_us_32() - start < APPLE_MSC_WAIT_US) {
        sleep_us(16666);
        tuh_task();
        msc_poll();   // Mounts the volume outside tuh_task
    }
}

#ifdef APPLE_DIAG
// USB drive stress test (bench) : writing N to diag_stress_go over SWD reads the first N blocks
// of the hard disk image from block diag_stress_first, 32 a frame, while the emulation runs ; FNV-1a 32 of the bytes read
// (to compare with the same sum over the file on the PC), reads failed, duration
volatile uint32_t diag_stress_first, diag_stress_go, diag_stress_done, diag_stress_fnv = 2166136261u, diag_stress_fail, diag_stress_ms;
static uint32_t diag_stress_start;
static void diag_stress_service(void) {
    prodos_hdd_t *hdd = &state.apple2e.hdc.hdd[0];
    if (!diag_stress_go || diag_stress_done >= diag_stress_go || !prodos_hdd_is_disk_inserted(hdd)) return;
    if (diag_stress_done == 0) diag_stress_start = to_ms_since_boot(get_absolute_time());
    uint8_t buf[PRODOS_HDD_BYTES_PER_BLOCK];
    for (int n = 0; n < 32 && diag_stress_done < diag_stress_go; n++) {
        if (neo_file_read(&hdd->file, (diag_stress_first + diag_stress_done) * PRODOS_HDD_BYTES_PER_BLOCK, buf, sizeof(buf))) {
            uint32_t h = diag_stress_fnv;
            for (int i = 0; i < PRODOS_HDD_BYTES_PER_BLOCK; i++) h = (h ^ buf[i]) * 16777619u;
            diag_stress_fnv = h;
        } else {
            diag_stress_fail++;
        }
        diag_stress_done++;
    }
    diag_stress_ms = to_ms_since_boot(get_absolute_time()) - diag_stress_start;
}

static void diag_key_service(void) {
    static int phase, timer, key;
    diag_frames++;
    if (timer > 0) {
        timer--;
    } else if (phase == 1) {
        kbd_raw_key_up(key);
        phase = 0;
        timer = 4;
    } else if (diag_keyq_head != diag_keyq_tail) {
        key = diag_keyq[diag_keyq_head & 255];
        diag_keyq_head++;
        kbd_raw_key_down(key);
        phase = 1;
        timer = 4;
    }
}
#endif

// USB drive mounted after the boot wait, or pulled out: msc_poll mounts it outside
// tuh_task, and the hard disk image is inserted (to boot it: PR#7). Restarting the machine
// cold there did not boot slot 7 on the board (2026-10-02, cause not found): not done
static void usb_disk_service(void) {
    static bool inserted_late;
    msc_poll();
    prodos_hdd_t *hdd = &state.apple2e.hdc.hdd[0];
    if (!state.apple2e.hdc.valid || CHIPS_ARRAY_SIZE(apple2_msc_images) == 0) return;
    if (!msc_inquiry_complete) {
        if (prodos_hdd_is_disk_inserted(hdd)) prodos_hdd_remove_disk(hdd);   // Drive pulled out
        inserted_late = false;
        return;
    }
    if (prodos_hdd_is_disk_inserted(hdd) || inserted_late) return;
    inserted_late = true;   // One attempt per mounting
#ifdef APPLE_DIAG
    diag_msc_ready_ms = to_ms_since_boot(get_absolute_time());
#endif
    const bool ok = prodos_hdd_insert_disk_file(hdd, apple2_msc_images[0]);
#ifdef APPLE_DIAG
    diag_hdd_insert = ok ? 1 : 2;
    diag_hdd_blocks = prodos_hdd_get_blocks(hdd);
#else
    (void)ok;
#endif
}

// The emulation loop, with apple2e_tick inlined, runs from RAM too
static void __no_inline_not_in_flash_func(run_ticks)(uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        apple2e_tick(&state.apple2e);
    }
}

int main() {
    vreg_set_voltage(VREG_VSEL);
    sleep_ms(10);
    set_sys_clock_khz(DVI_TIMING.bit_clk_khz, true);

    stdio_init_all();
    tusb_init();

    printf("Configuring DVI\n");

    dvi0.timing = &DVI_TIMING;
    dvi0.ser_cfg = DVI_DEFAULT_SERIAL_CONFIG;
    // Dedicated spin locks: the striped ones are shared with FatFs and TinyUSB (core 1
    // then waits with its IRQs masked: late lines, red dashes; docs/STRATEGIE-NEO6502.md)
    dvi_init(&dvi0, spin_lock_claim_unused(true), spin_lock_claim_unused(true));

#ifdef APPLE_TMDS_PALETTE
    tmds_palette_init();
#endif
#ifndef APPLE_TMDS_PALETTE
    rgb222_init();
    {   // Black line: all of it border
        const int words = FRAME_WIDTH / 2;
        for (int k = 0; k < 3 * words; k++) empty_tmdsbuf[k] = APPLE_TMDS_BLACK;
    }
#else
    tmds_encode_palette_data((const uint32_t *)scanbuf, tmds_palette, empty_tmdsbuf, FRAME_WIDTH, PALETTE_BITS);
#endif

    printf("Core 1 start\n");
    hw_set_bits(&bus_ctrl_hw->priority, BUSCTRL_BUS_PRIORITY_PROC1_BITS);
    multicore_launch_core1(core1_main);

    neo_storage_set(NEO_VOL_USB, "Clé USB", &neo_storage_fatfs_ops, "");
    wait_for_msc_ready();

    app_init();

    while (1) {
        uint32_t start_time_in_micros = time_us_32();

        uint32_t num_ticks = 17030;
        run_ticks(num_ticks);

        apple2e_screen_update(&state.apple2e);
        tuh_task();
        usb_disk_service();
#ifdef APPLE_DIAG
        diag_key_service();
        diag_stress_service();
#endif

        uint32_t end_time_in_micros = time_us_32();
        uint32_t execution_time = end_time_in_micros - start_time_in_micros;
#ifdef APPLE_DIAG
        diag_exec_us = execution_time;
        if (execution_time > diag_exec_max) diag_exec_max = execution_time;
#endif
        // printf("%d us\n", execution_time);

        int sleep_time = 16666 - execution_time;
        if (sleep_time > 0) {
            sleep_us(sleep_time);
        }
    }

    __builtin_unreachable();
}
