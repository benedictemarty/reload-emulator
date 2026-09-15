// bbc.c
//
// BBC Micro Model B emulator, PC platform (sokol).
//
// Keys: positional PC -> BBC mapping, F12 = BREAK, F10 = BBC f0, F1-F9 = f1-f9.
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

#define RGBA8(b, g, r) (0xFF000000 | (r << 16) | (g << 8) | (b))

#include <stdlib.h>

#include "roms/bbc_roms.h"

#include "chips/chips_common.h"
#include "common.h"
#include "chips/mos6502cpu.h"
#include "chips/mos6522via.h"
#include "chips/mem.h"
#include "chips/clk.h"
#include "devices/wd1770.h"
#include "systems/bbc.h"
#include "systems/bbc_keys.h"

typedef struct {
    bbc_t bbc;
    uint32_t frame_time_us;
    uint32_t ticks;
    double emu_time_ms;
} state_t;

static state_t state;

#define BORDER_TOP (8)
#define BORDER_LEFT (8)
#define BORDER_RIGHT (8)
#define BORDER_BOTTOM (16)

// Audio streaming callback
static void audio_callback(const uint8_t sample, void *user_data) {
    (void)user_data;

    static float samples[1024];
    static int sample_index = 0;
    samples[sample_index++] = ((float)sample - 128.0f) / 128.0f;
    if (sample_index == 1024) {
        saudio_push(samples, 1024);
        sample_index = 0;
    }
}

bbc_desc_t bbc_desc(void) {
    bbc_desc_t desc = {
        .audio =
            {
                .callback = {.func = audio_callback},
                .sample_rate = 44100,
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

// Unpacked framebuffer: one byte per pixel, 640x512 (lines doubled for a 5:4 aspect)
#define BBC_OUT_HEIGHT (BBC_SCREEN_HEIGHT * 2)
static uint8_t bbc_frame_buffer[BBC_SCREEN_WIDTH * BBC_OUT_HEIGHT];

static void bbc_update_frame_buffer(bbc_t* sys) {
    for (int row = 0; row < BBC_SCREEN_HEIGHT; row++) {
        const uint8_t* src_row = &sys->fb[row * (BBC_SCREEN_WIDTH / 2)];
        uint8_t* dst_row = &bbc_frame_buffer[row * 2 * BBC_SCREEN_WIDTH];
        for (int col = 0; col < BBC_SCREEN_WIDTH / 2; col++) {
            uint8_t pixel = src_row[col];
            dst_row[col * 2] = (pixel >> 4) & 0x07;
            dst_row[col * 2 + 1] = pixel & 0x07;
        }
        memcpy(dst_row + BBC_SCREEN_WIDTH, dst_row, BBC_SCREEN_WIDTH);
    }
}

chips_display_info_t bbc_display_info(bbc_t* sys) {
    const chips_display_info_t res = {
        .frame = {
            .dim = {
                .width = BBC_SCREEN_WIDTH,
                .height = BBC_OUT_HEIGHT,
            },
            .bytes_per_pixel = 1,
            .buffer = {
                .ptr = sys ? bbc_frame_buffer : 0,
                .size = sizeof(bbc_frame_buffer),
            }
        },
        .screen = {
            .x = 0,
            .y = 0,
            .width = BBC_SCREEN_WIDTH,
            .height = BBC_OUT_HEIGHT,
        },
        .palette = {
            .ptr = sys ? (void*)bbc_palette : 0,
            .size = sizeof(bbc_palette),
        }
    };
    return res;
}

void app_init(void) {
    saudio_setup(&(saudio_desc){
        .logger.func = slog_func,
    });

    bbc_desc_t desc = bbc_desc();
    bbc_init(&state.bbc, &desc);
    bbc_reset(&state.bbc);
    gfx_init(&(gfx_desc_t){
        .disable_speaker_icon = sargs_exists("disable-speaker-icon"),
        .border = {
            .left = BORDER_LEFT,
            .right = BORDER_RIGHT,
            .top = BORDER_TOP,
            .bottom = BORDER_BOTTOM,
        },
        .display_info = bbc_display_info(&state.bbc),
    });
    clock_init();
    prof_init();
}

static void draw_status_bar(void);

void app_frame(void) {
    state.frame_time_us = clock_frame_time();
    const uint64_t emu_start_time = stm_now();
    state.ticks = bbc_exec(&state.bbc, state.frame_time_us);
    state.emu_time_ms = stm_ms(stm_since(emu_start_time));
    draw_status_bar();
    bbc_update_frame_buffer(&state.bbc);
    gfx_draw(bbc_display_info(&state.bbc));
}

// Positional mapping from sokol key codes to BBC keys
static int bbc_key_from_keycode(sapp_keycode k) {
    switch (k) {
        case SAPP_KEYCODE_A: return BBC_KEY_A;
        case SAPP_KEYCODE_B: return BBC_KEY_B;
        case SAPP_KEYCODE_C: return BBC_KEY_C;
        case SAPP_KEYCODE_D: return BBC_KEY_D;
        case SAPP_KEYCODE_E: return BBC_KEY_E;
        case SAPP_KEYCODE_F: return BBC_KEY_F;
        case SAPP_KEYCODE_G: return BBC_KEY_G;
        case SAPP_KEYCODE_H: return BBC_KEY_H;
        case SAPP_KEYCODE_I: return BBC_KEY_I;
        case SAPP_KEYCODE_J: return BBC_KEY_J;
        case SAPP_KEYCODE_K: return BBC_KEY_K;
        case SAPP_KEYCODE_L: return BBC_KEY_L;
        case SAPP_KEYCODE_M: return BBC_KEY_M;
        case SAPP_KEYCODE_N: return BBC_KEY_N;
        case SAPP_KEYCODE_O: return BBC_KEY_O;
        case SAPP_KEYCODE_P: return BBC_KEY_P;
        case SAPP_KEYCODE_Q: return BBC_KEY_Q;
        case SAPP_KEYCODE_R: return BBC_KEY_R;
        case SAPP_KEYCODE_S: return BBC_KEY_S;
        case SAPP_KEYCODE_T: return BBC_KEY_T;
        case SAPP_KEYCODE_U: return BBC_KEY_U;
        case SAPP_KEYCODE_V: return BBC_KEY_V;
        case SAPP_KEYCODE_W: return BBC_KEY_W;
        case SAPP_KEYCODE_X: return BBC_KEY_X;
        case SAPP_KEYCODE_Y: return BBC_KEY_Y;
        case SAPP_KEYCODE_Z: return BBC_KEY_Z;
        case SAPP_KEYCODE_0: return BBC_KEY_0;
        case SAPP_KEYCODE_1: return BBC_KEY_1;
        case SAPP_KEYCODE_2: return BBC_KEY_2;
        case SAPP_KEYCODE_3: return BBC_KEY_3;
        case SAPP_KEYCODE_4: return BBC_KEY_4;
        case SAPP_KEYCODE_5: return BBC_KEY_5;
        case SAPP_KEYCODE_6: return BBC_KEY_6;
        case SAPP_KEYCODE_7: return BBC_KEY_7;
        case SAPP_KEYCODE_8: return BBC_KEY_8;
        case SAPP_KEYCODE_9: return BBC_KEY_9;
        case SAPP_KEYCODE_SPACE: return BBC_KEY_Space;
        case SAPP_KEYCODE_ENTER: return BBC_KEY_Return;
        case SAPP_KEYCODE_BACKSPACE: return BBC_KEY_Delete;
        case SAPP_KEYCODE_DELETE: return BBC_KEY_Delete;
        case SAPP_KEYCODE_ESCAPE: return BBC_KEY_Escape;
        case SAPP_KEYCODE_TAB: return BBC_KEY_Tab;
        case SAPP_KEYCODE_CAPS_LOCK: return BBC_KEY_CapsLock;
        case SAPP_KEYCODE_LEFT_SHIFT: case SAPP_KEYCODE_RIGHT_SHIFT: return BBC_KEY_Shift;
        case SAPP_KEYCODE_LEFT_CONTROL: case SAPP_KEYCODE_RIGHT_CONTROL: return BBC_KEY_Ctrl;
        case SAPP_KEYCODE_LEFT: return BBC_KEY_Left;
        case SAPP_KEYCODE_RIGHT: return BBC_KEY_Right;
        case SAPP_KEYCODE_UP: return BBC_KEY_Up;
        case SAPP_KEYCODE_DOWN: return BBC_KEY_Down;
        case SAPP_KEYCODE_MINUS: return BBC_KEY_Minus;
        case SAPP_KEYCODE_EQUAL: return BBC_KEY_Caret;
        case SAPP_KEYCODE_LEFT_BRACKET: return BBC_KEY_LeftSquareBracket;
        case SAPP_KEYCODE_RIGHT_BRACKET: return BBC_KEY_RightSquareBracket;
        case SAPP_KEYCODE_SEMICOLON: return BBC_KEY_Semicolon;
        case SAPP_KEYCODE_APOSTROPHE: return BBC_KEY_Colon;
        case SAPP_KEYCODE_BACKSLASH: return BBC_KEY_Backslash;
        case SAPP_KEYCODE_GRAVE_ACCENT: return BBC_KEY_At;
        case SAPP_KEYCODE_COMMA: return BBC_KEY_Comma;
        case SAPP_KEYCODE_PERIOD: return BBC_KEY_Stop;
        case SAPP_KEYCODE_SLASH: return BBC_KEY_Slash;
        case SAPP_KEYCODE_END: return BBC_KEY_Copy;
        case SAPP_KEYCODE_HOME: return BBC_KEY_Underline;
        case SAPP_KEYCODE_F1: return BBC_KEY_f1;
        case SAPP_KEYCODE_F2: return BBC_KEY_f2;
        case SAPP_KEYCODE_F3: return BBC_KEY_f3;
        case SAPP_KEYCODE_F4: return BBC_KEY_f4;
        case SAPP_KEYCODE_F5: return BBC_KEY_f5;
        case SAPP_KEYCODE_F6: return BBC_KEY_f6;
        case SAPP_KEYCODE_F7: return BBC_KEY_f7;
        case SAPP_KEYCODE_F8: return BBC_KEY_f8;
        case SAPP_KEYCODE_F9: return BBC_KEY_f9;
        case SAPP_KEYCODE_F10: return BBC_KEY_f0;
        case SAPP_KEYCODE_F12: return BBC_KEY_Break;
        default: return -1;
    }
}

void app_input(const sapp_event* event) {
    switch (event->type) {
        case SAPP_EVENTTYPE_KEY_DOWN:
        case SAPP_EVENTTYPE_KEY_UP: {
            int key = bbc_key_from_keycode(event->key_code);
            if (key >= 0) {
                if (event->type == SAPP_EVENTTYPE_KEY_DOWN) {
                    bbc_key_down(&state.bbc, (uint8_t)key);
                } else {
                    bbc_key_up(&state.bbc, (uint8_t)key);
                }
            }
            break;
        }
        default:
            break;
    }
}

void app_cleanup(void) {
    bbc_discard(&state.bbc);
    saudio_shutdown();
    gfx_shutdown();
    sargs_shutdown();
}

static void draw_status_bar(void) {
    prof_push(PROF_EMU, (float)state.emu_time_ms);
    prof_stats_t emu_stats = prof_stats(PROF_EMU);
    const float w = sapp_widthf();
    const float h = sapp_heightf();
    sdtx_canvas(w, h);
    sdtx_color3b(255, 255, 255);
    sdtx_pos(1.0f, (h / 8.0f) - 1.5f);
    sdtx_printf("frame:%.2fms emu:%.2fms (min:%.2fms max:%.2fms) ticks:%d", (float)state.frame_time_us * 0.001f, emu_stats.avg_val, emu_stats.min_val, emu_stats.max_val, state.ticks);
}

sapp_desc sokol_main(int argc, char* argv[]) {
    sargs_setup(&(sargs_desc){
        .argc=argc,
        .argv=argv,
        .buf_size = 512 * 1024,
    });
    const chips_display_info_t info = bbc_display_info(0);
    return (sapp_desc) {
        .init_cb = app_init,
        .frame_cb = app_frame,
        .event_cb = app_input,
        .cleanup_cb = app_cleanup,
        .width = 2 * (info.screen.width + BORDER_LEFT + BORDER_RIGHT),
        .height = 2 * (info.screen.height + BORDER_TOP + BORDER_BOTTOM),
        .window_title = "BBC Micro",
        .icon.sokol_default = true,
        .enable_dragndrop = true,
        .html5_bubble_mouse_events = true,
        .html5_update_document_title = true,
        .logger.func = slog_func,
    };
}
