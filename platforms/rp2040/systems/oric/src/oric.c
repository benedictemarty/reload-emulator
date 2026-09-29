// oric.c
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

#define RGBA8(r, g, b) (0xFF000000 | (r << 16) | (g << 8) | (b))

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <pico/platform.h>
#include "pico/stdlib.h"

#include "roms/pravetz8d_roms.h"
#include "images/oric_images.h"

#include "chips/chips_common.h"
#ifdef OLIMEX_NEO6502
#include "chips/wdc65C02cpu.h"
#else
#include "chips/mos6502cpu.h"
#endif
#include "chips/mos6522via.h"
#include "chips/ay38910psg.h"
#include "chips/kbd.h"
#include "chips/mem.h"
#include "chips/clk.h"
#include "devices/oric_td.h"
#include "devices/disk2_fdd.h"
#include "devices/disk2_fdc.h"
#include "devices/oric_fdc_rom.h"
#include "systems/oric.h"

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/vreg.h"
#include "hardware/interp.h"
#include "pico/multicore.h"

#include "tmds_encode.h"

#include "common_dvi_pin_configs.h"
#include "dvi.h"
#include "dvi_serialiser.h"

#include "audio.h"

#include "tusb.h"
#include "neo_multiboot.h"

typedef struct {
    uint32_t version;
    oric_t oric;
} oric_snapshot_t;

typedef struct {
    oric_t oric;
    uint32_t frame_time_us;
    uint32_t ticks;
    // double emu_time_ms;
} state_t;

state_t __not_in_flash() state;

// Layout for host tools (SWD capture, co-simulation), in every build: offset of
// the Oric RAM in `state` (text screen at $BB80, 40 x 28), size of oric_t
#include <stddef.h>
volatile uint32_t diag_layout[2] = {offsetof(state_t, oric) + offsetof(oric_t, ram), sizeof(oric_t)};

// Audio streaming callback
static void audio_callback(const uint8_t sample, void *user_data) {
    (void)user_data;
    audio_push_sample(sample);
}

// Get oric_desc_t struct based on joystick type
oric_desc_t oric_desc(void) {
    return (oric_desc_t){
        .td_enabled = true,
        .fdc_enabled = true,
        .audio =
            {
                .callback = {.func = audio_callback},
                .sample_rate = 22050,
            },
        .roms =
            {
                .rom = {.ptr = oric_rom, .size = sizeof(oric_rom)},
                .boot_rom = {.ptr = oric_fdc_rom, .size = sizeof(oric_fdc_rom)},
            },
    };
}

void app_init(void) {
    (void)diag_layout[0];   // Kept by the linker for the host tools
    oric_desc_t desc = oric_desc();
    oric_init(&state.oric, &desc);
}

#ifdef OLIMEX_NEO6502
// TMDS bit clock 295.2 MHz
// DVDD 1.2V
#define FRAME_WIDTH  800
#define FRAME_HEIGHT 480
#define VREG_VSEL    VREG_VOLTAGE_1_20
#define DVI_TIMING   dvi_timing_800x480p_60hz
#else
// TMDS bit clock 372 MHz
// DVDD 1.3V
#define FRAME_WIDTH  960
#define FRAME_HEIGHT 544
#define VREG_VSEL    VREG_VOLTAGE_1_30
#define DVI_TIMING   dvi_timing_960x544p_60hz
#endif  // OLIMEX_NEO6502

uint32_t __not_in_flash() tmds_palette[PALETTE_SIZE * 6];
uint32_t __not_in_flash() empty_tmdsbuf[3 * FRAME_WIDTH / DVI_SYMBOLS_PER_WORD];

uint8_t __not_in_flash() scanbuf[FRAME_WIDTH];

struct dvi_inst dvi0;

void tmds_palette_init() { tmds_setup_palette24_symbols(oric_palette, tmds_palette, PALETTE_SIZE); }

void kbd_raw_key_down(int code) {
    if (code == (NEO_MULTIBOOT_RETURN_KEY | 0x100)) neo_multiboot_return();  // Pause (code HID | 0x100 : pas d'ASCII) : retour au firmware Neo6502
    if (isascii(code)) {
        if (isupper(code)) {
            code = tolower(code);
        } else if (islower(code)) {
            code = toupper(code);
        }
    }

    oric_t *sys = &state.oric;

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
            uint8_t index = code - 0x13A;
            int num_nib_images = CHIPS_ARRAY_SIZE(oric_nib_images);
            if (index < num_nib_images) {
                if (sys->fdc.valid) {
                    disk2_fdd_insert_disk(&sys->fdc.fdd[0], oric_nib_images[index]);
                }
            } else {
                index -= num_nib_images;
                if (index < CHIPS_ARRAY_SIZE(oric_wave_images)) {
                    if (sys->td.valid) {
                        oric_td_insert_tape(&sys->td, oric_wave_images[index]);
                    }
                }
            }
            break;
        }

        case 0x144:  // F11
            oric_nmi(sys);
            break;

        case 0x145:  // F12
            oric_reset(sys);
            break;

        default:
            kbd_key_down(&sys->kbd, code);
            break;
    }
}

void kbd_raw_key_up(int code) {
    if (isascii(code)) {
        if (isupper(code)) {
            code = tolower(code);
        } else if (islower(code)) {
            code = toupper(code);
        }
    }
    kbd_key_up(&state.oric.kbd, code);
}

void gamepad_state_update(uint8_t index, uint8_t hat_state, uint32_t button_state) {}

#ifdef ORIC_DIAG
// Bench tests over SWD (-DORIC_DIAG): the host writes key codes as a PC
// keyboard gives them (lower case = unshifted, '*' = SHIFT+8, 0x0D = RETURN)
// into diag_keyq and advances diag_keyq_tail; each key is held then released
// for 4 frames.
volatile uint16_t diag_keyq[256];
volatile uint32_t diag_keyq_head, diag_keyq_tail, diag_frames, diag_exec_us, diag_exec_max;

static void diag_key_service(void) {
    static int phase, timer;
    static int key;
    (void)diag_layout[0];
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

// extern void oric_render_scanline_2x(const uint32_t *pixbuf, uint32_t *scanbuf, size_t n_pix);
extern void oric_render_scanline_3x(const uint32_t *pixbuf, uint32_t *scanbuf, size_t n_pix);
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

    oric_render_scanline_3x(pixbuf, scanbuf, n_pix);
}

#define ORIC_EMPTY_LINES   ((FRAME_HEIGHT - ORIC_SCREEN_HEIGHT * 2) / 4)
#define ORIC_EMPTY_COLUMNS ((FRAME_WIDTH - ORIC_SCREEN_WIDTH * 3) / 2)

static inline void __not_in_flash_func(render_empty_scanlines)() {
    for (int y = 0; y < ORIC_EMPTY_LINES; y += 2) {
        uint32_t *tmdsbuf;
        queue_remove_blocking_u32(&dvi0.q_tmds_free, &tmdsbuf);
        copy_tmdsbuf(tmdsbuf, empty_tmdsbuf);
        queue_add_blocking_u32(&dvi0.q_tmds_valid, &tmdsbuf);

        queue_remove_blocking_u32(&dvi0.q_tmds_free, &tmdsbuf);
        copy_tmdsbuf(tmdsbuf, empty_tmdsbuf);
        queue_add_blocking_u32(&dvi0.q_tmds_valid, &tmdsbuf);
    }
}

static inline void __not_in_flash_func(render_frame)() {
    for (int y = 0; y < ORIC_SCREEN_HEIGHT; y += 2) {
        uint32_t *tmdsbuf;
        queue_remove_blocking_u32(&dvi0.q_tmds_free, &tmdsbuf);
        render_scanline((const uint32_t *)(&state.oric.fb[y * 120]), (uint32_t *)(&scanbuf[ORIC_EMPTY_COLUMNS]), 120);
        tmds_encode_palette_data((const uint32_t *)scanbuf, tmds_palette, tmdsbuf, FRAME_WIDTH, PALETTE_BITS);
        queue_add_blocking_u32(&dvi0.q_tmds_valid, &tmdsbuf);

        queue_remove_blocking_u32(&dvi0.q_tmds_free, &tmdsbuf);
        render_scanline((const uint32_t *)(&state.oric.fb[(y + 1) * 120]), (uint32_t *)(&scanbuf[ORIC_EMPTY_COLUMNS]),
                        120);
        tmds_encode_palette_data((const uint32_t *)scanbuf, tmds_palette, tmdsbuf, FRAME_WIDTH, PALETTE_BITS);
        queue_add_blocking_u32(&dvi0.q_tmds_valid, &tmdsbuf);
    }
}

void __not_in_flash_func(core1_main()) {
    audio_init(_AUDIO_PIN, 22050);

    dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
    dvi_start(&dvi0);

    while (1) {
        render_empty_scanlines();
        render_frame();
        render_empty_scanlines();
    }

    __builtin_unreachable();
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
    // Dedicated spin locks for the DVI queues: next_striped_spin_lock_num() shares
    // 16-23 with the other SDK users, and core 1 could then wait for core 0 with
    // interrupts masked (a late scanline; found by the Neo6502Trinity project)
    dvi_init(&dvi0, spin_lock_claim_unused(true), spin_lock_claim_unused(true));

    tmds_palette_init();
    tmds_encode_palette_data((const uint32_t *)scanbuf, tmds_palette, empty_tmdsbuf, FRAME_WIDTH, PALETTE_BITS);

    printf("Core 1 start\n");
    hw_set_bits(&bus_ctrl_hw->priority, BUSCTRL_BUS_PRIORITY_PROC1_BITS);
    multicore_launch_core1(core1_main);

    app_init();

    uint32_t deadline = time_us_32();
    while (1) {
        uint32_t start_time_in_micros = time_us_32();

        uint32_t num_ticks = 19968;
        for (uint32_t ticks = 0; ticks < num_ticks; ticks++) {
            oric_tick(&state.oric);
        }

        oric_screen_update(&state.oric);
        kbd_update(&state.oric.kbd, 19968);
        tuh_task();
#ifdef ORIC_DIAG
        diag_key_service();
#endif

        uint32_t end_time_in_micros = time_us_32();
        uint32_t execution_time = end_time_in_micros - start_time_in_micros;
        (void)execution_time;
#ifdef ORIC_DIAG
        diag_exec_us += execution_time;
        if (execution_time > diag_exec_max) diag_exec_max = execution_time;
#endif
        // printf("%d us\n", execution_time);

        // Absolute deadlines: a frame that ran late is caught up by the next
        // ones (the average speed stays exact); more than 5 frames behind, the
        // schedule restarts from now instead of racing
        deadline += 19968;
        int32_t ahead = (int32_t)(deadline - time_us_32());
        if (ahead > 0) {
            sleep_us((uint64_t)ahead);
        } else if (ahead < -5 * (int32_t)19968) {
            deadline = time_us_32();
        }
    }

    __builtin_unreachable();
}
