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
// Core 1: DVI 960x544 (or 800x480) @ 60 Hz, BBC 640x256 centred, lines
//         doubled by PicoDVI: 960x544 at 372 MHz shows the 256 lines (default),
//         800x480 at 295.2 MHz (-DBBC_VIDEO_480) crops 8 lines top and bottom.
//         Each line is split into 3 bit planes and encoded by the 1 bpp TMDS
//         encoder (the BBC colours are 0x00/0xFF per channel).
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
#define BBC_HAVE_IMAGES 1   // (the preprocessor cannot evaluate BBC_NUM_IMAGES)
#define BBC_NUM_IMAGES ((int)(sizeof(bbc_disc_images) / sizeof(bbc_disc_images[0])))
#else
#define BBC_NUM_IMAGES 0
#endif

// Code run every emulated cycle goes to RAM: from XIP flash it thrashes the
// 16 KB cache (the Model B hot path alone is larger than that)
#define BBC_HOT      __attribute__((section(".time_critical.bbc")))
// Core 0 only captures each display line; core 1 draws it (no framebuffer)
#define BBC_DEFER_RENDER 1
// At most one byte -> pixels table built every 8 lines (core 1 line budget)
#define BBC_LUT_THROTTLE 8
#define CHIPS_HOT    __attribute__((section(".time_critical.bbc")))
#define WDC65C02_HOT __attribute__((section(".time_critical.bbc")))
#include <stddef.h>
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
#include "devices/tube.h"
#define W65C02_NO_MACROS
#include "chips/w65c02cpu.h"

#ifdef OLIMEX_NEO6502
// Bus cycle of the real 65C02 inlined in bbc_tick(), with direct SIO register
// accesses (same sequence and delays as wdc65C02cpu.h, without the calls)
#include "hardware/structs/sio.h"
#define BUS_NOP6() __asm volatile("nop\nnop\nnop\nnop\nnop\nnop\n")

static inline __attribute__((always_inline)) void bus_tick(wdc6502cpu_t *c) {
    sio_hw->gpio_clr = 1u << _CLOCK_PIN;
    sio_hw->gpio_set = 1u << _OE3_PIN;   // End of the data of a read cycle, after PHI2 fell (see bus_set_data)
    sio_hw->gpio_oe_clr = _GPIO_MASK;
    sio_hw->gpio_clr = 1u << _OE1_PIN;
    BUS_NOP6();
    uint32_t lo = sio_hw->gpio_in & 0xFF;
    sio_hw->gpio_set = 1u << _OE1_PIN;
    sio_hw->gpio_clr = 1u << _OE2_PIN;
    BUS_NOP6();
    uint32_t hi = sio_hw->gpio_in & 0xFF;
    sio_hw->gpio_set = 1u << _OE2_PIN;
    c->addr = (uint16_t)(lo | (hi << 8));
    c->rw = (sio_hw->gpio_in >> _RW_PIN) & 1;
    sio_hw->gpio_set = 1u << _CLOCK_PIN;
}

static inline __attribute__((always_inline)) uint8_t bus_get_data(void) {
    sio_hw->gpio_oe_clr = _GPIO_MASK;
    sio_hw->gpio_clr = 1u << _OE3_PIN;
    BUS_NOP6();
    uint8_t data = (uint8_t)(sio_hw->gpio_in & 0xFF);
    sio_hw->gpio_set = 1u << _OE3_PIN;
    return data;
}

// Read cycle: the data stays driven (OE3 low) until PHI2 falls at the next
// bus_tick, where the 65C02 latches it. A brief OE3 pulse left the byte on the
// bus capacitance only, and it leaked away when the emulation paused with PHI2
// high (end of a frame: milliseconds). OE3 goes high right after PHI2 falls,
// before the 65C02 changes R/W for the next cycle (tADS ~30 ns).
static inline __attribute__((always_inline)) void bus_set_data(uint8_t data) {
    sio_hw->gpio_oe_set = _GPIO_MASK;
    sio_hw->gpio_togl = (sio_hw->gpio_out ^ data) & _GPIO_MASK;
    sio_hw->gpio_clr = 1u << _OE3_PIN;
}

#ifdef BBC_BUS_PIO
// Option (-DBBC_BUS_PIO): measured 158-162 M0+ cycles per emulated cycle, as
// the bit-banged bus (161): the core's own work is the limit, not the bus.
// Bus cycles sequenced by PIO1 (bus6502.pio): the core only exchanges FIFO
// words, the transceiver delays overlap with the emulation work. Exactly one
// command per 65C02 cycle: an access nobody answered is closed at the next one.
#include "hardware/pio.h"
#include "bus6502.pio.h"
#define BUS_PIO   pio1
#define BUS_SM    0
static bool bus_open;         // Address taken, command not sent yet
static bool bus_rw_cycle;     // 65C02 read cycle
static uint8_t bus_wdata;     // Data of the last write cycle

static inline __attribute__((always_inline)) uint32_t bus_rx(void) {
    while (BUS_PIO->fstat & (1u << (PIO_FSTAT_RXEMPTY_LSB + BUS_SM))) {
    }
    return BUS_PIO->rxf[BUS_SM];
}

static inline __attribute__((always_inline)) void bus_close(void) {
    if (bus_open) {
        bus_open = false;
        if (bus_rw_cycle) {
            BUS_PIO->txf[BUS_SM] = 0x1FF;   // Open bus
        } else {
            BUS_PIO->txf[BUS_SM] = 0;
            bus_wdata = (uint8_t)bus_rx();
        }
    }
}

static inline __attribute__((always_inline)) void bus_tick_pio(wdc6502cpu_t *c) {
    bus_close();
    uint32_t v = bus_rx();
    c->addr = (uint16_t)(((v >> 12) & 0xFF) | ((v & 0xFF) << 8));
    c->rw = (v >> _RW_PIN) & 1;
    bus_rw_cycle = c->rw;
    bus_open = true;
}

static inline __attribute__((always_inline)) uint8_t bus_get_data_pio(void) {
    if (bus_open) {
        bus_open = false;
        BUS_PIO->txf[BUS_SM] = 0;
        bus_wdata = (uint8_t)bus_rx();
    }
    return bus_wdata;
}

static inline __attribute__((always_inline)) void bus_set_data_pio(uint8_t data) {
    if (bus_open) {
        bus_open = false;
        BUS_PIO->txf[BUS_SM] = 0x100u | data;
    }
}

// Hand GPIO 0-10 and PHI2 over to the state machine (after wdc65C02cpu_init)
static void bus_pio_start(void) {
    uint offset = pio_add_program(BUS_PIO, &bus6502_program);
    pio_sm_config cfg = bus6502_program_get_default_config(offset);
    sm_config_set_in_pins(&cfg, 0);
    sm_config_set_out_pins(&cfg, 0, 8);
    sm_config_set_set_pins(&cfg, _CLOCK_PIN, 1);
    sm_config_set_sideset_pins(&cfg, _OE1_PIN);
    sm_config_set_in_shift(&cfg, false, false, 32);   // Shift left, no autopush
    sm_config_set_out_shift(&cfg, true, false, 32);   // Shift right, no autopull
    sm_config_set_clkdiv(&cfg, 1.0f);
    pio_sm_set_pins_with_mask(BUS_PIO, BUS_SM, (1u << _OE1_PIN) | (1u << _OE2_PIN) | (1u << _OE3_PIN) | (1u << _CLOCK_PIN),
                              (1u << _OE1_PIN) | (1u << _OE2_PIN) | (1u << _OE3_PIN) | (1u << _CLOCK_PIN));
    pio_sm_set_pindirs_with_mask(BUS_PIO, BUS_SM, (1u << _OE1_PIN) | (1u << _OE2_PIN) | (1u << _OE3_PIN) | (1u << _CLOCK_PIN),
                                 0xFFu | (1u << _OE1_PIN) | (1u << _OE2_PIN) | (1u << _OE3_PIN) | (1u << _CLOCK_PIN));
    for (uint pin = 0; pin <= _OE3_PIN; pin++) pio_gpio_init(BUS_PIO, pin);
    pio_gpio_init(BUS_PIO, _CLOCK_PIN);
    BUS_PIO->input_sync_bypass |= 0xFFFu;   // GPIO 0-11 sampled directly (stable after the delays)
    pio_sm_init(BUS_PIO, BUS_SM, offset + bus6502_offset_entry, &cfg);
    pio_sm_set_enabled(BUS_PIO, BUS_SM, true);
}

#define bus_tick     bus_tick_pio
#define bus_get_data bus_get_data_pio
#define bus_set_data bus_set_data_pio
#endif

#undef MOS6502CPU_TICK
#undef MOS6502CPU_GET_ADDR
#undef MOS6502CPU_GET_DATA
#undef MOS6502CPU_SET_DATA
#define MOS6502CPU_TICK(c)           bus_tick(c)
#define MOS6502CPU_GET_ADDR(c)       ((c)->addr)
#define MOS6502CPU_GET_DATA(c)       bus_get_data()
#define MOS6502CPU_SET_DATA(c, data) bus_set_data(data)
#endif

#if defined(BBC_DIAG) && defined(BBC_PROFILE)
// Cycle counts of the bbc_tick blocks (build with -DBBC_PROFILE: the hooks slow the M0+ down) (SysTick counts CPU cycles down, 24 bits)
#include "hardware/structs/systick.h"
volatile uint32_t prof_cycles[8], prof_calls[8];
#define BBC_PROF_ENTER(i) uint32_t _prof_t##i = systick_hw->cvr
#define BBC_PROF_EXIT(i) do { prof_cycles[i] += (_prof_t##i - systick_hw->cvr) & 0xFFFFFF; prof_calls[i]++; } while (0)
#endif
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

#ifdef BBC_DIAG
// Diagnostic overlay (bottom of the screen, hex groups): HID keys received,
// last HID code, frame time (us), system VIA IFR, IER, IC32, CPU address,
// pressed matrix columns, frame counter
volatile uint32_t diag_bench[4];
volatile uint32_t diag_run_us;   // Time spent emulating (the rest of the loop: USB, keys)
// On-screen line off by default (it overruns the core 1 line budget): the
// values are read over SWD (tools/carte/carte.py)
volatile bool diag_overlay = false;
static volatile uint32_t diag_keys, diag_last_key, diag_late, diag_line_max, diag_line_sum, diag_line_count;
static char __not_in_flash() diag_text[48];
static uint8_t __not_in_flash() diag_font[16][5] = {
    {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7}, {5, 5, 7, 1, 1}, {7, 4, 7, 1, 7},
    {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1}, {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7}, {7, 5, 7, 5, 5}, {6, 5, 6, 5, 6},
    {7, 4, 4, 4, 7}, {6, 5, 5, 5, 6}, {7, 4, 6, 4, 7}, {7, 4, 6, 4, 4},
};

static char *diag_hex(char *p, uint32_t v, int digits) {
    for (int i = digits - 1; i >= 0; i--) *p++ = (char)((v >> (i * 4)) & 0xF);
    *p++ = ' ';
    return p;
}

static void diag_update(uint32_t frame) {
    uint8_t cols = 0;
    for (int i = 0; i < 16; i++) cols |= state.bbc.key_cols[i];
    char *p = diag_text;
    p = diag_hex(p, diag_keys, 2);
    p = diag_hex(p, diag_last_key, 2);
    p = diag_hex(p, state.frame_time_us, 5);
    p = diag_hex(p, state.bbc.sysvia.intr.ifr, 2);
    p = diag_hex(p, state.bbc.sysvia.intr.ier, 2);
    p = diag_hex(p, state.bbc.ic32, 2);
    p = diag_hex(p, state.bbc.cpu.addr, 4);
    p = diag_hex(p, cols, 2);
    p = diag_hex(p, frame, 4);
    p = diag_hex(p, diag_late, 4);
    *p = (char)0xFF;
}

#endif

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
#ifdef BBC_HAVE_IMAGES
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
#ifdef BBC_HAVE_IMAGES
    insert_image(0);   // Flash image until a USB drive shows up
#endif
}

#ifdef BBC_VIDEO_480
// TMDS bit clock 295.2 MHz, DVDD 1.2V (Neo6502 timing shared with the Oric build):
// 240 of the 256 BBC lines fit, 8 are cropped at the top and at the bottom
#define FRAME_WIDTH  800
#define FRAME_HEIGHT 480
#define VREG_VSEL    VREG_VOLTAGE_1_20
#define DVI_TIMING   dvi_timing_800x480p_60hz
#define BBC_DISPLAY_TOP 8
#else
// TMDS bit clock (and system clock) 372 MHz, DVDD 1.3V (reload-emulator's
// setting for the other RP2040 boards): 26 % more CPU for the 2 MHz bus, and
// the 256 BBC lines all fit (8 blank lines above and below)
#define FRAME_WIDTH  960
#define FRAME_HEIGHT 544
#define VREG_VSEL    VREG_VOLTAGE_1_30
#define DVI_TIMING   dvi_timing_960x544p_60hz
#define BBC_DISPLAY_TOP (-8)
#endif
// First BBC line shown (lines BBC_DISPLAY_TOP .. BBC_DISPLAY_TOP + FRAME_HEIGHT / 2 - 1,
// one TMDS buffer each: PicoDVI shows every buffer on two output lines, DVI_VERTICAL_REPEAT = 2)
#define BBC_DISPLAY_LINES (FRAME_HEIGHT / 2)
#define BBC_EMPTY_COLUMNS ((FRAME_WIDTH - BBC_SCREEN_WIDTH) / 2)

// The 8 BBC colours only use 0x00/0xFF per channel: each TMDS lane is a 1 bpp
// image, encoded by PicoDVI's fast 1 bpp encoder (2.125 cycles per pixel).
// Bit planes of one output line, LSB = leftmost pixel; bit n of a palette
// index is plane n (0 red, 1 green, 2 blue).
#define PLANE_WORDS (FRAME_WIDTH / 32)
static uint32_t __not_in_flash() planes[3][PLANE_WORDS + 1];

struct dvi_inst dvi0;

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

// HID reports are only serviced once per emulated frame: a quick tap can come
// in as key down + key up in the same tuh_task() call, and would then be seen
// by the BBC for zero cycles. Releases are deferred until the key has been
// held for KEY_MIN_HOLD_FRAMES emulated frames.
#define KEY_MIN_HOLD_FRAMES 2
static uint32_t emu_frames;
static uint32_t key_down_frame[128];
static bool key_release_pending[128];

#ifdef BBC_DIAG
// Remote typing for bench tests over SWD: the host writes BBC key codes into
// diag_keyq (bit 7 = with SHIFT, 0x7F = BREAK) and advances diag_keyq_tail; each key is held
// then released for DIAG_KEY_FRAMES emulated frames
#define DIAG_KEY_FRAMES 4
volatile uint8_t diag_keyq[256];
volatile uint32_t diag_keyq_head, diag_keyq_tail;
// Layout for the host-side screen capture (RAM and captured lines of bbc_t)
volatile uint32_t diag_layout[4] = {offsetof(bbc_t, ram), offsetof(bbc_t, lines), sizeof(bbc_line_t), offsetof(bbc_t, crtc_reg)};

static void diag_key_service(void) {
    static int phase, timer;
    (void)diag_layout[0];   // Keep the layout table for the host tools
    static uint8_t key;
    if (timer > 0) {
        timer--;
        return;
    }
    // Code 0x7F (no such matrix position) = BREAK
    uint8_t k = ((key & 0x7F) == 0x7F) ? BBC_KEY_Break : (key & 0x7F);
    if (phase == 1) {
        bbc_key_up(&state.bbc, k);
        if (key & 0x80) bbc_key_up(&state.bbc, BBC_KEY_Shift);
        phase = 2;
        timer = DIAG_KEY_FRAMES;
    } else if (diag_keyq_head != diag_keyq_tail) {
        key = diag_keyq[diag_keyq_head & 255];
        diag_keyq_head++;
        k = ((key & 0x7F) == 0x7F) ? BBC_KEY_Break : (key & 0x7F);
        if (key & 0x80) bbc_key_down(&state.bbc, BBC_KEY_Shift);
        bbc_key_down(&state.bbc, k);
        phase = 1;
        timer = DIAG_KEY_FRAMES;
    } else {
        phase = 0;
    }
}
#endif

static void release_pending_keys(void) {
    for (int k = 0; k < 128; k++) {
        if (key_release_pending[k] && emu_frames - key_down_frame[k] >= KEY_MIN_HOLD_FRAMES) {
            key_release_pending[k] = false;
            bbc_key_up(&state.bbc, (uint8_t)k);
        }
    }
}

void hid_raw_key_down(uint8_t keycode) {
#ifdef BBC_DIAG
    diag_keys++;
    diag_last_key = keycode;
#endif
    if (keycode == NEO_MULTIBOOT_RETURN_KEY) neo_multiboot_return();  // Pause : back to the Neo6502 firmware (multi-boot)
    if (keycode == HID_KEY_F11) {
        // Next disc image
        if (num_images() > 0) insert_image((current_image + 1) % num_images());
        return;
    }
    int key = bbc_key_from_hid(keycode);
    if (key >= 0) {
        key_release_pending[key & 0x7F] = false;
        key_down_frame[key & 0x7F] = emu_frames;
        bbc_key_down(&state.bbc, (uint8_t)key);
    }
}

void hid_raw_key_up(uint8_t keycode) {
    int key = bbc_key_from_hid(keycode);
    if (key >= 0) {
        if (emu_frames - key_down_frame[key & 0x7F] < KEY_MIN_HOLD_FRAMES) {
            key_release_pending[key & 0x7F] = true;
        } else {
            bbc_key_up(&state.bbc, (uint8_t)key);
        }
    }
}

/*-- Core 1: DVI -------------------------------------------------------------*/

#ifdef BBC_DIAG
// Draw font row `row` of the overlay into the bit planes (white on black)
static void __not_in_flash_func(diag_draw)(int row) {
    for (int ch = 0; ch < 3; ch++) {
        uint16_t *h = (uint16_t *)planes[ch] + BBC_EMPTY_COLUMNS / 16;
        for (int i = 0; i < 25; i++) h[i] = 0;
    }
    for (int c = 0; c < (int)sizeof(diag_text) && diag_text[c] != (char)0xFF; c++) {
        uint8_t ch = (uint8_t)diag_text[c];
        if (ch == ' ') continue;
        uint8_t bits = diag_font[ch & 0xF][row];
        for (int b = 0; b < 3; b++) {
            if (bits & (4 >> b)) {
                int p = BBC_EMPTY_COLUMNS + c * 8 + b * 2;   // Even: both pixels in the same word
                uint32_t m = 3u << (p & 31);
                planes[0][p >> 5] |= m;
                planes[1][p >> 5] |= m;
                planes[2][p >> 5] |= m;
            }
        }
    }
}
#endif

// Line renderer state of core 1 (byte -> pixels table of the last palette)
static bbc_lut_t __not_in_flash() core1_lut;

// One TMDS buffer per BBC line: PicoDVI shows each buffer on two output lines
// (DVI_VERTICAL_REPEAT = 2), so a line must be ready every two output lines
// (2 x 29.7 us at 960x544, 2 x 31.7 us at 800x480)
static inline void __not_in_flash_func(render_frame)() {
    for (int y = 0; y < BBC_DISPLAY_LINES; y++) {
        int src_line = BBC_DISPLAY_TOP + y;
        uint32_t *tmdsbuf;
        queue_remove_blocking_u32(&dvi0.q_tmds_free, &tmdsbuf);
#ifdef BBC_DIAG
        uint32_t t0 = time_us_32();
#endif
        uint8_t *pp[3] = {(uint8_t *)planes[0] + BBC_EMPTY_COLUMNS / 8, (uint8_t *)planes[1] + BBC_EMPTY_COLUMNS / 8,
                          (uint8_t *)planes[2] + BBC_EMPTY_COLUMNS / 8};
        if (src_line >= 0 && src_line < BBC_SCREEN_HEIGHT) {
            bbc_render_line(&state.bbc, &state.bbc.lines[src_line], &core1_lut, pp);
        } else {
            for (int ch = 0; ch < 3; ch++) memset(pp[ch], 0, BBC_SCREEN_WIDTH / 8);
        }
#ifdef BBC_DIAG
        if (diag_overlay && y >= BBC_DISPLAY_LINES - 6 && y < BBC_DISPLAY_LINES - 1) {
            diag_draw(y - (BBC_DISPLAY_LINES - 6));
        }
#endif
        // TMDS lanes: 0 blue, 1 green, 2 red
        tmds_encode_1bpp(planes[2], tmdsbuf, FRAME_WIDTH);
        tmds_encode_1bpp(planes[1], tmdsbuf + FRAME_WIDTH / DVI_SYMBOLS_PER_WORD, FRAME_WIDTH);
        tmds_encode_1bpp(planes[0], tmdsbuf + 2 * FRAME_WIDTH / DVI_SYMBOLS_PER_WORD, FRAME_WIDTH);
#ifdef BBC_DIAG
        uint32_t dt = time_us_32() - t0;
        if (dt > diag_line_max) diag_line_max = dt;
        diag_line_sum += dt;
        diag_line_count++;
#endif
        queue_add_blocking_u32(&dvi0.q_tmds_valid, &tmdsbuf);
#ifdef BBC_DIAG
        if (dvi0.late_scanline_ctr) diag_late++;
#endif
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

static void __no_inline_not_in_flash_func(run_ticks)(uint32_t n) {
    for (uint32_t ticks = 0; ticks < n; ticks += 4) {
        bbc_tick4(&state.bbc);
    }
}

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

    memset(planes, 0, sizeof(planes));

    printf("Core 1 start\n");
    // DVI DMA first: the core 0 bus traffic (emulated memory, XIP) must not starve the TMDS stream
    hw_set_bits(&bus_ctrl_hw->priority, BUSCTRL_BUS_PRIORITY_PROC1_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS | BUSCTRL_BUS_PRIORITY_DMA_W_BITS);
    multicore_launch_core1(core1_main);

#if defined(BBC_DIAG) && defined(BBC_PROFILE)
    systick_hw->rvr = 0xFFFFFF;
    systick_hw->csr = 5;   // Enabled, processor clock
#endif
    app_init();
#ifdef BBC_DIAG
    {
        // Bus micro-benchmarks, 40 000 iterations each (cycles = us * 295.2 / 40000)
        extern volatile uint32_t diag_bench[4];
        wdc6502cpu_t c;
        uint32_t t = time_us_32();
        for (int i = 0; i < 40000; i++) wdc65C02cpu_tick(&c);
        diag_bench[0] = time_us_32() - t;
        t = time_us_32();
        for (int i = 0; i < 40000; i++) {
            wdc65C02cpu_tick(&c);
            if (c.rw) wdc65C02cpu_set_data(state.bbc.ram[c.addr & 0x7FFF]);
            else state.bbc.ram[c.addr & 0x7FFF] = wdc65C02cpu_get_data();
        }
        diag_bench[1] = time_us_32() - t;
#ifndef BBC_BUS_PIO
        t = time_us_32();
        for (int i = 0; i < 40000; i++) bbc_tick(&state.bbc);
        diag_bench[2] = time_us_32() - t;
#endif
        bbc_reset(&state.bbc);
    }
#endif

#ifdef BBC_BUS_PIO
    bus_pio_start();
#endif

    // One frame = 20 ms = 40 000 cycles at 2 MHz
    const uint32_t frame_us = 20000;
    const uint32_t num_ticks = frame_us * (BBC_FREQUENCY / 1000000);
    uint32_t report_frames = 0;
    uint32_t report_busy_us = 0;

    while (1) {
        uint32_t start_time_in_micros = time_us_32();

#ifdef BBC_DIAG
        uint32_t t_run = time_us_32();
        run_ticks(num_ticks);
        diag_run_us += time_us_32() - t_run;
#else
        run_ticks(num_ticks);
#endif

        emu_frames++;
        tuh_task();
        release_pending_keys();
#ifdef BBC_DIAG
        diag_key_service();
#endif
        usb_poll();

        uint32_t execution_time = time_us_32() - start_time_in_micros;
        state.frame_time_us = execution_time;
#ifdef BBC_DIAG
        static uint32_t diag_frames;
        diag_update(++diag_frames);
#endif
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
