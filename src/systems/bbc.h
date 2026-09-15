#pragma once

// bbc.h
//
// BBC Micro Model B emulator in a C header, for the reload framework.
//
// Do this:
// ~~~C
// #define CHIPS_IMPL
// ~~~
// before you include this file in *one* C or C++ file to create the
// implementation.
//
// You need to include the following headers before including bbc.h:
//
// - chips/chips_common.h
// - chips/wdc65C02cpu.h | chips/mos6502cpu.h
// - chips/mos6522via.h
// - chips/mem.h
// - chips/clk.h
// - devices/wd1770.h
//
// ## The BBC Micro Model B
//
// - 6502 at 2 MHz, 32 KB RAM ($0000-$7FFF), 16 sideways ROM banks at
//   $8000-$BFFF selected by ROMSEL ($FE30), MOS at $C000-$FFFF with the
//   SHEILA I/O page at $FE00-$FEFF.
// - 6845 CRTC ($FE00/$FE01), Video ULA ($FE20 control / $FE21 palette),
//   system VIA ($FE40-$FE5F: keyboard, IC32 addressable latch, SN76489,
//   vsync on CA1, 100 Hz timers), user VIA ($FE60-$FE7F), Acorn 1770 FDC
//   ($FE80 control, $FE84-$FE87 registers, DRQ/INTRQ on NMI, .ssd/.dsd
//   images), ADC ($FEC0, stub), Tube ($FEE0, absent).
//
// Video is rendered one scanline at a time (at the start of every CRTC
// scanline) into a 640x256 4-bit framebuffer, so hardware scrolling
// (R12/R13) and palette changes between lines are honoured; changes
// inside a line are not.
//
// MODE 7 uses the SAA5050 English glyphs (bbc_teletext_font.h, from the
// public-domain Bedstead bitmaps) with the SAA5050 character rounding;
// colour, graphics (contiguous/separated, hold), double height and
// background, flash (48 fields on / 16 off) and conceal codes are honoured.
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

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BBC_FREQUENCY (2000000)  // 2 MHz

#define BBC_SCREEN_WIDTH      640
#define BBC_SCREEN_HEIGHT     256
#define BBC_FRAMEBUFFER_SIZE  ((BBC_SCREEN_WIDTH / 2) * BBC_SCREEN_HEIGHT)  // 4 bits per pixel

#define BBC_PALETTE_BITS 3
#define BBC_PALETTE_SIZE (1 << BBC_PALETTE_BITS)

#define BBC_NUM_ROM_BANKS 16

// Physical colours: bit 0 = red, bit 1 = green, bit 2 = blue
static const uint32_t bbc_palette[BBC_PALETTE_SIZE] = {
    RGBA8(0x00, 0x00, 0x00), /* black */
    RGBA8(0xFF, 0x00, 0x00), /* red */
    RGBA8(0x00, 0xFF, 0x00), /* green */
    RGBA8(0xFF, 0xFF, 0x00), /* yellow */
    RGBA8(0x00, 0x00, 0xFF), /* blue */
    RGBA8(0xFF, 0x00, 0xFF), /* magenta */
    RGBA8(0x00, 0xFF, 0xFF), /* cyan */
    RGBA8(0xFF, 0xFF, 0xFF), /* white */
};

typedef enum {
    BBC_MODEL_B = 0,        // Model B, OS 1.20, Acorn 1770 DFS at $FE80
    BBC_MODEL_MASTER = 1,   // Master 128, MOS 3.20: LYNNE/ANDY/HAZEL, ACCCON, RTC, 1770 at $FE24/$FE28
} bbc_model_t;

// Config parameters for bbc_init()
typedef struct {
    bbc_model_t model;
    chips_debug_t debug;  // Optional debugging hook
    chips_audio_desc_t audio;
    struct {
        chips_range_t os;                       // 16 KB MOS
        chips_range_t banks[BBC_NUM_ROM_BANKS]; // Sideways ROMs (16 KB each), .ptr == 0 for empty banks
    } roms;
    // Sideways RAM: bit n of `ram_banks` makes bank n writable RAM; `swr` provides
    // 16 KB per selected bank (in bank order), owned by the caller
    uint16_t ram_banks;
    chips_range_t swr;
    // Master 128 extra RAM (32 KB, owned by the caller): LYNNE 20 KB, ANDY 4 KB, HAZEL 8 KB
    chips_range_t master_ram;
    // Master 128 CMOS RAM contents (50 bytes) or .ptr == 0 for the defaults
    chips_range_t nvram;
} bbc_desc_t;

// SN76489 sound generator state
typedef struct {
    uint8_t reg;            // Latched register (channel << 1 | volume flag)
    uint16_t freq[4];       // Tone periods (10 bits), noise control for channel 3
    uint8_t vol[4];         // Volumes 0 (off) .. 15 (max)
    int32_t counter[4];
    uint8_t mask[4];        // Current output level of each channel (0 or 0xFF)
    bool noise_toggle;
    uint16_t noise_seed;
    int32_t sample_accum;   // Sample generation accumulator
    int32_t sample_period;
} bbc_sn76489_t;

// BBC Micro emulator state
typedef struct {
    MOS6502CPU_T cpu;
    mos6522via_t sysvia;
    mos6522via_t uservia;
    mem_t mem;
    bool valid;
    chips_debug_t debug;

    chips_audio_callback_t audio_callback;

    bbc_model_t model;
    uint8_t ram[0x8000];
    // Master 128
    uint8_t* lynne;            // Shadow screen RAM $3000-$7FFF (20 KB)
    uint8_t* andy;             // $8000-$8FFF when ROMSEL bit 7 (4 KB)
    uint8_t* hazel;            // $C000-$DFFF when ACCCON Y (8 KB)
    uint8_t acccon;            // bit 0 D (display LYNNE), 1 E (VDU code sees LYNNE), 2 X (all see LYNNE), 3 Y (HAZEL)
    uint16_t last_pc;          // Address of the executing instruction (for the E rule)
    uint8_t rtc_regs[64];      // MC146818: 0-9 time, 10-13 A-D, 14-63 CMOS RAM
    uint8_t rtc_addr;
    uint32_t rtc_ticks;        // 1 MHz ticks towards the next second
    uint8_t sysvia_pb_old_rtc;
    const uint8_t* os;
    const uint8_t* banks[BBC_NUM_ROM_BANKS];
    uint8_t* ram_bank[BBC_NUM_ROM_BANKS];   // Sideways RAM storage per bank, or 0
    uint8_t romsel;

    // System VIA peripherals
    uint8_t ic32;              // Addressable latch (bit 0: !sound write, bit 3: !keyboard write, bits 4-5: screen base, 6: caps LED, 7: shift LED)
    uint8_t sysvia_pb_old;
    uint8_t key_cols[16];      // Keyboard matrix: one byte per column, bit n = row n pressed
    uint8_t key_scan_column;   // Hardware auto-scan column counter

    // 6845 CRTC
    uint8_t crtc_reg[18];
    uint8_t crtc_addr;
    uint8_t hcc;               // Horizontal character counter
    uint8_t vcc;               // Vertical character row counter
    uint8_t rc;                // Raster counter
    uint8_t adjust;            // Vertical adjust line counter (0 = not in adjust)
    bool in_adjust;
    uint16_t ma;               // Current memory address
    uint16_t ma_row_start;     // Memory address at the start of the current character row
    uint8_t vsync_count;       // Remaining vsync scanlines (0 = no vsync)
    bool vsync;
    bool field;                // Interlace field (odd/even)
    int display_y;             // Output scanline (0 = first displayed row)
    bool frame_done;           // Set at the end of each vsync
    uint32_t field_count;      // Fields since power-up (teletext flash: 48 on / 16 off)

    // Video ULA
    bool ula_dirty;            // Palette or control changed: rebuild the byte -> pixels table
    uint8_t lut[256][8];       // One screen byte -> 16 (1 MHz) or 8 (2 MHz) packed 4-bit pixels
    uint8_t ula_ctrl;          // bit 0 flash, bit 1 teletext, bits 2-3 chars per line, bit 4 2 MHz CRTC clock, bits 5-7 cursor
    uint8_t ula_pal[16];       // Logical -> physical colour (0-15, 8-15 flashing)

    // Sound
    bbc_sn76489_t sn;

    // Floppy disc controller
    wd1770_t fdc;
    bool nmi;

    // uPD7002 ADC (analogue joysticks, centred): status, 16-bit result, conversion timer in us
    uint8_t adc_status;
    uint16_t adc_value;
    uint32_t adc_timer;

    // Teletext glyphs, 12x20 after SAA5050 rounding (bits 11..0)
    uint16_t tt_glyphs[96][20];

    // Framebuffer, 4 bits per pixel, 640x256
    uint8_t fb[BBC_FRAMEBUFFER_SIZE];

    uint32_t system_ticks;
    uint8_t stall;             // Remaining cycles during which the CPU clock is held (1 MHz bus access)
} bbc_t;

// Initialize a new BBC instance
void bbc_init(bbc_t* sys, const bbc_desc_t* desc);
// Discard BBC instance
void bbc_discard(bbc_t* sys);
// Reset a BBC instance (BREAK key)
void bbc_reset(bbc_t* sys);
// Tick BBC instance once (one 2 MHz CPU cycle)
void bbc_tick(bbc_t* sys);
// Tick BBC instance for a given number of microseconds, return number of executed ticks
uint32_t bbc_exec(bbc_t* sys, uint32_t micro_seconds);
// Press / release a key, key = BBC internal key number (row << 4 | column), e.g. 0x41 = A
void bbc_key_down(bbc_t* sys, uint8_t key);
void bbc_key_up(bbc_t* sys, uint8_t key);
// Insert a disc image (bytes stay owned by the caller and are modified by writes)
void bbc_insert_disc(bbc_t* sys, int drive, uint8_t* data, size_t size, int sides, bool write_protected);

#ifdef __cplusplus
}  // extern "C"
#endif

/*-- IMPLEMENTATION ----------------------------------------------------------*/
#ifdef CHIPS_IMPL
#include <string.h> /* memcpy, memset */
#ifndef CHIPS_ASSERT
#include <assert.h>
#define CHIPS_ASSERT(c) assert(c)
#endif

#include "bbc_teletext_font.h"

#define BBC_KEY_SHIFT 0x00
#define BBC_KEY_CTRL  0x01
#define BBC_KEY_BREAK 0xFF   // Not in the matrix: wired to the CPU reset line

// CRTC registers
#define CRTC_R0_HTOTAL      0
#define CRTC_R1_HDISPLAYED  1
#define CRTC_R2_HSYNC_POS   2
#define CRTC_R3_SYNC_WIDTH  3
#define CRTC_R4_VTOTAL      4
#define CRTC_R5_VADJUST     5
#define CRTC_R6_VDISPLAYED  6
#define CRTC_R7_VSYNC_POS   7
#define CRTC_R8_INTERLACE   8
#define CRTC_R9_MAX_RASTER  9
#define CRTC_R10_CURSOR_START 10
#define CRTC_R11_CURSOR_END   11
#define CRTC_R12_START_H    12
#define CRTC_R13_START_L    13
#define CRTC_R14_CURSOR_H   14
#define CRTC_R15_CURSOR_L   15

// Unpopulated sideways ROM sockets read as $FF
static uint8_t _bbc_empty_bank[0x4000];

static void _bbc_init_memorymap(bbc_t* sys);
static void _bbc_teletext_init(bbc_t* sys);
static void _bbc_update_keyboard(bbc_t* sys, bool advance);
static void _bbc_update_ic32(bbc_t* sys);
static void _bbc_update_rtc(bbc_t* sys, uint8_t old_ic32);
static void _bbc_render_scanline(bbc_t* sys);
static void _bbc_crtc_tick(bbc_t* sys);
static void _bbc_sn_write(bbc_sn76489_t* sn, uint8_t value);
static void _bbc_sn_tick(bbc_t* sys);

void bbc_init(bbc_t* sys, const bbc_desc_t* desc) {
    CHIPS_ASSERT(sys && desc);
    if (desc->debug.callback.func) {
        CHIPS_ASSERT(desc->debug.stopped);
    }

    memset(sys, 0, sizeof(bbc_t));
    sys->valid = true;
    sys->debug = desc->debug;
    sys->audio_callback = desc->audio.callback;

    CHIPS_ASSERT(desc->roms.os.ptr && (desc->roms.os.size == 0x4000));
    sys->os = desc->roms.os.ptr;
    for (int i = 0; i < BBC_NUM_ROM_BANKS; i++) {
        if (desc->roms.banks[i].ptr) {
            CHIPS_ASSERT(desc->roms.banks[i].size == 0x4000);
            sys->banks[i] = desc->roms.banks[i].ptr;
        }
    }
    if (desc->ram_banks && desc->swr.ptr) {
        uint8_t* p = desc->swr.ptr;
        size_t left = desc->swr.size;
        for (int i = 0; i < BBC_NUM_ROM_BANKS; i++) {
            if ((desc->ram_banks & (1 << i)) && left >= 0x4000) {
                sys->ram_bank[i] = p;
                p += 0x4000;
                left -= 0x4000;
            }
        }
    }

    sys->model = desc->model;
    if (desc->model == BBC_MODEL_MASTER) {
        CHIPS_ASSERT(desc->master_ram.ptr && desc->master_ram.size >= 0x8000);
        sys->lynne = (uint8_t*)desc->master_ram.ptr;
        sys->andy = sys->lynne + 0x5000;
        sys->hazel = sys->andy + 0x1000;
        // MC146818: 24-hour BCD clock (register B = $02), 2026-09-15 12:00:00, VRT set
        sys->rtc_regs[0] = 0x00; sys->rtc_regs[2] = 0x00; sys->rtc_regs[4] = 0x12;
        sys->rtc_regs[6] = 0x03; sys->rtc_regs[7] = 0x15; sys->rtc_regs[8] = 0x09; sys->rtc_regs[9] = 0x26;
        sys->rtc_regs[10] = 0x26; sys->rtc_regs[11] = 0x02; sys->rtc_regs[13] = 0x80;
        static const uint8_t nvram_default[50] = {
            0, 0, 0, 0, 0, 0xC9, 0xFF, 0xFF, 0x00, 0x00, 0x17, 0x80, 55, 0x03, 0x00, 0x01, 0x02};   // LANG 12, FS 9, MODE 7, FLOPPY, DELAY 55, REPEAT 3, TUBE, LOUD
        if (desc->nvram.ptr && desc->nvram.size >= 50) {
            memcpy(&sys->rtc_regs[14], desc->nvram.ptr, 50);
        } else {
            memcpy(&sys->rtc_regs[14], nvram_default, 50);
        }
    }

    MOS6502CPU_INIT(&sys->cpu, &(MOS6502CPU_DESC_T){0});
    mos6522via_init(&sys->sysvia);
    mos6522via_init(&sys->uservia);
    wd1770_init(&sys->fdc);
    sys->fdc.master_control = (desc->model == BBC_MODEL_MASTER);

    // Sound: one sample every (2 MHz / sample_rate) ticks, in 1/256 tick units
    int sample_rate = desc->audio.sample_rate > 0 ? desc->audio.sample_rate : 22050;
    sys->sn.sample_period = (int32_t)(((int64_t)BBC_FREQUENCY << 8) / sample_rate);
    sys->sn.noise_seed = 1 << 14;
    for (int i = 0; i < 4; i++) {
        sys->sn.vol[i] = 0;
    }

    sys->ic32 = 0xFF;  // Sound write and keyboard write disabled, screen base 3
    memset(sys->ula_pal, 0, sizeof(sys->ula_pal));
    sys->ula_dirty = true;
    sys->adc_status = 0xC0;   // Not busy, no conversion
    sys->adc_value = 0x8000;
    memset(_bbc_empty_bank, 0xFF, sizeof(_bbc_empty_bank));

    _bbc_init_memorymap(sys);
    _bbc_teletext_init(sys);
}

void bbc_discard(bbc_t* sys) {
    CHIPS_ASSERT(sys && sys->valid);
    sys->valid = false;
}

void bbc_reset(bbc_t* sys) {
    CHIPS_ASSERT(sys && sys->valid);
    mos6522via_reset(&sys->sysvia);
    mos6522via_reset(&sys->uservia);
    wd1770_reset(&sys->fdc);
    sys->romsel = 0;
    sys->acccon = 0;
    sys->ic32 = 0xFF;
    _bbc_init_memorymap(sys);
    MOS6502CPU_RESET(&sys->cpu);
}

// ROMSEL: select the sideways bank at $8000-$BFFF
static void _bbc_set_romsel(bbc_t* sys, uint8_t bank) {
    sys->romsel = (sys->model == BBC_MODEL_MASTER) ? (bank & 0x8F) : (bank & 0x0F);
    uint8_t b = sys->romsel & 0x0F;
    if (sys->ram_bank[b]) {
        mem_map_ram(&sys->mem, 0, 0x8000, 0x4000, sys->ram_bank[b]);
    } else if (sys->banks[b]) {
        mem_map_rom(&sys->mem, 0, 0x8000, 0x4000, sys->banks[b]);
    } else {
        mem_map_rom(&sys->mem, 0, 0x8000, 0x4000, _bbc_empty_bank);
    }
    if (sys->model == BBC_MODEL_MASTER && (sys->romsel & 0x80)) {
        mem_map_ram(&sys->mem, 0, 0x8000, 0x1000, sys->andy);   // ANDY
    }
}

// Master 128 ACCCON: HAZEL over the MOS at $C000-$DFFF (Y), LYNNE for all
// accesses to $3000-$7FFF (X). The E rule (only the VDU code in $C000-$DFFF
// sees LYNNE) is applied per access in _bbc_mem_rw.
static void _bbc_set_acccon(bbc_t* sys, uint8_t v) {
    sys->acccon = v;
    if (v & 0x08) {
        mem_map_ram(&sys->mem, 0, 0xC000, 0x2000, sys->hazel);
    } else {
        mem_map_rom(&sys->mem, 0, 0xC000, 0x2000, sys->os);
    }
    if (v & 0x04) {
        mem_map_ram(&sys->mem, 0, 0x3000, 0x5000, sys->lynne);
    } else {
        mem_map_ram(&sys->mem, 0, 0x3000, 0x5000, sys->ram + 0x3000);
    }
}

// System VIA port A: keyboard (out: column/row select, in: PA7 key state), SN76489 data
static void _bbc_update_keyboard(bbc_t* sys, bool advance) {
    // CA2 is high when a key is pressed in the scanned column (rows 1-7,
    // row 0 = SHIFT/CTRL/links never raises the interrupt).
    bool ca2 = false;
    uint8_t pa = mos6522via_get_pa(&sys->sysvia);
    if (sys->ic32 & 0x08) {
        // Auto-scan: hardware cycles through the columns (one per 1 MHz tick)
        if (sys->key_cols[sys->key_scan_column] & 0xFE) {
            ca2 = true;
        }
        if (advance) {
            sys->key_scan_column = (sys->key_scan_column + 1) & 0x0F;
        }
        // PA7 is not driven by the keyboard in auto-scan mode (the RTC may drive port A on the Master)
    } else {
        // Manual scan: MOS writes column (PA0-3) and row (PA4-6), reads PA7
        uint8_t col = pa & 0x0F;
        uint8_t row = (pa >> 4) & 0x07;
        if (sys->key_cols[col] & 0xFE) {
            ca2 = true;
        }
        if (sys->key_cols[col] & (1 << row)) {
            mos6522via_set_pa(&sys->sysvia, pa | 0x80);
        } else {
            mos6522via_set_pa(&sys->sysvia, pa & 0x7F);
        }
    }
    mos6522via_set_ca2(&sys->sysvia, ca2);
    if (!advance) {
        // Called from a VIA access: latch the CA2 edge into the IFR right away
        _mos6522via_update_cab(&sys->sysvia);
    }
}

// Master 128 MC146818 on the system VIA: PB6 = chip select, PB7 = address
// strobe, IC32 bit 1 = read, bit 2 = data strobe, port A = data bus
// Advance the MC146818 clock by `us` microseconds (BCD, 24-hour, register B bit 2 = binary)
static void _bbc_rtc_tick(bbc_t* sys, uint32_t us) {
    sys->rtc_ticks += us;
    if (sys->rtc_ticks < 1000000) return;
    sys->rtc_ticks -= 1000000;
    uint8_t* r = sys->rtc_regs;
    r[12] |= 0x90;   // Register C: update-ended flag (UF) + IRQF, cleared when read
    bool binary = r[11] & 0x04;
    static const uint8_t days_in_month[13] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    #define RTC_INC(reg, limit, wrap) do { \
        uint8_t v = r[reg]; \
        v = binary ? (uint8_t)(v + 1) : (uint8_t)(((v & 0x0F) == 9) ? ((v & 0xF0) + 0x10) : (v + 1)); \
        uint8_t dec = binary ? v : (uint8_t)((v >> 4) * 10 + (v & 0x0F)); \
        if (dec > (limit)) { r[reg] = (wrap); carry = true; } else { r[reg] = v; carry = false; } } while (0)
    bool carry;
    RTC_INC(0, 59, 0); if (!carry) return;
    RTC_INC(2, 59, 0); if (!carry) return;
    RTC_INC(4, 23, 0); if (!carry) return;
    RTC_INC(6, 7, 1);
    uint8_t month = binary ? r[8] : (uint8_t)((r[8] >> 4) * 10 + (r[8] & 0x0F));
    uint8_t dim = (month >= 1 && month <= 12) ? days_in_month[month] : 31;
    RTC_INC(7, dim, 1); if (!carry) return;
    RTC_INC(8, 12, 1); if (!carry) return;
    RTC_INC(9, 99, 0);
    #undef RTC_INC
}

static void _bbc_update_rtc(bbc_t* sys, uint8_t old_ic32) {
    uint8_t pb = mos6522via_get_pb(&sys->sysvia);
    bool cs = pb & 0x40, as = pb & 0x80;
    bool old_as = sys->sysvia_pb_old_rtc & 0x80;
    uint8_t pa = mos6522via_get_pa(&sys->sysvia);
    if (cs && old_as && !as) {
        sys->rtc_addr = pa & 0x3F;                              // Address latched on AS 1 -> 0
    }
    if (cs && !as) {
        if (sys->ic32 & 0x02) {
            // Read mode: the RTC drives port A (D register reports valid RAM/time)
            uint8_t v = sys->rtc_regs[sys->rtc_addr];
            if (sys->rtc_addr == 13) v |= 0x80;
            if (sys->rtc_addr == 12) sys->rtc_regs[12] = 0;    // Flags cleared by the read
            mos6522via_set_pa(&sys->sysvia, v);
        } else if ((old_ic32 & 0x04) && !(sys->ic32 & 0x04)) {
            // Write on data strobe 1 -> 0 (registers C and D are read-only)
            if (sys->rtc_addr != 12 && sys->rtc_addr != 13) sys->rtc_regs[sys->rtc_addr] = pa;
        }
    }
    sys->sysvia_pb_old_rtc = pb;
}

// System VIA port B: IC32 addressable latch (PB0-2 = bit, PB3 = value), joystick buttons (PB4-5 in)
static void _bbc_update_ic32(bbc_t* sys) {
    uint8_t pb = mos6522via_get_pb(&sys->sysvia);
    uint8_t old = sys->ic32;
    if (pb != sys->sysvia_pb_old) {
        uint8_t mask = 1 << (pb & 7);
        if (pb & 0x08) {
            sys->ic32 |= mask;
        } else {
            sys->ic32 &= ~mask;
        }
        if ((old & 0x01) && !(sys->ic32 & 0x01)) {
            // Sound write enable went low: latch port A into the SN76489
            _bbc_sn_write(&sys->sn, mos6522via_get_pa(&sys->sysvia));
        }
        sys->sysvia_pb_old = pb;
    }
    // Joystick fire buttons not pressed, speech chip absent (PB6 = 1, PB7 = 1)
    mos6522via_set_pb(&sys->sysvia, pb | 0xF0);

    if (sys->model == BBC_MODEL_MASTER) {
        _bbc_update_rtc(sys, old);
        _bbc_rtc_tick(sys, 2);
    }
}

static void _bbc_mem_rw(bbc_t* sys, uint16_t addr, bool rw) {
    if ((addr & 0xFF00) == 0xFE00) {
        // SHEILA
        uint8_t data = 0xFF;
        switch (addr & 0xE0) {
            case 0x00:
                if (addr < 0xFE08) {
                    // 6845 CRTC
                    if (rw) {
                        if (addr & 1) {
                            data = (sys->crtc_addr < 18) ? sys->crtc_reg[sys->crtc_addr] : 0;
                            // R12/R13 are write-only on the 6845
                            if (sys->crtc_addr == 12 || sys->crtc_addr == 13) {
                                data = 0;
                            }
                        } else {
                            data = 0;
                        }
                    } else {
                        if (addr & 1) {
                            if (sys->crtc_addr < 18) {
                                sys->crtc_reg[sys->crtc_addr] = MOS6502CPU_GET_DATA(&sys->cpu);
                            }
                        } else {
                            sys->crtc_addr = MOS6502CPU_GET_DATA(&sys->cpu) & 0x1F;
                        }
                    }
                } else {
                    // 6850 ACIA (cassette / RS423): no interrupt, nothing received
                    if (rw) {
                        data = (addr & 1) ? 0x00 : 0x02;  // Status: TDRE set
                    }
                }
                break;
            case 0x20:
                if ((addr & 0xFF) < 0x24) {
                    // Video ULA (write only)
                    if (!rw) {
                        uint8_t v = MOS6502CPU_GET_DATA(&sys->cpu);
                        if (addr & 1) {
                            sys->ula_pal[v >> 4] = (v & 0x0F) ^ 7;
                        } else {
                            sys->ula_ctrl = v;
                        }
                        sys->ula_dirty = true;
                    } else {
                        data = 0xFE;
                    }
                } else if (sys->model == BBC_MODEL_MASTER) {
                    // Master 128: ROMSEL ($FE30-$FE33) and ACCCON ($FE34-$FE37) read back;
                    // 1770 control at $FE24-$FE27, registers at $FE28-$FE2F
                    uint8_t lo = (uint8_t)addr;
                    if (lo < 0x28) {
                        if (rw) data = wd1770_read_control(&sys->fdc);
                        else wd1770_write_control(&sys->fdc, MOS6502CPU_GET_DATA(&sys->cpu));
                    } else if (lo < 0x30) {
                        if (rw) data = wd1770_read(&sys->fdc, addr & 3);
                        else wd1770_write(&sys->fdc, addr & 3, MOS6502CPU_GET_DATA(&sys->cpu));
                    } else if (lo < 0x34) {
                        if (rw) data = sys->romsel;
                        else _bbc_set_romsel(sys, MOS6502CPU_GET_DATA(&sys->cpu));
                    } else {
                        if (rw) data = sys->acccon;
                        else _bbc_set_acccon(sys, MOS6502CPU_GET_DATA(&sys->cpu));
                    }
                } else {
                    // ROMSEL (write only)
                    if (!rw) {
                        _bbc_set_romsel(sys, MOS6502CPU_GET_DATA(&sys->cpu));
                    } else {
                        data = 0xFE;
                    }
                }
                break;
            case 0x40: {
                // System VIA. The keyboard answer (PA7, CA2) must be valid on the
                // very next access after the column/row was written on port A.
                uint8_t reg = addr & 0x0F;
                bool port_a = (reg == 1) || (reg == 15) || (reg == 3);
                bool port_b = (reg == 0) || (reg == 2);
                if (rw) {
                    if (port_a) {
                        if (sys->model == BBC_MODEL_MASTER) _bbc_update_rtc(sys, sys->ic32);
                        _bbc_update_keyboard(sys, false);
                    }
                    data = mos6522via_read(&sys->sysvia, reg);
                } else {
                    mos6522via_write(&sys->sysvia, reg, MOS6502CPU_GET_DATA(&sys->cpu));
                    if (port_b) _bbc_update_ic32(sys);          // Latch, RTC strobes
                    if (port_a) _bbc_update_keyboard(sys, false);
                }
                break;
            }
            case 0x60:
                // User VIA
                if (rw) {
                    data = mos6522via_read(&sys->uservia, addr & 0x0F);
                } else {
                    mos6522via_write(&sys->uservia, addr & 0x0F, MOS6502CPU_GET_DATA(&sys->cpu));
                }
                break;
            case 0x80:
                // Acorn 1770 interface (Model B): $FE80-$FE83 control latch, $FE84-$FE87 WD1770
                if (sys->model == BBC_MODEL_MASTER) {
                    if (rw) data = 0xFE;
                    break;
                }
                if (addr & 0x04) {
                    if (rw) {
                        data = wd1770_read(&sys->fdc, addr & 3);
                    } else {
                        wd1770_write(&sys->fdc, addr & 3, MOS6502CPU_GET_DATA(&sys->cpu));
                    }
                } else {
                    if (rw) {
                        data = wd1770_read_control(&sys->fdc);
                    } else {
                        wd1770_write_control(&sys->fdc, MOS6502CPU_GET_DATA(&sys->cpu));
                    }
                }
                break;
            case 0xA0:
                // Econet
                if (rw) {
                    data = 0xFE;
                }
                break;
            case 0xC0:
                // ADC uPD7002: bits 0-1 channel, 2 flag, 3 10-bit, 4-5 result MSBs,
                // 6 not busy, 7 not end of conversion; conversion 4 ms (8 bit) / 10 ms
                if (rw) {
                    switch (addr & 3) {
                        case 0: data = sys->adc_status; break;
                        case 1: sys->adc_status |= 0x80; data = (uint8_t)(sys->adc_value >> 8); break;
                        default: data = (uint8_t)sys->adc_value; break;
                    }
                } else if ((addr & 3) == 0) {
                    uint8_t cmd = MOS6502CPU_GET_DATA(&sys->cpu);
                    sys->adc_status = (uint8_t)((cmd & 0x0F) | 0x80);   // busy, not complete
                    sys->adc_timer = (cmd & 0x08) ? 10000 : 4000;
                }
                break;
            case 0xE0:
                // Tube: absent
                if (rw) {
                    data = 0xFE;
                }
                break;
            default:
                break;
        }
        if (rw) {
            MOS6502CPU_SET_DATA(&sys->cpu, data);
        }
    } else if ((addr & 0xFF00) == 0xFC00 || (addr & 0xFF00) == 0xFD00) {
        // FRED / JIM: 1 MHz bus, nothing connected
        if (rw) {
            MOS6502CPU_SET_DATA(&sys->cpu, 0xFF);
        }
    } else if (sys->model == BBC_MODEL_MASTER && (sys->acccon & 0x02) && !(sys->acccon & 0x04) &&
               addr >= 0x3000 && addr < 0x8000 && sys->last_pc >= 0xC000 && sys->last_pc < 0xE000) {
        // ACCCON E: code running from $C000-$DFFF (VDU driver) accesses LYNNE
        if (rw) {
            MOS6502CPU_SET_DATA(&sys->cpu, sys->lynne[addr - 0x3000]);
        } else {
            sys->lynne[addr - 0x3000] = MOS6502CPU_GET_DATA(&sys->cpu);
        }
    } else {
        // Regular memory access
        if (rw) {
            MOS6502CPU_SET_DATA(&sys->cpu, mem_rd(&sys->mem, addr));
        } else {
            mem_wr(&sys->mem, addr, MOS6502CPU_GET_DATA(&sys->cpu));
        }
    }
}

// 1 MHz bus devices: FRED/JIM ($FC00-$FDFF), CRTC/ACIA/SERPROC ($FE00-$FE1F),
// VIAs ($FE40-$FE7F), FDC/Econet ($FE80-$FE9F), ADC ($FEC0-$FEDF)
static inline bool _bbc_is_1mhz(uint16_t addr) {
    if ((addr & 0xFE00) == 0xFC00) return true;
    if ((addr & 0xFF00) != 0xFE00) return false;
    uint8_t lo = (uint8_t)addr;
    return lo < 0x20 || (lo >= 0x40 && lo < 0xA0) || (lo >= 0xC0 && lo < 0xE0);
}

void bbc_tick(bbc_t* sys) {
    if (sys->stall) {
        // CPU clock held: the access to a 1 MHz device is being stretched
        sys->stall--;
    } else {
        MOS6502CPU_TICK(&sys->cpu);
        uint16_t a = MOS6502CPU_GET_ADDR(&sys->cpu);
        if (sys->cpu.sync) sys->last_pc = a;
        if (_bbc_is_1mhz(a)) {
            // Stretched to the next 1 MHz edge, then one full 1 MHz cycle: 1 or 2 extra cycles
            sys->stall = (uint8_t)((sys->system_ticks & 1) ? 1 : 2);
        }

        _bbc_mem_rw(sys, a, sys->cpu.rw);
    }

    // CRTC: 2 MHz character clock in modes 0-3, 1 MHz otherwise
    if ((sys->ula_ctrl & 0x10) || (sys->system_ticks & 1)) {
        _bbc_crtc_tick(sys);
    }

    // 1 MHz bus (VIAs, keyboard, latch, FDC): serviced every 2 us by 2 cycles,
    // which keeps the cost per CPU cycle low for the RP2040 (timers count in us)
    if ((sys->system_ticks & 3) == 3) {
        _bbc_update_keyboard(sys, true);
        _bbc_update_ic32(sys);
        mos6522via_set_ca1(&sys->sysvia, sys->vsync);
        if (sys->adc_timer) {
            sys->adc_timer = sys->adc_timer > 2 ? sys->adc_timer - 2 : 0;
            if (sys->adc_timer == 0) {
                sys->adc_value = 0x8000;                                  // Joystick centred
                sys->adc_status = (uint8_t)((sys->adc_status & 0x0F) | 0x40 | ((sys->adc_value >> 10) & 0x30));
            }
        }
        mos6522via_set_cb1(&sys->sysvia, (sys->adc_status & 0x80) != 0);   // CB1 = not end of conversion
        bool irq = mos6522via_tick(&sys->sysvia, 2);
        irq |= mos6522via_tick(&sys->uservia, 2);
        MOS6502CPU_SET_IRQ(&sys->cpu, irq);
        wd1770_tick(&sys->fdc);
        wd1770_tick(&sys->fdc);
        sys->nmi = wd1770_nmi(&sys->fdc);
        MOS6502CPU_SET_NMI(&sys->cpu, sys->nmi);
    }

    // Sound chip clock: 250 kHz
    if ((sys->system_ticks & 7) == 7) {
        _bbc_sn_tick(sys);
    }

    sys->system_ticks++;
}

uint32_t bbc_exec(bbc_t* sys, uint32_t micro_seconds) {
    CHIPS_ASSERT(sys && sys->valid);
    uint32_t num_ticks = clk_us_to_ticks(BBC_FREQUENCY, micro_seconds);
    if (0 == sys->debug.callback.func) {
        for (uint32_t ticks = 0; ticks < num_ticks; ticks++) {
            bbc_tick(sys);
        }
    } else {
        for (uint32_t ticks = 0; (ticks < num_ticks) && !(*sys->debug.stopped); ticks++) {
            bbc_tick(sys);
            sys->debug.callback.func(sys->debug.callback.user_data, 0);
        }
    }
    return num_ticks;
}

void bbc_key_down(bbc_t* sys, uint8_t key) {
    if (key == BBC_KEY_BREAK) {
        bbc_reset(sys);
        return;
    }
    sys->key_cols[key & 0x0F] |= (uint8_t)(1 << ((key >> 4) & 7));
}

void bbc_key_up(bbc_t* sys, uint8_t key) {
    if (key == BBC_KEY_BREAK) {
        return;
    }
    sys->key_cols[key & 0x0F] &= (uint8_t)~(1 << ((key >> 4) & 7));
}

void bbc_insert_disc(bbc_t* sys, int drive, uint8_t* data, size_t size, int sides, bool write_protected) {
    wd1770_insert(&sys->fdc, drive, data, size, sides, write_protected);
}

static void _bbc_init_memorymap(bbc_t* sys) {
    mem_init(&sys->mem);
    // RAM contents are undefined at power-up; the MOS clears what it needs
    mem_map_ram(&sys->mem, 0, 0x0000, 0x8000, sys->ram);
    _bbc_set_romsel(sys, sys->romsel);
    mem_map_rom(&sys->mem, 0, 0xC000, 0x4000, sys->os);
    if (sys->model == BBC_MODEL_MASTER) {
        _bbc_set_acccon(sys, sys->acccon);
    }
}

/*-- 6845 CRTC ---------------------------------------------------------------*/

// Byte of the displayed screen RAM (LYNNE when ACCCON D on the Master)
static inline uint8_t _bbc_vram(const bbc_t* sys, uint16_t addr) {
    if (sys->model == BBC_MODEL_MASTER && (sys->acccon & 0x01) && addr >= 0x3000) {
        return sys->lynne[addr - 0x3000];
    }
    return sys->ram[addr];
}

// Screen address wrap-around per IC32 bits 4-5 (values in 8-byte units, MA is a byte/8 address)
static const uint16_t _bbc_screen_wrap[4] = {0x4000 >> 3, 0x2000 >> 3, 0x5000 >> 3, 0x2800 >> 3};

static void _bbc_crtc_new_frame(bbc_t* sys) {
    sys->vcc = 0;
    sys->rc = 0;
    sys->in_adjust = false;
    sys->ma_row_start = (uint16_t)(((sys->crtc_reg[CRTC_R12_START_H] << 8) | sys->crtc_reg[CRTC_R13_START_L]) & 0x3FFF);
    sys->ma = sys->ma_row_start;
    sys->display_y = 0;
}

static void _bbc_crtc_tick(bbc_t* sys) {
    const uint8_t* r = sys->crtc_reg;
    bool interlace = (r[CRTC_R8_INTERLACE] & 3) == 3;

    if (sys->hcc == 0) {
        // Start of a scanline: render it, then handle vsync
        _bbc_render_scanline(sys);
        if (sys->vsync_count) {
            sys->vsync_count--;
            if (sys->vsync_count == 0) {
                sys->vsync = false;
                sys->frame_done = true;
                sys->field_count++;
            }
        }
    }

    sys->ma++;
    sys->hcc++;
    if (sys->hcc > r[CRTC_R0_HTOTAL]) {
        // End of scanline
        sys->hcc = 0;
        sys->display_y++;
        uint8_t raster_step = interlace ? 2 : 1;
        bool row_end;
        if (sys->in_adjust) {
            sys->adjust++;
            row_end = false;
            if (sys->adjust >= r[CRTC_R5_VADJUST]) {
                sys->field = !sys->field;
                _bbc_crtc_new_frame(sys);
            } else {
                sys->ma = sys->ma_row_start;
            }
        } else {
            sys->rc += raster_step;
            row_end = (sys->rc > r[CRTC_R9_MAX_RASTER]);
            if (!row_end) {
                sys->ma = sys->ma_row_start;
            }
        }
        if (row_end) {
            sys->rc = 0;
            sys->ma_row_start = (uint16_t)((sys->ma_row_start + r[CRTC_R1_HDISPLAYED]) & 0x3FFF);
            sys->ma = sys->ma_row_start;
            sys->vcc++;
            if (sys->vcc == r[CRTC_R7_VSYNC_POS] && !sys->vsync) {
                uint8_t width = (r[CRTC_R3_SYNC_WIDTH] >> 4) & 0x0F;
                sys->vsync_count = width ? width : 16;
                sys->vsync = true;
            }
            if (sys->vcc > r[CRTC_R4_VTOTAL]) {
                if (r[CRTC_R5_VADJUST]) {
                    sys->in_adjust = true;
                    sys->adjust = 0;
                } else {
                    sys->field = !sys->field;
                    _bbc_crtc_new_frame(sys);
                }
            }
        }
    }
}

/*-- Video ULA rendering -----------------------------------------------------*/

// Resolve a logical colour to a physical colour, honouring flashing colours
static inline uint8_t _bbc_phys_colour(const bbc_t* sys, uint8_t logical) {
    uint8_t p = sys->ula_pal[logical & 0x0F];
    if (p & 8) {
        return (sys->ula_ctrl & 1) ? ((p & 7) ^ 7) : (p & 7);
    }
    return p & 7;
}

// Bits of a bitmap byte for logical colour extraction (ULA shift register):
// 2 colours: bit 7 of each shift; 4 colours: bits 7,3 ; 16 colours: bits 7,5,3,1
static inline uint8_t _bbc_ula_index(uint8_t byte) {
    return (uint8_t)(((byte >> 4) & 0x08) | ((byte >> 3) & 0x04) | ((byte >> 2) & 0x02) | ((byte >> 1) & 0x01));
}

// Teletext (MODE 7): SAA5050 character generator, English set. Glyphs are
// 5x9 in a 6x10 cell, doubled to 12x20 with the SAA5050 character rounding
// (an off pixel gets a quadrant filled when its two orthogonal neighbours
// towards that quadrant are on and the diagonal one is off). One frame line
// = two of the 20 rows (the two interlaced fields merged).
static void _bbc_teletext_init(bbc_t* sys) {
    for (int ch = 0; ch < 96; ch++) {
        const uint8_t* g = bbc_teletext_font[ch];
        // 6x10 source with 1-pixel blank margin at right and bottom
        #define TT_PX(x, y) (((x) >= 0 && (x) < 5 && (y) >= 0 && (y) < 9) ? ((g[y] >> (4 - (x))) & 1) : 0)
        for (int y = 0; y < 10; y++) {
            uint16_t top = 0, bottom = 0;
            for (int x = 0; x < 6; x++) {
                bool on = TT_PX(x, y);
                bool l = TT_PX(x - 1, y), r = TT_PX(x + 1, y), u = TT_PX(x, y - 1), d = TT_PX(x, y + 1);
                bool ul = TT_PX(x - 1, y - 1), ur = TT_PX(x + 1, y - 1), dl = TT_PX(x - 1, y + 1), dr = TT_PX(x + 1, y + 1);
                bool tl = on || (l && u && !ul);
                bool tr = on || (r && u && !ur);
                bool bl = on || (l && d && !dl);
                bool br = on || (r && d && !dr);
                if (tl) top |= (uint16_t)(0x800 >> (x * 2));
                if (tr) top |= (uint16_t)(0x400 >> (x * 2));
                if (bl) bottom |= (uint16_t)(0x800 >> (x * 2));
                if (br) bottom |= (uint16_t)(0x400 >> (x * 2));
            }
            sys->tt_glyphs[ch][y * 2] = top;
            sys->tt_glyphs[ch][y * 2 + 1] = bottom;
        }
        #undef TT_PX
    }
}

typedef struct {
    uint8_t fg, bg;
    bool graphics, separated, double_height, hold, flash, conceal;
    uint8_t held;
    bool held_separated;
} _bbc_tt_state_t;

// Draw 12 teletext pixels (bits 11..0) as 16 frame pixels
static inline void _bbc_tt_put(uint8_t* p, uint16_t bits, uint8_t fg, uint8_t bg) {
    for (int px = 0; px < 16; px++) {
        int tx = (px * 3) >> 2;
        p[px] = (bits & (0x800 >> tx)) ? fg : bg;
    }
}

static void _bbc_render_teletext_line(bbc_t* sys, uint8_t* line, int chars, uint16_t ma, int raster) {
    _bbc_tt_state_t st = {.fg = 7, .bg = 0};
    bool flash_off = (sys->field_count & 63) >= 48;
    // A row following a row that used double height shows the bottom halves
    bool bottom_row = false;
    if (sys->vcc > 0) {
        uint16_t prev = (uint16_t)(ma - chars);
        for (int c = 0; c < chars && c < 40; c++) {
            if ((_bbc_vram(sys, (uint16_t)(((prev + c) & 0x3FF) | 0x7C00)) & 0x7F) == 0x0D) {
                bottom_row = true;
                break;
            }
        }
    }
    for (int c = 0; c < chars && c < 40; c++) {
        uint16_t a = (uint16_t)((ma + c) & 0x3FF) | 0x7C00;
        uint8_t ch = _bbc_vram(sys, a) & 0x7F;
        uint8_t fg = st.fg, bg = st.bg;
        uint8_t* p = line + c * 16;
        uint16_t bits = 0;
        bool draw_sixels = false;
        uint8_t sixel = 0;
        bool sixel_sep = st.separated;
        if (ch < 0x20) {
            // Control codes: set-after, except background which applies at once
            switch (ch) {
                case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07:
                    st.fg = ch; st.graphics = false; st.conceal = false; break;
                case 0x08: st.flash = true; break;
                case 0x09: st.flash = false; break;
                case 0x0C: st.double_height = false; break;
                case 0x0D: st.double_height = true; break;
                case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
                    st.fg = ch - 0x10; st.graphics = true; st.conceal = false; break;
                case 0x18: st.conceal = true; break;
                case 0x19: st.separated = false; break;
                case 0x1A: st.separated = true; break;
                case 0x1C: st.bg = 0; bg = 0; break;
                case 0x1D: st.bg = st.fg; bg = st.bg; break;
                case 0x1E: st.hold = true; break;
                case 0x1F: st.hold = false; break;
                default: break;
            }
            if (st.hold && st.graphics) {
                draw_sixels = true;
                sixel = st.held;
                sixel_sep = st.held_separated;
            }
        } else if (st.graphics && (ch & 0x20)) {
            draw_sixels = true;
            sixel = ch;
            st.held = ch;
            st.held_separated = st.separated;
        } else {
            int row;
            if (st.double_height) {
                row = bottom_row ? 10 + raster : raster;
            } else {
                row = raster * 2;
            }
            bits = sys->tt_glyphs[ch - 0x20][row];
            if (!st.double_height) {
                bits |= sys->tt_glyphs[ch - 0x20][row + 1];
            }
        }
        if (draw_sixels) {
            // 2 x 3 blocks of 6 x (6,8,6) teletext pixels; separated: 4 x (4,6,4) inside
            int band = raster < 3 ? 0 : (raster < 7 ? 1 : 2);
            bool left, right;
            switch (band) {
                case 0: left = sixel & 0x01; right = sixel & 0x02; break;
                case 1: left = sixel & 0x04; right = sixel & 0x08; break;
                default: left = sixel & 0x10; right = sixel & 0x40; break;
            }
            uint16_t lmask = 0xFC0, rmask = 0x03F;
            if (sixel_sep) {
                lmask = 0x3C0; rmask = 0x00F;
                bool gap_line = (band == 0 && raster == 2) || (band == 1 && raster == 6) || (band == 2 && raster == 9);
                if (gap_line) { lmask = 0; rmask = 0; }
            }
            bits = (uint16_t)((left ? lmask : 0) | (right ? rmask : 0));
        }
        if (st.conceal || (st.flash && flash_off)) {
            bits = 0;
        }
        _bbc_tt_put(p, bits, fg, bg);
    }
}

// Byte -> packed pixels table for the current ULA mode and palette.
// ULA chars per line (bits 2-3): 0 = 10, 1 = 20, 2 = 40, 3 = 80 ; with the 2 MHz
// clock (bit 4) a byte covers 8 output pixels, else 16.
// MODE 0/3: 1 bpp, MODE 1: 2 bpp, MODE 2: 4 bpp, MODE 4/6: 1 bpp, MODE 5: 2 bpp.
static void _bbc_ula_build_lut(bbc_t* sys) {
    int cpl_sel = (sys->ula_ctrl >> 2) & 3;
    bool fast = sys->ula_ctrl & 0x10;
    int px_per_byte = fast ? 8 : 16;
    int bpp = fast ? ((cpl_sel == 3) ? 1 : (cpl_sel == 2) ? 2 : 4) : ((cpl_sel == 2) ? 1 : (cpl_sel == 1) ? 2 : 4);
    int pixels_per_byte = 8 / bpp;
    int px_w = px_per_byte / pixels_per_byte;
    for (int b = 0; b < 256; b++) {
        uint8_t px[16];
        uint8_t byte = (uint8_t)b;
        int x = 0;
        for (int p = 0; p < pixels_per_byte; p++) {
            uint8_t col = _bbc_phys_colour(sys, _bbc_ula_index(byte));
            byte = (uint8_t)((byte << 1) | 1);
            for (int k = 0; k < px_w; k++) {
                px[x++] = col;
            }
        }
        for (int i = 0; i < px_per_byte / 2; i++) {
            sys->lut[b][i] = (uint8_t)((px[i * 2] << 4) | px[i * 2 + 1]);
        }
    }
    sys->ula_dirty = false;
}

// 6845 hardware cursor: R14/R15 address, rasters R10 (bits 0-4) to R11, blink
// from R10 bits 5-6 (00 steady, 01 off, 10 = 16 fields, 11 = 32 fields). The
// ULA inverts the colours of the cell (width: one character, ULA control bits
// 5-7 select 1 or 2 characters in the 2 MHz modes).
static void _bbc_draw_cursor(bbc_t* sys, int y, int cell_bytes) {
    const uint8_t* r = sys->crtc_reg;
    uint8_t mode = (r[CRTC_R10_CURSOR_START] >> 5) & 3;
    if (mode == 1) return;
    if (mode == 2 && (sys->field_count & 16)) return;
    if (mode == 3 && (sys->field_count & 32)) return;
    uint8_t start = r[CRTC_R10_CURSOR_START] & 0x1F, end = r[CRTC_R11_CURSOR_END] & 0x1F;
    // Interlaced video: the line shows rasters rc (even field) and rc + 1 (odd field)
    uint8_t r0 = sys->rc, r1 = (r[CRTC_R8_INTERLACE] & 3) == 3 ? (uint8_t)(sys->rc + 1) : sys->rc;
    if (!((r0 >= start && r0 <= end) || (r1 >= start && r1 <= end))) return;
    uint16_t cursor = (uint16_t)(((r[CRTC_R14_CURSOR_H] << 8) | r[CRTC_R15_CURSOR_L]) & 0x3FFF);
    int col = (int)((cursor - sys->ma_row_start) & 0x3FFF);
    if (col < 0 || col >= r[CRTC_R1_HDISPLAYED]) return;
    int width = ((sys->ula_ctrl & 0xA0) == 0xA0 && (sys->ula_ctrl & 0x10)) ? 2 : 1;   // Large cursor (MODE 0-2)
    uint8_t* dst = &sys->fb[y * (BBC_SCREEN_WIDTH / 2) + col * cell_bytes];
    for (int i = 0; i < width * cell_bytes && col * cell_bytes + i < BBC_SCREEN_WIDTH / 2; i++) {
        dst[i] ^= 0x77;
    }
}

static void _bbc_render_scanline(bbc_t* sys) {
    const uint8_t* r = sys->crtc_reg;
    int y = sys->display_y;
    if (y < 0 || y >= BBC_SCREEN_HEIGHT) {
        return;
    }
    uint8_t line[BBC_SCREEN_WIDTH];
    bool displayed = (sys->vcc < r[CRTC_R6_VDISPLAYED]) && !sys->in_adjust;
    int chars = r[CRTC_R1_HDISPLAYED];
    bool teletext = sys->ula_ctrl & 0x02;

    if (!displayed || chars == 0) {
        memset(line, 0, sizeof(line));
    } else if (teletext) {
        memset(line, 0, sizeof(line));
        int raster = (sys->rc >> 1) % 10;
        _bbc_render_teletext_line(sys, line, chars, sys->ma_row_start, raster);
    } else {
        // Bitmap modes: one table lookup per screen byte (see _bbc_ula_build_lut)
        if (sys->ula_dirty) {
            _bbc_ula_build_lut(sys);
        }
        bool fast = sys->ula_ctrl & 0x10;
        int out_bytes = fast ? 4 : 8;       // Packed bytes per screen byte (8 or 16 pixels)
        uint8_t raster = sys->rc;
        uint8_t* dst = &sys->fb[y * (BBC_SCREEN_WIDTH / 2)];
        int x = 0;
        for (int c = 0; c < chars && x + out_bytes <= BBC_SCREEN_WIDTH / 2; c++) {
            uint16_t ma = (uint16_t)((sys->ma_row_start + c) & 0x3FFF);
            uint16_t addr;
            if (ma & 0x1000) {
                addr = (uint16_t)((ma - _bbc_screen_wrap[(sys->ic32 >> 4) & 3]) & ~0x1000u);
            } else {
                addr = ma;
            }
            addr = (uint16_t)((addr << 3) | (raster & 7));
            uint8_t byte = (raster < 8 && addr < 0x8000) ? _bbc_vram(sys, addr) : 0;
            memcpy(dst + x, sys->lut[byte], (size_t)out_bytes);
            x += out_bytes;
        }
        if (x < BBC_SCREEN_WIDTH / 2) {
            memset(dst + x, 0, (size_t)(BBC_SCREEN_WIDTH / 2 - x));
        }
        _bbc_draw_cursor(sys, y, out_bytes);
        return;
    }

    // Pack into the 4-bit framebuffer
    uint8_t* dst = &sys->fb[y * (BBC_SCREEN_WIDTH / 2)];
    for (int x = 0; x < BBC_SCREEN_WIDTH; x += 2) {
        *dst++ = (uint8_t)((line[x] << 4) | (line[x + 1] & 0x0F));
    }
    if (displayed && chars) {
        _bbc_draw_cursor(sys, y, teletext ? 8 : ((sys->ula_ctrl & 0x10) ? 4 : 8));
    }
}

/*-- SN76489 -----------------------------------------------------------------*/

static void _bbc_sn_write(bbc_sn76489_t* sn, uint8_t value) {
#ifdef BBC_SN_TRACE
    fprintf(stderr, "sn76489: write %02X\n", value);
#endif
    if (value & 0x80) {
        sn->reg = (value >> 4) & 7;
        uint8_t v = value & 0x0F;
        int ch = sn->reg >> 1;
        if (sn->reg & 1) {
            sn->vol[ch] = v ^ 0x0F;
        } else {
            sn->freq[ch] = (uint16_t)((sn->freq[ch] & ~0x0Fu) | v);
            if (ch == 3) {
                sn->noise_seed = 1 << 14;
            }
        }
    } else {
        int ch = sn->reg >> 1;
        uint8_t v = value & 0x3F;
        if (sn->reg & 1) {
            sn->vol[ch] = (v & 0x0F) ^ 0x0F;
        } else if (ch == 3) {
            sn->freq[3] = v;
            sn->noise_seed = 1 << 14;
        } else {
            sn->freq[ch] = (uint16_t)((sn->freq[ch] & 0x0F) | (v << 4));
        }
    }
}

// Volume table: 2 dB steps, 15 = max
static const uint8_t _bbc_sn_volume[16] = {0, 1, 1, 2, 2, 3, 4, 5, 6, 8, 10, 13, 16, 20, 25, 31};

static void _bbc_sn_tick(bbc_t* sys) {
    bbc_sn76489_t* sn = &sys->sn;

    // Called at 250 kHz
    {
        for (int i = 0; i < 3; i++) {
            if (sn->counter[i] > 0) {
                sn->counter[i]--;
            }
            if (sn->counter[i] == 0) {
                sn->mask[i] = ~sn->mask[i];
                sn->counter[i] = sn->freq[i] ? sn->freq[i] : 1024;
            }
        }
        if (sn->counter[3] > 0) {
            sn->counter[3]--;
        }
        if (sn->counter[3] == 0) {
            sn->noise_toggle = !sn->noise_toggle;
            if (sn->noise_toggle) {
                bool bit;
                if (sn->freq[3] & 4) {
                    // White noise: 15-bit LFSR, taps 0 and 1
                    bit = sn->noise_seed & 1;
                    uint16_t fb = ((sn->noise_seed & 1) ^ ((sn->noise_seed >> 1) & 1)) & 1;
                    sn->noise_seed = (uint16_t)((sn->noise_seed >> 1) | (fb << 14));
                } else {
                    // Periodic noise
                    bit = sn->noise_seed & 1;
                    sn->noise_seed = (uint16_t)((sn->noise_seed >> 1) | ((sn->noise_seed & 1) << 14));
                }
                sn->mask[3] = bit ? 0xFF : 0x00;
            }
            switch (sn->freq[3] & 3) {
                case 3: sn->counter[3] = sn->freq[2] ? sn->freq[2] : 1024; break;
                case 2: sn->counter[3] = 0x40; break;
                case 1: sn->counter[3] = 0x20; break;
                default: sn->counter[3] = 0x10; break;
            }
        }
    }

    // Output sample (sample_period is in 1/256 CPU cycles; 8 cycles per call)
    sn->sample_accum += 8 * 256;
    if (sn->sample_accum >= sn->sample_period) {
        sn->sample_accum -= sn->sample_period;
        int level = 0;
        for (int i = 0; i < 4; i++) {
            level += sn->mask[i] ? _bbc_sn_volume[sn->vol[i]] : 0;
        }
        // 4 channels x 31 max = 124 -> centre at 128
        uint8_t sample = (uint8_t)(128 + level);
        if (sys->audio_callback.func) {
            sys->audio_callback.func(sample, sys->audio_callback.user_data);
        }
    }
}

#endif  // CHIPS_IMPL
