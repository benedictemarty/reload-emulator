// bbc.c
//
// BBC Micro Model B on the Olimex Neo6502: the real 65C02 executes the MOS,
// the RP2040 emulates memory, 6845/ULA video, VIAs, keyboard and SN76489.
//
// Core 0: CPU bus (bit-banged, 2 MHz target), USB host (keyboard, gamepad = joystick 1).
// Discs: .ssd/.dsd files in the root of a USB drive (FAT, sectors read on
// demand through FatFs, writes go back to the file) or images compiled in
// flash (src/images/bbc_images.h, read-only);
// F11 = next image (USB files first, then flash images).
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

// ROMs: in SRAM by default (fastest bus service); BBC_ROMS_IN_FLASH keeps them in
// XIP flash (cached) to free 48 KB of SRAM — required for the Master 128 build.
#ifdef BBC_ROMS_IN_FLASH
#pragma push_macro("__not_in_flash")
#undef __not_in_flash
#define __not_in_flash() __in_flash("bbcroms")
#endif
#ifdef BBC_MASTER
#include "roms/bbc_master_roms.h"
#else
#include "roms/bbc_roms.h"
#endif
#ifdef BBC_ROMS_IN_FLASH
#pragma pop_macro("__not_in_flash")
#endif
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
#include "neo_multiboot.h"
#include "ff.h"

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
    };
#ifdef BBC_MASTER
    // Master 128: MOS 3.20 in flash, 32 KB of LYNNE/ANDY/HAZEL, no sideways RAM (SRAM budget)
    static uint8_t master_ram[0x8000];
    desc.model = BBC_MODEL_MASTER;
    desc.roms.os = (chips_range_t){.ptr = bbc_master_mos_rom, .size = sizeof(bbc_master_mos_rom)};
    desc.roms.banks[9] = (chips_range_t){.ptr = bbc_master_dfs_rom, .size = 0x4000};
    desc.roms.banks[10] = (chips_range_t){.ptr = bbc_master_viewsht_rom, .size = 0x4000};
    desc.roms.banks[11] = (chips_range_t){.ptr = bbc_master_edit_rom, .size = 0x4000};
    desc.roms.banks[12] = (chips_range_t){.ptr = bbc_master_basic4_rom, .size = 0x4000};
    desc.roms.banks[13] = (chips_range_t){.ptr = bbc_master_adfs_rom, .size = 0x4000};
    desc.roms.banks[14] = (chips_range_t){.ptr = bbc_master_view_rom, .size = 0x4000};
    desc.roms.banks[15] = (chips_range_t){.ptr = bbc_master_terminal_rom, .size = 0x4000};
    desc.master_ram = (chips_range_t){.ptr = master_ram, .size = sizeof(master_ram)};
#else
    desc.roms.os = (chips_range_t){.ptr = bbc_os_rom, .size = sizeof(bbc_os_rom)};
    desc.roms.banks[15] = (chips_range_t){.ptr = bbc_basic_rom, .size = sizeof(bbc_basic_rom)};
    desc.roms.banks[14] = (chips_range_t){.ptr = bbc_dfs_rom, .size = sizeof(bbc_dfs_rom)};
    // Sideways RAM in banks 4.. (1 x 16 KB)
    static uint8_t swr[1 * 0x4000];
    desc.ram_banks = 0x0010;
    desc.swr = (chips_range_t){.ptr = swr, .size = sizeof(swr)};
#endif
    return desc;
}

/*-- Disc images: USB drive files, then flash images -------------------------*/

#define USB_MAX_FILES 32
static char usb_files[USB_MAX_FILES][13];   // 8.3 names of .ssd/.dsd files in the root
static int usb_num_files = 0;
static bool usb_scanned = false;
static FIL usb_fil;
static bool usb_fil_open = false;
static int current_image = -1;               // 0.. usb files, then flash images

extern bool msc_inquiry_complete;

static bool has_ext(const char* name, const char* ext) {
    size_t n = strlen(name), e = strlen(ext);
    if (n < e) return false;
    for (size_t i = 0; i < e; i++) {
        char c = name[n - e + i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c != ext[i]) return false;
    }
    return true;
}

// Sector reader for the WD1770: 256 bytes at `offset` of the open USB file
static bool usb_read_sector(void* ctx, uint32_t offset, uint8_t* buf) {
    (void)ctx;
    UINT n = 0;
    if (!usb_fil_open) return false;
    if (f_lseek(&usb_fil, offset) != FR_OK) return false;
    if (f_read(&usb_fil, buf, 256, &n) != FR_OK) return false;
    return n == 256;
}

static bool usb_write_sector(void* ctx, uint32_t offset, const uint8_t* buf) {
    (void)ctx;
    UINT n = 0;
    if (!usb_fil_open) return false;
    if (f_lseek(&usb_fil, offset) != FR_OK) return false;
    if (f_write(&usb_fil, buf, 256, &n) != FR_OK || n != 256) return false;
    return f_sync(&usb_fil) == FR_OK;
}

static void usb_scan(void) {
    DIR dir;
    FILINFO fno;
    usb_num_files = 0;
    if (f_opendir(&dir, "/") != FR_OK) return;
    while (usb_num_files < USB_MAX_FILES && f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
        if (fno.fattrib & AM_DIR) continue;
        if (has_ext(fno.fname, ".ssd") || has_ext(fno.fname, ".dsd")) {
            strncpy(usb_files[usb_num_files], fno.fname, 12);
            usb_files[usb_num_files][12] = 0;
            usb_num_files++;
        }
    }
    f_closedir(&dir);
    printf("USB: %d disc image(s)\n", usb_num_files);
}

static int num_images(void) { return usb_num_files + BBC_NUM_IMAGES; }

// Insert image `index` (read-only) in drive 0: USB file or flash image
static void insert_image(int index) {
    if (index < 0 || index >= num_images()) return;
    if (usb_fil_open) {
        f_close(&usb_fil);
        usb_fil_open = false;
    }
    if (index < usb_num_files) {
        const char* name = usb_files[index];
        if (f_open(&usb_fil, name, FA_READ | FA_WRITE) != FR_OK) {
            printf("USB: cannot open %s\n", name);
            return;
        }
        usb_fil_open = true;
        wd1770_insert_streamed(&state.bbc.fdc, 0, f_size(&usb_fil), has_ext(name, ".dsd") ? 2 : 1, usb_read_sector,
                               usb_write_sector, 0);
        printf("Disc inserted: %s (%u bytes)\n", name, (unsigned)f_size(&usb_fil));
    } else {
#if BBC_NUM_IMAGES > 0
        const bbc_disc_image_t* im = &bbc_disc_images[index - usb_num_files];
        bbc_insert_disc(&state.bbc, 0, (uint8_t*)im->data, im->size, im->sides, true);
        printf("Flash disc %d inserted (%u bytes)\n", index - usb_num_files, (unsigned)im->size);
#endif
    }
    current_image = index;
}

// Called every frame: once the USB drive is mounted, list its images and insert the first one
static void usb_poll(void) {
    if (usb_scanned || !msc_inquiry_complete) return;
    usb_scanned = true;
    usb_scan();
    if (usb_num_files > 0) {
        insert_image(0);
    }
}

void app_init(void) {
    bbc_desc_t desc = bbc_desc();
    bbc_init(&state.bbc, &desc);
    bbc_reset(&state.bbc);
#if BBC_NUM_IMAGES > 0
    insert_image(0);   // Flash image until a USB drive shows up
#endif
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
        // Master 128 keypad
        case HID_KEY_KEYPAD_0: return BBC_KEY_Keypad0;
        case HID_KEY_KEYPAD_1: return BBC_KEY_Keypad1;
        case HID_KEY_KEYPAD_2: return BBC_KEY_Keypad2;
        case HID_KEY_KEYPAD_3: return BBC_KEY_Keypad3;
        case HID_KEY_KEYPAD_4: return BBC_KEY_Keypad4;
        case HID_KEY_KEYPAD_5: return BBC_KEY_Keypad5;
        case HID_KEY_KEYPAD_6: return BBC_KEY_Keypad6;
        case HID_KEY_KEYPAD_7: return BBC_KEY_Keypad7;
        case HID_KEY_KEYPAD_8: return BBC_KEY_Keypad8;
        case HID_KEY_KEYPAD_9: return BBC_KEY_Keypad9;
        case HID_KEY_KEYPAD_ADD: return BBC_KEY_KeypadPlus;
        case HID_KEY_KEYPAD_SUBTRACT: return BBC_KEY_KeypadMinus;
        case HID_KEY_KEYPAD_MULTIPLY: return BBC_KEY_KeypadStar;
        case HID_KEY_KEYPAD_DIVIDE: return BBC_KEY_KeypadSlash;
        case HID_KEY_KEYPAD_ENTER: return BBC_KEY_KeypadReturn;
        case HID_KEY_KEYPAD_DECIMAL: return BBC_KEY_KeypadStop;
        case HID_KEY_KEYPAD_COMMA: return BBC_KEY_KeypadComma;
        default: return -1;
    }
}

bool hid_raw_keys_enabled(void) { return true; }

// The ASCII keyboard callbacks of hid_app.c are not used (raw keys instead)
void kbd_raw_key_down(int code) { (void)code; }
void kbd_raw_key_up(int code) { (void)code; }

// USB gamepad -> BBC analogue joystick 1: d-pad = extremes, button A = fire
void gamepad_state_update(uint8_t index, uint8_t hat_state, uint32_t button_state) {
    if (index != 0) return;
    uint16_t x = 0x8000, y = 0x8000;
    switch (hat_state) {
        case GAMEPAD_HAT_UP: y = 0xFFFF; break;
        case GAMEPAD_HAT_UP_RIGHT: y = 0xFFFF; x = 0; break;
        case GAMEPAD_HAT_RIGHT: x = 0; break;
        case GAMEPAD_HAT_DOWN_RIGHT: y = 0; x = 0; break;
        case GAMEPAD_HAT_DOWN: y = 0; break;
        case GAMEPAD_HAT_DOWN_LEFT: y = 0; x = 0xFFFF; break;
        case GAMEPAD_HAT_LEFT: x = 0xFFFF; break;
        case GAMEPAD_HAT_UP_LEFT: y = 0xFFFF; x = 0xFFFF; break;
        default: break;
    }
    bbc_set_joystick(&state.bbc, 0, x, y, (button_state & GAMEPAD_BUTTON_A) != 0);
}

void hid_raw_key_down(uint8_t keycode) {
    if (keycode == NEO_MULTIBOOT_RETURN_KEY) neo_multiboot_return();  // Pause : back to the Neo6502 firmware (multi-boot)
    if (keycode == HID_KEY_F11) {
        // Next disc image
        if (num_images() > 0) insert_image((current_image + 1) % num_images());
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

#ifdef BBC_MASTER
    printf("BBC Master 128 on Neo6502: configuring DVI\n");
#else
    printf("BBC Micro on Neo6502: configuring DVI\n");
#endif

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
        usb_poll();

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
