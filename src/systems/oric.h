#pragma once

// oric.h
//
// Oric emulator in a C header.
//
// Do this:
// ~~~C
// #define CHIPS_IMPL
// ~~~
// before you include this file in *one* C or C++ file to create the
// implementation.
//
// Optionally provide the following macros with your own implementation
//
// ~~~C
// CHIPS_ASSERT(c)
// ~~~
//     your own assert macro (default: assert(c))
//
// You need to include the following headers before including oric.h:
//
// - chips/chips_common.h
// - chips/wdc65C02cpu.h | chips/mos6502cpu.h
// - chips/mos6522via.h
// - chips/ay38910psg.h
// - chips/kbd.h
// - chips/mem.h
// - chips/clk.h
// - devices/disk2_fdd.h, devices/disk2_fdc.h (Pravetz 8D disk interface)
// - devices/oric_dsk.h, devices/wd1793.h (Microdisc and Jasmin interfaces)
// - devices/oric_tape.h, devices/oric_tape_rec.h, devices/oric_tape_turbo.h
//
// ## The Oric
//
// Oric Atmos: 6502 at 1 MHz, VIA 6522, AY-3-8912, 48 KB RAM + 16 KB overlay
// RAM under the ROM, tape drive (.tap images or WAVE images), and one of the
// disk interfaces (oric_desc_t.fdc_type):
//
// - Pravetz 8D (Disk II compatible, NIB images, boot ROM in $0320-$03FF);
// - Microdisc (WD1793 at $0310-$0313, control register at $0314: bit 0
//   INTRQ -> IRQ enable, bit 1 = 0 BASIC ROM disabled, bit 4 side, bits 5-6
//   drive, bit 7 = 0 Microdisc EPROM at $E000-$FFFF when the BASIC ROM is
//   disabled; $0314 bit 7 reads /INTRQ, $0318 bit 7 reads /DRQ). The 8 KB
//   EPROM boots Sedoric from a MFM_DISK image;
// - Jasmin (WD1793 at $03F4-$03F7, $03F8 side, $03FA bit 0 overlay RAM on
//   $C000-$FFFF, $03FB bit 0 BASIC ROM disabled and Jasmin 2 KB ROM at
//   $F800-$FFFF, $03FC-$03FF drive select; DRQ -> IRQ). The Jasmin boots
//   FT-DOS with its "boot" button: after a reset the BASIC ROM starts, then
//   the boot is done (ROM disabled + reset) once BASIC is at its prompt.
//
// These register assignments are the ones of Oricutron (disk.c, machine.c).
// As in Oricutron, writes to $C000-$FFFF are ignored while a ROM is mapped
// there with the Microdisc and Jasmin interfaces.
//
// ## Links
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

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bump snapshot version when oric_t memory layout changes
#define ORIC_SNAPSHOT_VERSION (3)   // 2: sample_rate, sample_acc (audio clock); 3: sample_rate_q8

#define ORIC_FREQUENCY     (1000000)  // 1 MHz
#define ORIC_MAX_TAPE_SIZE (1 << 16)  // Max size of tape file in bytes

#define ORIC_SCREEN_WIDTH     240  // (240)
#define ORIC_SCREEN_HEIGHT    224  // (224)
#define ORIC_FRAMEBUFFER_SIZE ((ORIC_SCREEN_WIDTH / 2) * ORIC_SCREEN_HEIGHT)

#define PALETTE_BITS 3
#define PALETTE_SIZE (1 << PALETTE_BITS)

static const uint32_t oric_palette[PALETTE_SIZE] = {
    RGBA8(0x00, 0x00, 0x00), /* black */
    RGBA8(0xFF, 0x00, 0x00), /* red */
    RGBA8(0x00, 0xFF, 0x00), /* green */
    RGBA8(0xFF, 0xFF, 0x00), /* yellow */
    RGBA8(0x00, 0x00, 0xFF), /* blue */
    RGBA8(0xFF, 0x00, 0xFF), /* magenta */
    RGBA8(0x00, 0xFF, 0xFF), /* cyan */
    RGBA8(0xFF, 0xFF, 0xFF), /* white */
};

// Disk interfaces
typedef enum {
    ORIC_FDC_NONE = 0,
    ORIC_FDC_PRAVETZ,    // Pravetz 8D, Disk II compatible (NIB images)
    ORIC_FDC_MICRODISC,  // Oric Microdisc, WD1793 (MFM_DISK .dsk images, Sedoric)
    ORIC_FDC_JASMIN,     // Jasmin, WD1793 (MFM_DISK .dsk images, FT-DOS)
} oric_fdc_type_t;

#define ORIC_MICRODISC_ROM_SIZE 0x2000
#define ORIC_JASMIN_ROM_SIZE    0x0800
// Jasmin boot: address of the BASIC 1.1 keyboard wait loop (Oricutron), and
// delay after a reset when the CPU gives no opcode fetch information
#define ORIC_JASMIN_BOOT_PC     0xEB78
#ifndef ORIC_JASMIN_BOOT_TICKS
#define ORIC_JASMIN_BOOT_TICKS  3500000   // BASIC 1.1 reaches $EB78 after 2.46 s (measured)
#endif

// Config parameters for oric_init()
typedef struct {
    bool td_enabled;      // Set to true to enable tape drive emulation
    bool fdc_enabled;     // Set to true to enable the Pravetz 8D floppy disk controller (same as fdc_type = ORIC_FDC_PRAVETZ)
    oric_fdc_type_t fdc_type;  // Disk interface
    chips_debug_t debug;  // Optional debugging hook
    chips_audio_desc_t audio;
    void (*printer_cb)(uint8_t c, void* user_data);  // Optional: bytes sent to the printer port
    void* printer_user_data;
    struct {
        chips_range_t rom;
        chips_range_t boot_rom;       // Pravetz 8D disk boot ROM (512 bytes)
        chips_range_t microdisc_rom;  // Microdisc EPROM (8 KB), optional
        chips_range_t jasmin_rom;     // Jasmin ROM (2 KB), optional
    } roms;
} oric_desc_t;

// Oric emulator state
typedef struct {
    MOS6502CPU_T cpu;
    mos6522via_t via;
    ay38910psg_t psg;
    kbd_t kbd;
    mem_t mem;
    bool valid;
    chips_debug_t debug;

    chips_audio_callback_t audio_callback;
    uint32_t sample_rate_q8; // Audio samples per second, in 1/256 Hz (desc.audio.sample_rate, oric_set_sample_rate_q8)
    uint32_t sample_acc;     // Fractional sample clock: + sample_rate_q8 per cycle, 256 * ORIC_FREQUENCY per sample

    uint8_t ram[0xC000];
    uint8_t overlay_ram[0x4000];
    uint8_t* rom;
    uint8_t* boot_rom;

    int blink_counter;
    uint8_t pattr;

    uint8_t reserved[3];
    uint8_t fb[ORIC_FRAMEBUFFER_SIZE];
    bool screen_dirty;

    uint16_t extension;

    // Tape (devices/oric_tape.h, shared with the Telestrat): signal on CB1, motor on PB6,
    // CSAVE from PB7 (oric_tape_rec.h), fast loading by the patched ROM (oric_tape_turbo.h)
    bool td_enabled;
    oric_tape_t tape;
    uint32_t tape_due;           // system_ticks of the next edge of the signal
    oric_tape_rec_t tape_rec;
    bool tape_turbo;             // $03FE / $03FF answer (not with Jasmin or Pravetz)

    disk2_fdc_t fdc;  // Disk II floppy disk controller

    uint32_t system_ticks;

    // Microdisc / Jasmin interfaces
    oric_fdc_type_t fdc_type;
    uint8_t* microdisc_rom;
    uint8_t* jasmin_rom;
    bool via_irq;
    bool irq_line;           // Last level driven on the CPU IRQ line (written only when it changes)
    uint8_t md_ctrl;         // Microdisc control register ($0314)
    bool romdis;             // BASIC ROM disabled
    bool diskrom;            // Microdisc EPROM enabled
    bool jasmin_olay;        // Jasmin overlay RAM
    bool jasmin_rom_on;      // Jasmin ROM visible at $F800-$FFFF
    bool jasmin_boot;        // Jasmin boot pending (after a reset, disk in drive 0)
    uint32_t jasmin_boot_ticks;
    wd1793_t wd;             // WD1793 of the Microdisc / Jasmin

    // Printer (always acknowledging)
    void (*printer_cb)(uint8_t c, void* user_data);
    void* printer_user_data;
    uint8_t printer_strobe;
    uint8_t printer_ack;

} oric_t;

// Oric interface

// Initialize a new Oric instance
void oric_init(oric_t* sys, const oric_desc_t* desc);
// Discard Oric instance
void oric_discard(oric_t* sys);
// Reset a Oric instance
void oric_reset(oric_t* sys);

void oric_tick(oric_t* sys);

// Tick Oric instance for a given number of microseconds, return number of executed ticks
uint32_t oric_exec(oric_t* sys, uint32_t micro_seconds);
// Take a snapshot, patches pointers to zero or offsets, returns snapshot version
uint32_t oric_save_snapshot(oric_t* sys, oric_t* dst);
// Load a snapshot, returns false if snapshot version doesn't match
bool oric_load_snapshot(oric_t* sys, uint32_t version, oric_t* src);

void oric_screen_update(oric_t* sys);

// Change the disk interface (the machine is reset); false if its ROM is missing
bool oric_set_fdc(oric_t* sys, oric_fdc_type_t type);
// Change the BASIC ROM (16 KB, kept by the caller; e.g. BASIC 1.0 of the Oric-1); reset the machine afterwards
void oric_set_rom(oric_t* sys, uint8_t* rom);
// Interface a MFM_DISK image is made for (FT-DOS boot sector: Jasmin, else Microdisc)
oric_fdc_type_t oric_dsk_interface(oric_t* sys, oric_dsk_t* dsk);
// Insert an in-memory MFM_DISK image in a Microdisc / Jasmin drive (0-3)
bool oric_insert_dsk(oric_t* sys, int drive, uint8_t* data, uint32_t size, bool write_protected);
// Insert a .tap image held in memory (the tape drive must be enabled)
bool oric_insert_tap(oric_t* sys, const uint8_t* tap, uint32_t size);
// Insert a streamed .tap image (read by `read`), rewound; len 0: eject
bool oric_insert_tape(oric_t* sys, uint32_t len, oric_tape_read_t read, void* ctx);
void oric_eject_tape(oric_t* sys);
// CSAVE to .tap files (out: open/write/close callbacks; NULL: off)
void oric_tape_recorder(oric_t* sys, const oric_tape_rec_out_t* out);
// Fast loading: patch (or restore) the BASIC 1.1 ROM in RAM and answer $03FE/$03FF;
// false if the ROM is not BASIC 1.1 (then nothing is patched)
bool oric_tape_fast(oric_t* sys, bool on);
// Sample rate in 1/256 Hz: the rate the audio output really consumes (on the
// RP2040: audio_pwm_rate_q8), so that no sample is dropped or missing
void oric_set_sample_rate_q8(oric_t* sys, uint32_t rate_q8);

#ifdef __cplusplus
}  // extern "C"
#endif

/*-- IMPLEMENTATION ----------------------------------------------------------*/
#ifdef CHIPS_IMPL
// Placement of the functions run every emulated cycle (e.g. RAM on the RP2040)
#ifndef ORIC_HOT
#define ORIC_HOT
#endif
#include <string.h> /* memcpy, memset */
#ifndef CHIPS_ASSERT
#include <assert.h>
#define CHIPS_ASSERT(c) assert(c)
#endif

static void _oric_psg_out(int port_id, uint8_t data, void* user_data);
static uint8_t _oric_psg_in(int port_id, void* user_data);
static void _oric_init_memorymap(oric_t* sys);
static void _oric_init_key_map(oric_t* sys);
static void _oric_update_map(oric_t* sys);
static void _oric_fdc_reset(oric_t* sys);

#define PATTR_50HZ  (0x02)
#define PATTR_HIRES (0x04)
#define LATTR_ALT   (0x01)
#define LATTR_DSIZE (0x02)
#define LATTR_BLINK (0x04)

void oric_init(oric_t* sys, const oric_desc_t* desc) {
    CHIPS_ASSERT(sys && desc);
    if (desc->debug.callback.func) {
        CHIPS_ASSERT(desc->debug.stopped);
    }

    memset(sys, 0, sizeof(oric_t));
    sys->valid = true;
    sys->debug = desc->debug;
    sys->audio_callback = desc->audio.callback;
    sys->sample_rate_q8 = (uint32_t)CHIPS_DEFAULT(desc->audio.sample_rate, 22050) << 8;
    sys->printer_cb = desc->printer_cb;
    sys->printer_user_data = desc->printer_user_data;

    CHIPS_ASSERT(desc->roms.rom.ptr && (desc->roms.rom.size == 0x4000));
    sys->rom = desc->roms.rom.ptr;
    sys->boot_rom = desc->roms.boot_rom.ptr;
    if (desc->roms.microdisc_rom.ptr) {
        CHIPS_ASSERT(desc->roms.microdisc_rom.size == ORIC_MICRODISC_ROM_SIZE);
        sys->microdisc_rom = desc->roms.microdisc_rom.ptr;
    }
    if (desc->roms.jasmin_rom.ptr) {
        CHIPS_ASSERT(desc->roms.jasmin_rom.size == ORIC_JASMIN_ROM_SIZE);
        sys->jasmin_rom = desc->roms.jasmin_rom.ptr;
    }
    sys->fdc_type = desc->fdc_type;
    if (sys->fdc_type == ORIC_FDC_NONE && desc->fdc_enabled) {
        sys->fdc_type = ORIC_FDC_PRAVETZ;
    }
    if ((sys->fdc_type == ORIC_FDC_PRAVETZ && !sys->boot_rom) ||
        (sys->fdc_type == ORIC_FDC_MICRODISC && !sys->microdisc_rom) ||
        (sys->fdc_type == ORIC_FDC_JASMIN && !sys->jasmin_rom)) {
        sys->fdc_type = ORIC_FDC_NONE;   // ROM missing
    }
    wd1793_init(&sys->wd);

    MOS6502CPU_INIT(&sys->cpu, &(MOS6502CPU_DESC_T){0});

    mos6522via_init(&sys->via);
    sys->via.t1.t_bit = true;   // One-shot T1 idle until T1C-H is written (see oric_reset)
    ay38910psg_init(&sys->psg, &(ay38910psg_desc_t){.type = AY38910PSG_TYPE_8912,
                                                    .in_cb = _oric_psg_in,
                                                    .out_cb = _oric_psg_out,
                                                    .magnitude = CHIPS_DEFAULT(desc->audio.volume, 1.0f),
                                                    .user_data = sys});

    // setup memory map and keyboard matrix
    _oric_init_memorymap(sys);
    _oric_init_key_map(sys);

    sys->blink_counter = 0;
    sys->pattr = 0;

    sys->extension = 0;

    // Optionally setup tape drive
    sys->td_enabled = desc->td_enabled;
    oric_tape_init(&sys->tape);
    oric_tape_rec_init(&sys->tape_rec, 0);

    // Optionally setup floppy disk controller
    if (sys->fdc_type == ORIC_FDC_PRAVETZ) {
        CHIPS_ASSERT(sys->boot_rom && (desc->roms.boot_rom.size == 0x200));
        disk2_fdc_init(&sys->fdc);
        if (CHIPS_ARRAY_SIZE(oric_nib_images) > 0) {
            disk2_fdd_insert_disk(&sys->fdc.fdd[0], oric_nib_images[0]);
        }
    }
    _oric_fdc_reset(sys);
}

// Interface state after a reset
static void _oric_fdc_reset(oric_t* sys) {
    sys->md_ctrl = 0;
    sys->romdis = false;
    sys->diskrom = false;
    sys->jasmin_olay = false;
    sys->jasmin_rom_on = false;
    sys->jasmin_boot = false;
    sys->extension = 0;
    wd1793_reset(&sys->wd);
    wd1793_select(&sys->wd, 0, 0);
    if (sys->fdc_type == ORIC_FDC_MICRODISC) {
        // Control register cleared: BASIC ROM disabled, EPROM enabled (boot from the EPROM)
        sys->romdis = true;
        sys->diskrom = true;
    } else if (sys->fdc_type == ORIC_FDC_JASMIN) {
        // BASIC starts; the boot button is pressed once it waits for a key
        sys->jasmin_boot = wd1793_disk_present(&sys->wd, 0);
        sys->jasmin_boot_ticks = ORIC_JASMIN_BOOT_TICKS;
    }
    _oric_update_map(sys);
}

static void _oric_update_map(oric_t* sys) {
    sys->jasmin_rom_on = false;
    switch (sys->fdc_type) {
        case ORIC_FDC_MICRODISC:
            if (!sys->romdis) {
                mem_map_rom(&sys->mem, 0, 0xC000, 0x4000, sys->rom);
            } else if (sys->diskrom) {
                mem_map_ram(&sys->mem, 0, 0xC000, 0x2000, sys->overlay_ram);
                mem_map_rom(&sys->mem, 0, 0xE000, 0x2000, sys->microdisc_rom);
            } else {
                mem_map_ram(&sys->mem, 0, 0xC000, 0x4000, sys->overlay_ram);
            }
            break;
        case ORIC_FDC_JASMIN:
            if (sys->jasmin_olay) {
                mem_map_ram(&sys->mem, 0, 0xC000, 0x4000, sys->overlay_ram);
            } else if (sys->romdis) {
                // $F800-$FFFF: Jasmin ROM (2 KB, smaller than a memory page: see _oric_mem_rw)
                mem_map_ram(&sys->mem, 0, 0xC000, 0x4000, sys->overlay_ram);
                sys->jasmin_rom_on = true;
            } else {
                mem_map_rom(&sys->mem, 0, 0xC000, 0x4000, sys->rom);
            }
            break;
        default:
            mem_map_rw(&sys->mem, 0, 0xC000, 0x4000, sys->rom, sys->overlay_ram);
            break;
    }
}

void oric_set_rom(oric_t* sys, uint8_t* rom) {
    CHIPS_ASSERT(sys && sys->valid && rom);
    sys->rom = rom;
    _oric_update_map(sys);
}

bool oric_set_fdc(oric_t* sys, oric_fdc_type_t type) {
    CHIPS_ASSERT(sys && sys->valid);
    if ((type == ORIC_FDC_PRAVETZ && !sys->boot_rom) || (type == ORIC_FDC_MICRODISC && !sys->microdisc_rom) ||
        (type == ORIC_FDC_JASMIN && !sys->jasmin_rom)) {
        return false;
    }
    if (type == ORIC_FDC_PRAVETZ && !sys->fdc.valid) {
        disk2_fdc_init(&sys->fdc);
    }
    if (type != ORIC_FDC_PRAVETZ && sys->fdc.valid) {
        disk2_fdc_discard(&sys->fdc);
    }
    sys->fdc_type = type;
    oric_reset(sys);
    return true;
}

// First bytes of the FT-DOS boot sectors (track 0, sector 1), as recognised by Oricutron
static const uint8_t _oric_ftdos_boot[2][12] = {
    {0x78, 0xA9, 0x7F, 0x8D, 0x0E, 0x03, 0xA9, 0x01, 0x8D, 0xFA, 0x03, 0xA9},
    {0xEA, 0x78, 0xA9, 0x7F, 0x8D, 0x0E, 0x03, 0xA9, 0x01, 0x8D, 0xFA, 0x03},
};

oric_fdc_type_t oric_dsk_interface(oric_t* sys, oric_dsk_t* dsk) {
    CHIPS_ASSERT(sys && dsk);
    oric_dsk_track_t* t = &sys->wd.cache;
    oric_fdc_type_t type = ORIC_FDC_MICRODISC;
    if (oric_dsk_load_track(t, dsk, 0, 0)) {
        for (int i = 0; i < t->num_sectors; i++) {
            const uint8_t* id = t->raw + t->sectors[i].id;
            if (id[3] == 1 && (id[4] & 3) == 1 && t->sectors[i].data >= 0) {
                const uint8_t* d = t->raw + t->sectors[i].data + 1;
                if (memcmp(d, _oric_ftdos_boot[0], 12) == 0 || memcmp(d, _oric_ftdos_boot[1], 12) == 0) {
                    type = ORIC_FDC_JASMIN;
                }
                break;
            }
        }
    }
    oric_dsk_invalidate(t, dsk);
    return type;
}

bool oric_insert_dsk(oric_t* sys, int drive, uint8_t* data, uint32_t size, bool write_protected) {
    CHIPS_ASSERT(sys && sys->valid);
    return wd1793_insert_mem(&sys->wd, drive, data, size, write_protected);
}

bool oric_insert_tap(oric_t* sys, const uint8_t* tap, uint32_t size) {
    CHIPS_ASSERT(sys && sys->valid);
    if (!sys->td_enabled || !tap || size == 0) {
        return false;
    }
    oric_tape_insert_mem(&sys->tape, tap, size);
    sys->tape_due = sys->system_ticks;
    return true;
}

bool oric_insert_tape(oric_t* sys, uint32_t len, oric_tape_read_t read, void* ctx) {
    CHIPS_ASSERT(sys && sys->valid);
    if (!sys->td_enabled) {
        return false;
    }
    if (len && read) {
        oric_tape_insert(&sys->tape, len, read, ctx);
    } else {
        oric_tape_eject(&sys->tape);
    }
    sys->tape_due = sys->system_ticks;
    return sys->tape.inserted;
}

void oric_eject_tape(oric_t* sys) {
    CHIPS_ASSERT(sys && sys->valid);
    oric_tape_eject(&sys->tape);
}

void oric_tape_recorder(oric_t* sys, const oric_tape_rec_out_t* out) {
    CHIPS_ASSERT(sys && sys->valid);
    oric_tape_rec_init(&sys->tape_rec, out);
}

void oric_set_sample_rate_q8(oric_t* sys, uint32_t rate_q8) {
    CHIPS_ASSERT(sys && rate_q8 > 0);
    sys->sample_rate_q8 = rate_q8;
}

bool oric_tape_fast(oric_t* sys, bool on) {
    CHIPS_ASSERT(sys && sys->valid);
    const bool basic11 = sys->rom && oric_turbo_apply(sys->rom, on);
    sys->tape_turbo = on && basic11;
    return basic11;
}

void oric_discard(oric_t* sys) {
    CHIPS_ASSERT(sys && sys->valid);
    if (sys->fdc.valid) {
        disk2_fdc_discard(&sys->fdc);
    }
    wd1793_flush(&sys->wd);
    oric_tape_rec_motor_off(&sys->tape_rec);   // Closes a file being written
    sys->valid = false;
}

void oric_nmi(oric_t* sys) {
    CHIPS_ASSERT(sys && sys->valid);
    MOS6502CPU_NMI(&sys->cpu);
}

void oric_reset(oric_t* sys) {
    CHIPS_ASSERT(sys && sys->valid);
    mos6522via_reset(&sys->via);
    // As in Oricutron, a one-shot T1 raises no interrupt until T1C-H is written: with the
    // Microdisc the EPROM boots before BASIC programs T1, and Sedoric's disk routines lose
    // their end-of-command interrupt if a stray T1 interrupt comes in the middle of a read
    sys->via.t1.t_bit = true;
    ay38910psg_reset(&sys->psg);
    if (sys->fdc.valid) {
        disk2_fdc_reset(&sys->fdc);
    }
    // The tape is rewound (the machine starts again), the motor stopped
    oric_tape_rewind(&sys->tape);
    oric_tape_set_motor(&sys->tape, false);
    oric_tape_rec_motor_off(&sys->tape_rec);
    sys->tape_due = sys->system_ticks;
    _oric_fdc_reset(sys);
    MOS6502CPU_RESET(&sys->cpu);
}

// Microdisc registers ($0310-$031F; the other addresses reach the VIA)
static ORIC_HOT void _oric_microdisc_rw(oric_t* sys, uint16_t addr, bool rw) {
    if (addr >= 0x0310 && addr <= 0x0313) {
        if (rw) {
            MOS6502CPU_SET_DATA(&sys->cpu, wd1793_read(&sys->wd, addr & 3));
        } else {
            wd1793_write(&sys->wd, addr & 3, MOS6502CPU_GET_DATA(&sys->cpu));
        }
    } else if (addr == 0x0314) {
        if (rw) {
            MOS6502CPU_SET_DATA(&sys->cpu, (sys->wd.intrq ? 0x00 : 0x80) | 0x7F);
        } else {
            uint8_t v = MOS6502CPU_GET_DATA(&sys->cpu);
            sys->md_ctrl = v;
            wd1793_select(&sys->wd, (v >> 5) & 3, (v >> 4) & 1);
            sys->romdis = !(v & 0x02);
            sys->diskrom = !(v & 0x80);
            _oric_update_map(sys);
        }
    } else if (addr == 0x0318) {
        if (rw) {
            MOS6502CPU_SET_DATA(&sys->cpu, (sys->wd.drq ? 0x00 : 0x80) | 0x7F);
        }
    } else {
        if (rw) {
            MOS6502CPU_SET_DATA(&sys->cpu, mos6522via_read(&sys->via, addr & 0xF));
        } else {
            mos6522via_write(&sys->via, addr & 0xF, MOS6502CPU_GET_DATA(&sys->cpu));
        }
    }
}

// Jasmin registers ($03F4-$03FF; the other addresses reach the VIA)
static ORIC_HOT void _oric_jasmin_rw(oric_t* sys, uint16_t addr, bool rw) {
    if (addr >= 0x03F4 && addr <= 0x03F7) {
        if (rw) {
            MOS6502CPU_SET_DATA(&sys->cpu, wd1793_read(&sys->wd, addr & 3));
        } else {
            wd1793_write(&sys->wd, addr & 3, MOS6502CPU_GET_DATA(&sys->cpu));
        }
    } else if (addr >= 0x03F8) {
        if (rw) {
            uint8_t v = 0;
            if (addr == 0x03F8) v = (uint8_t)sys->wd.side;
            else if (addr == 0x03FA) v = sys->jasmin_olay ? 1 : 0;
            else if (addr == 0x03FB) v = sys->romdis ? 1 : 0;
            MOS6502CPU_SET_DATA(&sys->cpu, v);
        } else {
            uint8_t v = MOS6502CPU_GET_DATA(&sys->cpu);
            switch (addr) {
                case 0x03F8: wd1793_select(&sys->wd, sys->wd.drive, v & 1); break;
                case 0x03F9: break;   // Controller reset: ignored, as in Oricutron
                case 0x03FA: sys->jasmin_olay = v & 1; _oric_update_map(sys); break;
                case 0x03FB: sys->romdis = v & 1; _oric_update_map(sys); break;
                default: wd1793_select(&sys->wd, addr & 3, sys->wd.side); break;   // $03FC-$03FF
            }
        }
    } else {
        if (rw) {
            MOS6502CPU_SET_DATA(&sys->cpu, mos6522via_read(&sys->via, addr & 0xF));
        } else {
            mos6522via_write(&sys->via, addr & 0xF, MOS6502CPU_GET_DATA(&sys->cpu));
        }
    }
}

// Jasmin boot button: BASIC ROM disabled (Jasmin ROM on), then reset
static void _oric_jasmin_press_boot(oric_t* sys) {
    sys->jasmin_boot = false;
    sys->romdis = true;
    sys->jasmin_olay = false;
    _oric_update_map(sys);
    mos6522via_reset(&sys->via);
    sys->via.t1.t_bit = true;
    MOS6502CPU_RESET(&sys->cpu);
}

// Fast loading registers (oric_tape_turbo.h): $03FE next byte, $03FF synchro
static void _oric_tape_turbo_read(oric_t* sys, uint16_t addr) {
    uint8_t data;
    if ((addr & 0xFF) == ORIC_TURBO_BYTE_REG) {
        data = oric_tape_turbo_byte(&sys->tape);
    } else {
        data = oric_tape_turbo_sync(&sys->tape) ? 0x00 : 0x80;
    }
    MOS6502CPU_SET_DATA(&sys->cpu, data);
}

static ORIC_HOT void _oric_mem_rw(oric_t* sys, uint16_t addr, bool rw) {
    if ((addr >= 0x0300) && (addr <= 0x03FF)) {
        // Memory-mapped IO area
        if (sys->tape_turbo && rw && addr >= 0x03FE && sys->fdc_type != ORIC_FDC_JASMIN && !sys->fdc.valid) {
            _oric_tape_turbo_read(sys, addr);
        } else if (sys->fdc_type == ORIC_FDC_MICRODISC && addr >= 0x0310) {
            _oric_microdisc_rw(sys, addr, rw);
        } else if (sys->fdc_type == ORIC_FDC_JASMIN && addr >= 0x0310) {
            _oric_jasmin_rw(sys, addr, rw);
        } else if ((addr >= 0x0300) && (addr <= 0x030F)) {
            if (rw) {
                MOS6502CPU_SET_DATA(&sys->cpu, mos6522via_read(&sys->via, addr & 0xF));
            } else {
                mos6522via_write(&sys->via, addr & 0xF, MOS6502CPU_GET_DATA(&sys->cpu));
            }
        } else if ((addr >= 0x0310) && (addr <= 0x031F)) {
            if (sys->fdc.valid) {
                // Disk II FDC
                if (rw) {
                    // Memory read
                    MOS6502CPU_SET_DATA(&sys->cpu, disk2_fdc_read_byte(&sys->fdc, addr & 0xF));
                } else {
                    // Memory write
                    disk2_fdc_write_byte(&sys->fdc, addr & 0xF, MOS6502CPU_GET_DATA(&sys->cpu));
                }
            } else {
                if (rw) {
                    MOS6502CPU_SET_DATA(&sys->cpu, 0x00);
                }
            }
        } else if ((addr >= 0x0320) && (addr <= 0x03FF)) {
            if (sys->fdc.valid) {
                // Disk II boot rom
                if (rw) {
                    // Memory read
                    MOS6502CPU_SET_DATA(&sys->cpu, sys->boot_rom[(addr & 0xFF) + sys->extension]);
                } else {
                    // Memory write
                    switch (addr) {
                        case 0x380:
                            mem_map_rw(&sys->mem, 0, 0xC000, 0x4000, sys->rom, sys->overlay_ram);
                            sys->extension = 0;
                            break;

                        case 0x381:
                            mem_map_ram(&sys->mem, 0, 0xC000, 0x4000, sys->overlay_ram);
                            sys->extension = 0;
                            break;

                        case 0x382:
                            mem_map_rw(&sys->mem, 0, 0xC000, 0x4000, sys->rom, sys->overlay_ram);
                            sys->extension = 0x100;
                            break;

                        case 0x383:
                            mem_map_ram(&sys->mem, 0, 0xC000, 0x4000, sys->overlay_ram);
                            sys->extension = 0x100;
                            break;

                        default:
                            break;
                    }
                }
            } else {
                if (rw) {
                    MOS6502CPU_SET_DATA(&sys->cpu, 0x00);
                }
            }
        }
    } else if (sys->jasmin_rom_on && addr >= 0xF800) {
        // Jasmin ROM (read only)
        if (rw) {
            MOS6502CPU_SET_DATA(&sys->cpu, sys->jasmin_rom[addr - 0xF800]);
        }
    } else {
        // Regular memory access
        if (rw) {
            // Memory read
            MOS6502CPU_SET_DATA(&sys->cpu, mem_rd(&sys->mem, addr));
        } else {
            // Memory write
            mem_wr(&sys->mem, addr, MOS6502CPU_GET_DATA(&sys->cpu));

            if (addr >= 0x9800 && addr <= 0xBFDF) {
                sys->screen_dirty = true;
            }
        }
    }
}

// Tape: motor on PB6 (output only: at a reset, PB6 as an input reads 1 and must not start
// the motor), edges of the signal at their cycle (4-cycle steps), CSAVE from PB7
static inline void _oric_update_tape(oric_t* sys, uint8_t pb_out) {
    oric_tape_t* tp = &sys->tape;
    const uint32_t t = sys->system_ticks;
    const bool motor = (pb_out & 0x40) != 0;
    if (motor != tp->motor) {
        oric_tape_set_motor(tp, motor);
        sys->tape_due = t + (uint32_t)oric_tape_next(tp);
        if (!motor) oric_tape_rec_motor_off(&sys->tape_rec);
    }
    if (motor && sys->tape_rec.enabled) {
        oric_tape_rec_level(&sys->tape_rec, (mos6522via_get_pb(&sys->via) >> 7) & 1, t);
    }
    if (!tp->inserted) return;
    while (oric_tape_running(tp) && (int32_t)(t - sys->tape_due) >= 0) {
        oric_tape_toggle(tp);
        sys->tape_due += (uint32_t)oric_tape_next(tp);
    }
    // CB1 only when it changes (or an edge is pending): keeps the VIA's lazy mode
    const bool cb1 = tp->level != 0;
    if (cb1 != sys->via.pb.c1_in || sys->via.pb.c1_triggered) {
        mos6522via_set_cb1(&sys->via, cb1);
    }
}

// One cycle. The work done every 4 cycles (system_ticks multiple of 4) is grouped
// in the `quad` blocks: the PSG, Disk II and VIA periods are multiples of 4, and
// the sample clock advances by 4 cycles at a time (same count per second).
// _oric_tick can be inlined in a platform's frame loop (no call per cycle).
static inline __attribute__((always_inline)) void _oric_tick(oric_t* sys) {
    MOS6502CPU_TICK(&sys->cpu);

    _oric_mem_rw(sys, sys->cpu.addr, sys->cpu.rw);

    const bool quad = (sys->system_ticks & 3) == 0;
    if (quad) {
        // Tick PSG
        if ((sys->system_ticks & 63) == 0) {
            ay38910psg_tick_channels(&sys->psg);
            if ((sys->system_ticks & 127) == 0) {
                ay38910psg_tick_envelope_generator(&sys->psg);
                // Tick Disk II
                if (sys->fdc.valid) {
                    disk2_fdc_tick(&sys->fdc);
                }
            }
        }

        // Exactly sample_rate_q8 / 256 samples per second of emulated time (fractional clock),
        // three PSG channels mixed in integers (no overflow when several are loud)
        sys->sample_acc += 4 * sys->sample_rate_q8;
        if (sys->sample_acc >= 256u * ORIC_FREQUENCY) {
            sys->sample_acc -= 256u * ORIC_FREQUENCY;
            if (sys->audio_callback.func) {
                sys->audio_callback.func(ay38910psg_sample_u8(&sys->psg), sys->audio_callback.user_data);
            }
        }

        // Tick VIA (lazy: while no timer can reach zero, only the cycles are counted)
        sys->via_irq = mos6522via_tick(&sys->via, 4);
    }

    // Tick WD1793 (Microdisc, Jasmin)
    if (sys->fdc_type >= ORIC_FDC_MICRODISC) {
        if (sys->wd.state != WD1793_IDLE) {
            wd1793_tick(&sys->wd);
        }
        if (sys->jasmin_boot) {
            // Boot button, once BASIC waits for a key (or after a delay without opcode fetch information)
            if ((MOS6502CPU_SYNC(&sys->cpu) && MOS6502CPU_GET_ADDR(&sys->cpu) == ORIC_JASMIN_BOOT_PC && !sys->romdis) ||
                --sys->jasmin_boot_ticks == 0) {
                _oric_jasmin_press_boot(sys);
            }
        }
    }

    // IRQ line: VIA, plus Microdisc INTRQ (when enabled) or Jasmin DRQ; driven
    // only when it changes (on the board, a GPIO write per cycle otherwise)
    {
        bool irq = sys->via_irq;
        if (sys->fdc_type == ORIC_FDC_MICRODISC) {
            irq |= (sys->md_ctrl & 0x01) && sys->wd.intrq;
        } else if (sys->fdc_type == ORIC_FDC_JASMIN) {
            irq |= sys->wd.drq;
        }
        if (irq != sys->irq_line) {
            sys->irq_line = irq;
            MOS6502CPU_SET_IRQ(&sys->cpu, irq);
        }
    }

    if (quad) {

        // Update PSG state
        if (mos6522via_get_cb2(&sys->via)) {
            const uint8_t psg_data = mos6522via_get_pa(&sys->via);
            if (mos6522via_get_ca2(&sys->via)) {
                ay38910psg_latch_address(&sys->psg, psg_data);
            } else {
                ay38910psg_write(&sys->psg, psg_data);
            }
        }

        if (!mos6522via_get_cb2(&sys->via)) {
            mos6522via_set_pa(&sys->via, ay38910psg_read(&sys->psg));
        }

        // PB0..PB2: select keyboard matrix line
        uint8_t pb = mos6522via_get_pb(&sys->via);
        uint8_t line = pb & 7;
        if (line >= 0 && line <= 7) {
            uint8_t line_mask = 1 << line;
            // Any key of this line (another line may have a key down too, e.g. SHIFT)
            const uint8_t data = (kbd_scan_lines(&sys->kbd) & line_mask) ? (pb | (1 << 3)) : (pb & ~(1 << 3));
            // Same input and no PB6 edge pending: the call would change nothing, and
            // it would leave the VIA's lazy mode (timers ticked every 4 cycles)
            if (data != sys->via.pb.inpr || sys->via.pb6_triggered) {
                mos6522via_set_pb(&sys->via, data);
            }
        }

        // Printer port: data on port A, STROBE = PB4 (falling edge), ACK on CA1.
        // As in Oricutron, a printer is always connected: it acknowledges each
        // byte (CA1 up, then down 40 us later)
        uint8_t pb_out = sys->via.pb.outr & sys->via.pb.ddr;
        if ((sys->printer_strobe & 0x10) && !(pb_out & 0x10)) {
            if (sys->printer_cb) {
                sys->printer_cb(sys->via.pa.outr & sys->via.pa.ddr, sys->printer_user_data);
            }
            mos6522via_set_ca1(&sys->via, true);
            sys->printer_ack = 10;
        } else if (sys->printer_ack > 0 && --sys->printer_ack == 0) {
            mos6522via_set_ca1(&sys->via, false);
        }
        sys->printer_strobe = pb_out;

        if (sys->tape.inserted || sys->tape_rec.enabled || ((pb_out & 0x40) != 0) != sys->tape.motor) {
            _oric_update_tape(sys, pb_out);
        }
    }

    sys->system_ticks++;
}

ORIC_HOT void oric_tick(oric_t* sys) { _oric_tick(sys); }

// PSG OUT callback (nothing to do here)
static void _oric_psg_out(int port_id, uint8_t data, void* user_data) {
    oric_t* sys = (oric_t*)user_data;
    if (port_id == AY38910PSG_PORT_A) {
        kbd_set_active_columns(&sys->kbd, data ^ 0xFF);
    } else {
        // This shouldn't happen since the AY-3-8912 only has one IO port
    }
}

// PSG IN callback (read keyboard matrix)
static uint8_t _oric_psg_in(int port_id, void* user_data) {
    // this shouldn't be called
    (void)port_id;
    (void)user_data;
    return 0xFF;
}

void oric_screen_update(oric_t* sys) {
    if (!sys->screen_dirty) {
        return;
    }

    bool blink_state = sys->blink_counter & 0x20;
    sys->blink_counter = (sys->blink_counter + 1) & 0x3F;

    uint8_t pattr = sys->pattr;

    for (int y = 0; y < 224; y++) {
        // Line attributes and current colors
        uint8_t lattr = 0;
        uint8_t fgcol = 7;
        uint8_t bgcol = 0;

        uint8_t* p = &sys->fb[y * (ORIC_SCREEN_WIDTH / 2)];

        for (int x = 0; x < 40; x++) {
            // Lookup the byte and, if needed, the pattern data
            uint8_t ch, pat;
            if ((pattr & PATTR_HIRES) && y < 200)
                ch = pat = sys->ram[0xA000 + y * 40 + x];

            else {
                ch = sys->ram[0xBB80 + (y >> 3) * 40 + x];
                int off = (lattr & LATTR_DSIZE ? y >> 1 : y) & 7;
                const uint8_t* base;
                if (pattr & PATTR_HIRES)
                    if (lattr & LATTR_ALT)
                        base = sys->ram + 0x9C00;
                    else
                        base = sys->ram + 0x9800;
                else if (lattr & LATTR_ALT)
                    base = sys->ram + 0xB800;
                else
                    base = sys->ram + 0xB400;
                pat = base[((ch & 0x7F) << 3) | off];
            }

            // Handle state-chaging attributes
            if (!(ch & 0x60)) {
                pat = 0x00;
                switch (ch & 0x18) {
                    case 0x00:
                        fgcol = ch & 7;
                        break;
                    case 0x08:
                        lattr = ch & 7;
                        break;
                    case 0x10:
                        bgcol = ch & 7;
                        break;
                    case 0x18:
                        pattr = ch & 7;
                        break;
                }
            }

            // Pick up the colors for the pattern
            uint8_t c_fgcol = fgcol;
            uint8_t c_bgcol = bgcol;

            // inverse video
            if (ch & 0x80) {
                c_bgcol = c_bgcol ^ 0x07;
                c_fgcol = c_fgcol ^ 0x07;
            }
            // blink
            if ((lattr & LATTR_BLINK) && blink_state) c_fgcol = c_bgcol;

            // Draw the pattern
            uint8_t c;
            c = pat & 0x20 ? c_fgcol : c_bgcol;
            *p = c << 4;
            c = pat & 0x10 ? c_fgcol : c_bgcol;
            *p++ |= c;
            c = pat & 0x08 ? c_fgcol : c_bgcol;
            *p = c << 4;
            c = pat & 0x04 ? c_fgcol : c_bgcol;
            *p++ |= c;
            c = pat & 0x02 ? c_fgcol : c_bgcol;
            *p = c << 4;
            c = pat & 0x01 ? c_fgcol : c_bgcol;
            *p++ |= c;
        }
    }

    sys->pattr = pattr;

    sys->screen_dirty = false;
}

uint32_t oric_exec(oric_t* sys, uint32_t micro_seconds) {
    CHIPS_ASSERT(sys && sys->valid);
    uint32_t num_ticks = clk_us_to_ticks(ORIC_FREQUENCY, micro_seconds);
    if (0 == sys->debug.callback.func) {
        // run without debug callback
        for (uint32_t ticks = 0; ticks < num_ticks; ticks++) {
            oric_tick(sys);
        }
    } else {
        // run with debug callback
        for (uint32_t ticks = 0; (ticks < num_ticks) && !(*sys->debug.stopped); ticks++) {
            oric_tick(sys);
            sys->debug.callback.func(sys->debug.callback.user_data, 0);
        }
    }
    kbd_update(&sys->kbd, micro_seconds);
    oric_screen_update(sys);
    return num_ticks;
}

static void _oric_init_memorymap(oric_t* sys) {
    mem_init(&sys->mem);
    // Power-on RAM pattern (as Oricutron): in each 256 byte page, 128 bytes $00 then 128 bytes $FF.
    // Sedoric's loader relies on it: an all-zero RAM makes it load only 4 sectors
    for (uint32_t i = 0; i < sizeof(sys->ram); i++) {
        sys->ram[i] = (i & 0x80) ? 0xFF : 0x00;
    }
    for (uint32_t i = 0; i < sizeof(sys->overlay_ram); i++) {
        sys->overlay_ram[i] = (i & 0x80) ? 0xFF : 0x00;
    }
    mem_map_ram(&sys->mem, 0, 0x0000, 0xC000, sys->ram);
    _oric_update_map(sys);
}

static void _oric_init_key_map(oric_t* sys) {
    kbd_init(&sys->kbd, 2);
    const char* keymap =
        // no shift
        //   01234567 (col)
        "7N5V 1X3"   // row 0
        "JTRF  QD"   // row 1
        "M6B4 Z2C"   // row 2
        "K9;-  \\'"  // row 3
        " ,.     "   // row 4 (unshifted , and .)
        "UIOP  ]["   // row 5
        "YHGE ASW"   // row 6
        "8L0/   ="   // row 7

        /* shift */
        "&n%v !x#"
        "jtrf  qd"
        "m^b$ z@c"
        "k(:_  |\""
        " <>     "
        "uiop  }{"
        "yhge asw"
        "*l)?   +";

    CHIPS_ASSERT(strlen(keymap) == 128);
    // shift is column 4, line 4
    kbd_register_modifier(&sys->kbd, 0, 4, 4);
    // ctrl is column 4, line 2
    kbd_register_modifier(&sys->kbd, 1, 4, 2);
    for (int shift = 0; shift < 2; shift++) {
        for (int column = 0; column < 8; column++) {
            for (int line = 0; line < 8; line++) {
                int c = keymap[shift * 64 + line * 8 + column];
                if (c != 0x20) {
                    kbd_register_key(&sys->kbd, c, column, line, shift ? (1 << 0) : 0);
                }
            }
        }
    }

    // Special keys
    kbd_register_key(&sys->kbd, 0x20, 0, 4, 0);   // Space
    kbd_register_key(&sys->kbd, 0x150, 5, 4, 0);  // Left
    kbd_register_key(&sys->kbd, 0x14F, 7, 4, 0);  // Right
    kbd_register_key(&sys->kbd, 0x151, 6, 4, 0);  // Down
    kbd_register_key(&sys->kbd, 0x152, 3, 4, 0);  // Up
    kbd_register_key(&sys->kbd, 0x08, 5, 5, 0);   // Delete
    kbd_register_key(&sys->kbd, 0x0D, 5, 7, 0);   // Return

    kbd_register_key(&sys->kbd, 0x14, 1, 1, 2);  // Ctrl+T
    kbd_register_key(&sys->kbd, 0x10, 3, 5, 2);  // Ctrl+P
    kbd_register_key(&sys->kbd, 0x06, 3, 1, 2);  // Ctrl+F
    kbd_register_key(&sys->kbd, 0x04, 7, 1, 2);  // Ctrl+D
    kbd_register_key(&sys->kbd, 0x11, 6, 1, 2);  // Ctrl+Q
    kbd_register_key(&sys->kbd, 0x13, 6, 6, 2);  // Ctrl+S
    kbd_register_key(&sys->kbd, 0x0C, 1, 7, 2);  // Ctrl+L
    kbd_register_key(&sys->kbd, 0x0E, 1, 0, 2);  // Ctrl+N
}

void oric_key_up(oric_t* sys, int key_code) {
    CHIPS_ASSERT(sys && sys->valid);
    kbd_key_up(&sys->kbd, key_code);
}

uint32_t oric_save_snapshot(oric_t* sys, oric_t* dst) {
    CHIPS_ASSERT(sys && dst);
    *dst = *sys;
    chips_debug_snapshot_onsave(&dst->debug);
    chips_audio_callback_snapshot_onsave(&dst->audio_callback);
    // m6502_snapshot_onsave(&dst->cpu);
    ay38910psg_snapshot_onsave(&dst->psg);
    disk2_fdc_snapshot_onsave(&dst->fdc);
    mem_snapshot_onsave(&dst->mem, sys);
    return ORIC_SNAPSHOT_VERSION;
}

bool oric_load_snapshot(oric_t* sys, uint32_t version, oric_t* src) {
    CHIPS_ASSERT(sys && src);
    if (version != ORIC_SNAPSHOT_VERSION) {
        return false;
    }
    static oric_t im;
    im = *src;
    chips_debug_snapshot_onload(&im.debug, &sys->debug);
    chips_audio_callback_snapshot_onload(&im.audio_callback, &sys->audio_callback);
    // m6502_snapshot_onload(&im.cpu, &sys->cpu);
    ay38910psg_snapshot_onload(&im.psg, &sys->psg);
    // The tape stays that of the running machine (read callback, file being written)
    im.tape = sys->tape;
    im.tape_rec = sys->tape_rec;
    im.tape_due = sys->tape_due;
    disk2_fdc_snapshot_onload(&im.fdc, &sys->fdc);
    // Disks stay those of the running machine
    memcpy(im.wd.disk, sys->wd.disk, sizeof(im.wd.disk));
    memcpy(im.wd.present, sys->wd.present, sizeof(im.wd.present));
    oric_dsk_invalidate(&sys->wd.cache, 0);
    im.wd.cache.disk = 0;
    im.wd.cache.raw = 0;
    im.wd.cache.num_sectors = 0;
    mem_snapshot_onload(&im.mem, sys);
    *sys = im;
    return true;
}

#endif  // CHIPS_IMPL
