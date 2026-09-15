// bbc.c
//
// BBC Micro Model B on the Olimex Neo6502: the real 65C02 executes the MOS,
// the RP2040 emulates memory, 6845/ULA video, VIAs, keyboard and SN76489.
//
// Core 0: CPU bus (bit-banged, 2 MHz target), USB host.
// Discs: images compiled in flash (src/images/bbc_images.h), F11 = next image.
// Core 1: DVI 800x480 @ 60 Hz, BBC 640x256 framebuffer centred, lines
//         BBC_DISPLAY_TOP .. BBC_DISPLAY_TOP+239 doubled (256 lines do not
//         fit twice in 480; policy: crop 8 lines top and bottom).
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

#define RGBA8(r, g, b) (0xFF000000 | ((r) << 16) | ((g) << 8) | (b))

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pico/platform.h>
#include "pico/stdlib.h"

#include "roms/bbc_roms.h"
#if __has_include("images/bbc_images.h")
#include "images/bbc_images.h"
#define BBC_NUM_IMAGES ((int)(sizeof(bbc_disc_images) / sizeof(bbc_disc_images[0])))
#else
#define BBC_NUM_IMAGES 0
#endif

#include "chips/chips_common.h"
#ifdef OLIMEX_NEO6502
#include "chips/wdc65C02cpu.h"
#else
#include "chips/mos6502cpu.h"
#endif
#include "chips/mos6522via.h"
#include "chips/mem.h"
#include "chips/clk.h"
#include "devices/wd1770.h"
#include "systems/bbc.h"
#include "systems/bbc_keys.h"

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/vreg.h"
#include "pico/multicore.h"

#include "tmds_encode.h"

#include "common_dvi_pin_configs.h"
#include "dvi.h"
#include "dvi_serialiser.h"

#include "audio.h"

#include "tusb.h"
#include "class/hid/hid.h"

typedef struct {
    bbc_t bbc;
    uint32_t frame_time_us;
    uint32_t ticks;
} state_t;

state_t __not_in_flash() state;

// Audio streaming callback
static void audio_callback(const uint8_t sample, void *user_data) {
    (void)user_data;
    audio_push_sample(sample);
}

bbc_desc_t bbc_desc(void) {
    bbc_desc_t desc = {
        .audio =
            {
                .callback = {.func = audio_callback},
                .sample_rate = 22050,
            },
        .roms =
            {
                .os = {.ptr = bbc_os_rom, .size = sizeof(bbc_os_rom)},
            },
    };
    desc.roms.banks[15] = (chips_range_t){.ptr = bbc_basic_rom, .size = sizeof(bbc_basic_rom)};
    desc.roms.banks[14] = (chips_range_t){.ptr = bbc_dfs_rom, .size = sizeof(bbc_dfs_rom)};
    return desc;
}

static int current_image __attribute__((unused)) = -1;

// Insert flash image `index` (read-only) in drive 0
static void insert_image(int index) {
#if BBC_NUM_IMAGES > 0
    if (index < 0 || index >= BBC_NUM_IMAGES) return;
    const bbc_disc_image_t* im = &bbc_disc_images[index];
    bbc_insert_disc(&state.bbc, 0, (uint8_t*)im->data, im->size, im->sides, true);
    current_image = index;
    printf("Disc %d inserted (%u bytes)\n", index, (unsigned)im->size);
#else
    (void)index;
#endif
}

void app_init(void) {
    bbc_desc_t desc = bbc_desc();
    bbc_init(&state.bbc, &desc);
    bbc_reset(&state.bbc);
    insert_image(0);
}

// TMDS bit clock 295.2 MHz, DVDD 1.2V (Neo6502 timing shared with the Oric build)
#define FRAME_WIDTH  800
#define FRAME_HEIGHT 480
#define VREG_VSEL    VREG_VOLTAGE_1_20
#define DVI_TIMING   dvi_timing_800x480p_60hz

// First BBC line shown (lines BBC_DISPLAY_TOP .. BBC_DISPLAY_TOP + 239 are doubled)
#ifndef BBC_DISPLAY_TOP
#define BBC_DISPLAY_TOP 8
#endif
#define BBC_DISPLAY_LINES (FRAME_HEIGHT / 2)
#define BBC_EMPTY_COLUMNS ((FRAME_WIDTH - BBC_SCREEN_WIDTH) / 2)

uint32_t __not_in_flash() tmds_palette[BBC_PALETTE_SIZE * 6];

uint8_t __not_in_flash() scanbuf[FRAME_WIDTH];

struct dvi_inst dvi0;

void tmds_palette_init() { tmds_setup_palette24_symbols(bbc_palette, tmds_palette, BBC_PALETTE_SIZE); }

/*-- Keyboard: HID usage codes -> BBC matrix ---------------------------------*/

static int bbc_key_from_hid(uint8_t k) {
    if (k >= HID_KEY_A && k <= HID_KEY_Z) {
        static const uint8_t letters[26] = {
            BBC_KEY_A, BBC_KEY_B, BBC_KEY_C, BBC_KEY_D, BBC_KEY_E, BBC_KEY_F, BBC_KEY_G, BBC_KEY_H, BBC_KEY_I,
            BBC_KEY_J, BBC_KEY_K, BBC_KEY_L, BBC_KEY_M, BBC_KEY_N, BBC_KEY_O, BBC_KEY_P, BBC_KEY_Q, BBC_KEY_R,
            BBC_KEY_S, BBC_KEY_T, BBC_KEY_U, BBC_KEY_V, BBC_KEY_W, BBC_KEY_X, BBC_KEY_Y, BBC_KEY_Z};
        return letters[k - HID_KEY_A];
    }
    if (k >= HID_KEY_1 && k <= HID_KEY_9) {
        static const uint8_t digits[9] = {BBC_KEY_1, BBC_KEY_2, BBC_KEY_3, BBC_KEY_4, BBC_KEY_5,
                                          BBC_KEY_6, BBC_KEY_7, BBC_KEY_8, BBC_KEY_9};
        return digits[k - HID_KEY_1];
    }
    switch (k) {
        case HID_KEY_0: return BBC_KEY_0;
        case HID_KEY_ENTER: return BBC_KEY_Return;
        case HID_KEY_ESCAPE: return BBC_KEY_Escape;
        case HID_KEY_BACKSPACE: return BBC_KEY_Delete;
        case HID_KEY_DELETE: return BBC_KEY_Delete;
        case HID_KEY_TAB: return BBC_KEY_Tab;
        case HID_KEY_SPACE: return BBC_KEY_Space;
        case HID_KEY_MINUS: return BBC_KEY_Minus;
        case HID_KEY_EQUAL: return BBC_KEY_Caret;
        case HID_KEY_BRACKET_LEFT: return BBC_KEY_LeftSquareBracket;
        case HID_KEY_BRACKET_RIGHT: return BBC_KEY_RightSquareBracket;
        case HID_KEY_BACKSLASH: return BBC_KEY_Backslash;
        case HID_KEY_EUROPE_1: return BBC_KEY_Backslash;
        case HID_KEY_SEMICOLON: return BBC_KEY_Semicolon;
        case HID_KEY_APOSTROPHE: return BBC_KEY_Colon;
        case HID_KEY_GRAVE: return BBC_KEY_At;
        case HID_KEY_COMMA: return BBC_KEY_Comma;
        case HID_KEY_PERIOD: return BBC_KEY_Stop;
        case HID_KEY_SLASH: return BBC_KEY_Slash;
        case HID_KEY_CAPS_LOCK: return BBC_KEY_CapsLock;
        case HID_KEY_F1: return BBC_KEY_f1;
        case HID_KEY_F2: return BBC_KEY_f2;
        case HID_KEY_F3: return BBC_KEY_f3;
        case HID_KEY_F4: return BBC_KEY_f4;
        case HID_KEY_F5: return BBC_KEY_f5;
        case HID_KEY_F6: return BBC_KEY_f6;
        case HID_KEY_F7: return BBC_KEY_f7;
        case HID_KEY_F8: return BBC_KEY_f8;
        case HID_KEY_F9: return BBC_KEY_f9;
        case HID_KEY_F10: return BBC_KEY_f0;
        case HID_KEY_F12: return BBC_KEY_Break;
        case HID_KEY_HOME: return BBC_KEY_Underline;
        case HID_KEY_END: return BBC_KEY_Copy;
        case HID_KEY_ARROW_RIGHT: return BBC_KEY_Right;
        case HID_KEY_ARROW_LEFT: return BBC_KEY_Left;
        case HID_KEY_ARROW_DOWN: return BBC_KEY_Down;
        case HID_KEY_ARROW_UP: return BBC_KEY_Up;
        case HID_KEY_SHIFT_LEFT: case HID_KEY_SHIFT_RIGHT: return BBC_KEY_Shift;
        case HID_KEY_CONTROL_LEFT: case HID_KEY_CONTROL_RIGHT: return BBC_KEY_Ctrl;
        case HID_KEY_ALT_RIGHT: return BBC_KEY_ShiftLock;
        default: return -1;
    }
}

void hid_raw_key_down(uint8_t keycode) {
    if (keycode == HID_KEY_F11) {
        // Next disc image
#if BBC_NUM_IMAGES > 0
        insert_image((current_image + 1) % BBC_NUM_IMAGES);
#endif
        return;
    }
    int key = bbc_key_from_hid(keycode);
    if (key >= 0) {
        bbc_key_down(&state.bbc, (uint8_t)key);
    }
}

void hid_raw_key_up(uint8_t keycode) {
    int key = bbc_key_from_hid(keycode);
    if (key >= 0) {
        bbc_key_up(&state.bbc, (uint8_t)key);
    }
}

/*-- Core 1: DVI -------------------------------------------------------------*/

extern void copy_tmdsbuf(uint32_t *dest, const uint32_t *src);

// Unpack one 4 bpp BBC line (320 bytes) into 640 palette indices
static inline void __not_in_flash_func(unpack_scanline)(const uint8_t *src, uint8_t *dst) {
    for (int i = 0; i < BBC_SCREEN_WIDTH / 2; i++) {
        uint8_t b = src[i];
        dst[i * 2] = b >> 4;
        dst[i * 2 + 1] = b & 0x0F;
    }
}

static inline void __not_in_flash_func(render_frame)() {
    for (int y = 0; y < BBC_DISPLAY_LINES; y++) {
        int src_line = BBC_DISPLAY_TOP + y;
        uint32_t *tmdsbuf;
        queue_remove_blocking_u32(&dvi0.q_tmds_free, &tmdsbuf);
        if (src_line < BBC_SCREEN_HEIGHT) {
            unpack_scanline(&state.bbc.fb[src_line * (BBC_SCREEN_WIDTH / 2)], &scanbuf[BBC_EMPTY_COLUMNS]);
        } else {
            memset(&scanbuf[BBC_EMPTY_COLUMNS], 0, BBC_SCREEN_WIDTH);
        }
        tmds_encode_palette_data((const uint32_t *)scanbuf, tmds_palette, tmdsbuf, FRAME_WIDTH, BBC_PALETTE_BITS);
        queue_add_blocking_u32(&dvi0.q_tmds_valid, &tmdsbuf);

        // Same line again (vertical doubling)
        uint32_t *tmdsbuf2;
        queue_remove_blocking_u32(&dvi0.q_tmds_free, &tmdsbuf2);
        copy_tmdsbuf(tmdsbuf2, tmdsbuf);
        queue_add_blocking_u32(&dvi0.q_tmds_valid, &tmdsbuf2);
    }
}

void __not_in_flash_func(core1_main()) {
    audio_init(_AUDIO_PIN, 22050);

    dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
    dvi_start(&dvi0);

    while (1) {
        render_frame();
    }

    __builtin_unreachable();
}

/*-- Core 0: CPU bus ---------------------------------------------------------*/

int main() {
    vreg_set_voltage(VREG_VSEL);
    sleep_ms(10);
    set_sys_clock_khz(DVI_TIMING.bit_clk_khz, true);

    stdio_init_all();
    tusb_init();

    printf("BBC Micro on Neo6502: configuring DVI\n");

    dvi0.timing = &DVI_TIMING;
    dvi0.ser_cfg = DVI_DEFAULT_SERIAL_CONFIG;
    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());

    tmds_palette_init();
    memset(scanbuf, 0, sizeof(scanbuf));

    printf("Core 1 start\n");
    hw_set_bits(&bus_ctrl_hw->priority, BUSCTRL_BUS_PRIORITY_PROC1_BITS);
    multicore_launch_core1(core1_main);

    app_init();

    // One frame = 20 ms = 40 000 cycles at 2 MHz
    const uint32_t frame_us = 20000;
    const uint32_t num_ticks = frame_us * (BBC_FREQUENCY / 1000000);
    uint32_t report_frames = 0;
    uint32_t report_busy_us = 0;

    while (1) {
        uint32_t start_time_in_micros = time_us_32();

        for (uint32_t ticks = 0; ticks < num_ticks; ticks++) {
            bbc_tick(&state.bbc);
        }

        tuh_task();

        uint32_t execution_time = time_us_32() - start_time_in_micros;
        state.frame_time_us = execution_time;
        report_busy_us += execution_time;
        if (++report_frames == 250) {
            // Every 5 s: average time spent per 20 ms frame (>= 20000 means the bus cannot reach 2 MHz)
            printf("frame: %lu us for %lu cycles\n", (unsigned long)(report_busy_us / report_frames), (unsigned long)num_ticks);
            report_frames = 0;
            report_busy_us = 0;
        }

        int sleep_time = (int)frame_us - (int)execution_time;
        if (sleep_time > 0) {
            sleep_us(sleep_time);
        }
    }

    __builtin_unreachable();
}
