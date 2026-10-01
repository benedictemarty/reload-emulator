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
#include <strings.h>
#include <ctype.h>

#include <pico/platform.h>
#include "pico/stdlib.h"

#include "roms/pravetz8d_roms.h"
// Microdisc / Jasmin ROMs: optional
#if __has_include("roms/oric_microdisc_rom.h")
#include "roms/oric_microdisc_rom.h"
#define HAVE_MICRODISC_ROM 1
#endif
#if __has_include("roms/oric_jasmin_rom.h")
#include "roms/oric_jasmin_rom.h"
#define HAVE_JASMIN_ROM 1
#endif
#include "images/oric_images.h"

// Code run every emulated cycle goes to RAM (as the BBC does): from XIP flash,
// the bus driver, memory decoding, VIA, PSG, tape and FDC ticks miss the 16 KB
// cache. -DORIC_COLD leaves them in flash (before/after measurements)
#ifndef ORIC_COLD
#define ORIC_HOT     __attribute__((section(".time_critical.oric")))
#define CHIPS_HOT    __attribute__((section(".time_critical.oric")))
#define WDC65C02_HOT __attribute__((section(".time_critical.oric")))
#endif

#include "chips/chips_common.h"
#ifdef OLIMEX_NEO6502
#include "chips/wdc65C02cpu.h"
// Bus of the real 65C02 inlined in oric_tick (-DORIC_BUS_PIO: PIO state machine, as the
// BBC, the default of the oric and oric_diag builds; -DORIC_BUS_INLINE: SIO, as
// wdc65C02cpu.h); neither: wdc65C02cpu.h calls
#ifdef ORIC_BUS_PIO
#define WDC65C02_BUS_PIO 1
#endif
#if defined(ORIC_BUS_PIO) || defined(ORIC_BUS_INLINE)
#include "chips/wdc65C02bus.h"
#endif
#else
#include "chips/mos6502cpu.h"
#endif
#ifdef ORIC_VIA6522
#include "chips/via6522.h"   // Trial of the common VIA (Neo6502Vic20)
#else
#include "chips/mos6522via.h"
#endif
#include "chips/ay38910psg.h"
#include "chips/kbd.h"
#include "chips/mem.h"
#include "chips/clk.h"
#include "devices/oric_tape.h"
#include "devices/oric_tape_rec.h"
#include "devices/oric_tape_turbo.h"
#include "devices/disk2_fdd.h"
#include "devices/disk2_fdc.h"
#include "devices/oric_fdc_rom.h"
#include "devices/oric_dsk.h"
#include "devices/wd1793.h"
#define NEO_STORAGE_IMPL
#include "devices/neo_storage.h"
#define NEO_TNFS_IMPL
#include "devices/neo_tnfs.h"
#define NEO_ESP_AT_IMPL
#include "devices/neo_esp_at.h"
#define NEO_DGRAM_SERIAL_IMPL
#include "devices/neo_dgram_serial.h"
#include "systems/oric.h"
#define ORIC_PLANES_RAM __attribute__((section(".time_critical.oric_planes")))
#include "systems/oric_planes.h"

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
#include "ff.h"
extern const neo_storage_ops_t neo_storage_fatfs_ops;   // neo_storage_fatfs.c (socle)
// Wi-Fi modem on the USB hub (Neo6502picowifi): neo_cdc_serial.c (socle)
bool neo_cdc_ready(void);
bool neo_cdc_port_ready(int port);
uint32_t neo_cdc_mount_count(void);
bool neo_cdc_write(void *ctx, const uint8_t *p, int n);
int neo_cdc_read_byte(void *ctx, uint32_t timeout_us);

// Control panel (F1): grid of 8 x 8 cells covering the output, each buffer line shown twice
#ifdef OLIMEX_NEO6502
#define OSD_COLS 100   // 800 x 480
#define OSD_ROWS 30
#else
#define OSD_COLS 120   // 960 x 544
#define OSD_ROWS 34
#endif
// Plain panel backgrounds: the dithered ones (one pixel out of two) gave red
// streaks on the Neo6502 HDMI link (bmarty's screen, 2026-09-30)
#define OSD_NO_DITHER 1
#define OSD_HOT          __attribute__((section(".time_critical.osd")))
#define OSD_FONT_SECTION __attribute__((section(".time_critical.osd_font")))
#include "osd/oric_menu.h"
#include "osd/oric_config.h"

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

// Control panel (F1): menu state, surface drawn by core 1, open flag
static oric_menu_t __not_in_flash() menu;
// Double-buffered surface: core 0 draws the hidden one, then shows it; core 1
// takes the shown one at the start of each frame (no half-drawn panel on screen)
static osd_surface_t __not_in_flash() osd_surfaces[2];
static volatile uint8_t osd_front;            // Surface shown by core 1
static volatile uint32_t core1_frames;        // Frames started by core 1
// Tape banner (bottom border, while the tape motor runs), double-buffered too
static osd_row_t __not_in_flash() banner_rows[2];
static volatile uint8_t banner_front;
static volatile bool banner_on;
static void profiles_list(void);               // Profiles offered by the panel (built in, then ORIC.CFG)
static volatile bool panel_open;

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
#ifdef HAVE_MICRODISC_ROM
                .microdisc_rom = {.ptr = oric_microdisc_rom, .size = sizeof(oric_microdisc_rom)},
#endif
#ifdef HAVE_JASMIN_ROM
                .jasmin_rom = {.ptr = oric_jasmin_rom, .size = sizeof(oric_jasmin_rom)},
#endif
            },
    };
}

void app_init(void) {
    (void)diag_layout[0];   // Kept by the linker for the host tools
    oric_desc_t desc = oric_desc();
    neo_storage_set(NEO_VOL_USB, "Clé USB", &neo_storage_fatfs_ops, "");   // Served once the drive is mounted (usb_poll)
    oric_init(&state.oric, &desc);
    oric_set_sample_rate_q8(&state.oric, audio_pwm_rate_q8(22050));   // The rate the PWM really consumes
    oric_menu_init(&menu);
    menu.version = "reload";
    menu.fdc = (int)state.oric.fdc_type;
    menu.fdc_available[ORIC_FDC_PRAVETZ] = true;
#ifdef HAVE_MICRODISC_ROM
    menu.fdc_available[ORIC_FDC_MICRODISC] = true;
#endif
#ifdef HAVE_JASMIN_ROM
    menu.fdc_available[ORIC_FDC_JASMIN] = true;
#endif
    profiles_list();
}

#if defined(OLIMEX_NEO6502) && !defined(ORIC_VIDEO_295)
// Default: the 800x480 picture at a 345 MHz system clock (blanking stretched, as the
// BBC build; neo_dvi_timings.h), 17 % more CPU time per emulated cycle: 3D Fongus
// takes 89.6 % of the frame instead of 105.1 % at 295.2 MHz (measured 2026-09-30).
// -DORIC_VIDEO_295: the standard 800x480 timing at 295.2 MHz. DVDD 1.25V
#include "neo_dvi_timings.h"
#define FRAME_WIDTH  800
#define FRAME_HEIGHT 480
#define VREG_VSEL    VREG_VOLTAGE_1_25
#define DVI_TIMING   dvi_timing_800x480p_60hz_345
#elif defined(OLIMEX_NEO6502)
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

uint32_t __not_in_flash() empty_tmdsbuf[3 * FRAME_WIDTH / DVI_SYMBOLS_PER_WORD];


struct dvi_inst dvi0;

extern void copy_tmdsbuf(uint32_t *dest, const uint32_t *src);

static void panel_toggle(void);
static void panel_refresh(void);
// Keys of the open panel, queued by the USB HID callback and handled by the main
// loop (the actions open files on the USB drive: not from inside a USB callback)
static volatile uint16_t panel_keys[16];
static volatile uint8_t panel_keys_head, panel_keys_tail;

void kbd_raw_key_down(int code) {
    if (code == (NEO_MULTIBOOT_RETURN_KEY | 0x100)) neo_multiboot_return();  // Pause (code HID | 0x100 : pas d'ASCII) : retour au firmware Neo6502
    if (code == 0x13A) {   // F1: control panel
        panel_toggle();
        return;
    }
    if (panel_open) {
        if ((uint8_t)(panel_keys_tail - panel_keys_head) < 16) {
            panel_keys[panel_keys_tail & 15] = (uint16_t)code;
            panel_keys_tail++;
        }
        return;
    }
    if (isascii(code)) {
        if (isupper(code)) {
            code = tolower(code);
        } else if (islower(code)) {
            code = toupper(code);
        }
    }

    oric_t *sys = &state.oric;

    switch (code) {
        case 0x13B:  // F2
        case 0x13C:  // F3
        case 0x13D:  // F4
        case 0x13E:  // F5
        case 0x13F:  // F6
        case 0x140:  // F7
        case 0x141:  // F8
        case 0x142:  // F9
        {
            uint8_t index = code - 0x13B;   // F2: first image (F1 opens the control panel)
            int num_nib_images = CHIPS_ARRAY_SIZE(oric_nib_images);
            if (index < num_nib_images) {
                if (sys->fdc.valid) {
                    disk2_fdd_insert_disk(&sys->fdc.fdd[0], oric_nib_images[index]);
                }
            } else {
                index -= num_nib_images;
                if (index < CHIPS_ARRAY_SIZE(oric_tap_images)) {
                    oric_insert_tap(sys, oric_tap_images[index], oric_tap_image_sizes[index]);
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
// Emulation alone (the 19968 ticks of a frame, panel closed): sum, frames, slowest
volatile uint32_t diag_tick_us, diag_tick_frames, diag_tick_max;
// Start-up micro-benchmarks, 40 000 cycles each (us): [0] bus cycles alone (65C02
// answered from RAM), [1] full oric_tick (bus, memory decoding, VIA, PSG, tape…)
volatile uint32_t diag_bench[2];

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

/*-- USB drive: disk and tape images streamed from files ----------------------*/

extern bool msc_inquiry_complete;
extern void msc_poll(void);

static bool usb_scanned;
// Network settings of ORIC.CFG (reseau, wifi, wifi_mdp), used by net_poll
static char net_server[64], net_ssid[33], net_pass[64];
// Files of the USB drive through neo_storage (volume NEO_VOL_USB, FatFs driver of the socle)
static neo_file_t dsk_file[ORIC_MENU_DRIVES];
static neo_file_t tap_file;

static bool has_ext(const char *name, const char *ext) {
    size_t n = strlen(name), e = strlen(ext);
    if (n < e) return false;
    for (size_t i = 0; i < e; i++) {
        char c = name[n - e + i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c != ext[i]) return false;
    }
    return true;
}

static void usb_scan(void);

// CSAVE: one NAME.TAP file per recording in the root of the USB drive (oric_tape_rec.h)
static neo_file_t rec_file;
static bool rec_file_open(void *ctx, const char *name) {
    (void)ctx;
    neo_file_close(&rec_file);
    return neo_file_open(&rec_file, NEO_VOL_USB, name, NEO_WRITE | NEO_CREATE);
}
static void rec_file_write(void *ctx, const uint8_t *data, uint32_t len) {
    (void)ctx;
    if (neo_file_is_open(&rec_file)) neo_file_append(&rec_file, data, len);
}
static void rec_file_close(void *ctx) {
    (void)ctx;
    neo_file_close(&rec_file);
    usb_scan();   // List the new file (the configuration is not applied again)
}
static const oric_tape_rec_out_t rec_out = {rec_file_open, rec_file_write, rec_file_close, 0};

static void dsk_close(int d) {
    wd1793_eject(&state.oric.wd, d);   // Writes the cached track back first
    neo_file_close(&dsk_file[d]);
    menu.drive[d][0] = 0;
    menu.drive_ro[d] = false;
}

static bool dsk_insert(int d, const char *name) {
    dsk_close(d);
    bool ro = false;
    neo_file_t *f = &dsk_file[d];
    if (!neo_file_open_path(f, name, NEO_READ | NEO_WRITE)) {
        if (!neo_file_open_path(f, name, NEO_READ)) return false;
        ro = true;
    }
    uint8_t header[ORIC_DSK_HEADER_SIZE];
    if (!neo_file_read(f, 0, header, sizeof(header)) ||
        !wd1793_insert_streamed(&state.oric.wd, d, header, f->size, neo_file_read_cb, ro ? 0 : neo_file_write_cb, f)) {
        neo_file_close(f);
        return false;
    }
    snprintf(menu.drive[d], sizeof(menu.drive[d]), "%s", name);
    menu.drive_ro[d] = ro;
    return true;
}

static void tap_close(void) {
    oric_eject_tape(&state.oric);
    neo_file_close(&tap_file);
    menu.tape[0] = 0;
}

static bool tap_insert(const char *name) {
    if (neo_file_is_open(&tap_file) && !strcmp(menu.tape, name)) {
        oric_tape_rewind(&state.oric.tape);   // The same tape: rewound
        return true;
    }
    tap_close();
    if (!neo_file_open_path(&tap_file, name, NEO_READ)) return false;
    if (!oric_insert_tape(&state.oric, tap_file.size, neo_file_read_cb, &tap_file)) {
        tap_close();
        return false;
    }
    snprintf(menu.tape, sizeof(menu.tape), "%s", name);
    return true;
}

// .dsk and .tap files of the root of the USB drive, then of the network volume
// (named "net:/NAME", user = that prefix), sorted by name
static bool usb_scan_entry(const neo_dirent_t *e, void *user) {
    const char *prefix = (const char *)user;
    if (menu.nfiles >= ORIC_MENU_FILES) return false;
    if (e->dir || e->hidden) return true;
    int kind = has_ext(e->name, ".dsk") ? ORIC_FILE_DSK : has_ext(e->name, ".tap") ? ORIC_FILE_TAP : -1;
    if (kind < 0) return true;
    oric_menu_file_t *f = &menu.files[menu.nfiles++];
    snprintf(f->name, sizeof(f->name), "%s%s", prefix ? prefix : "", e->name);
    f->size = e->size;
    f->kind = (uint8_t)kind;
    return true;
}

static void usb_scan(void) {
    menu.nfiles = 0;
    const bool usb = neo_storage_list(NEO_VOL_USB, "/", usb_scan_entry, 0);
    const bool net = neo_storage_list(NEO_VOL_NET, "/", usb_scan_entry, (void *)NEO_NET_PREFIX "/");
    if (!usb && !net) return;
    for (int i = 1; i < menu.nfiles; i++) {
        oric_menu_file_t t = menu.files[i];
        int j = i - 1;
        while (j >= 0 && strcmp(menu.files[j].name, t.name) > 0) {
            menu.files[j + 1] = menu.files[j];
            j--;
        }
        menu.files[j + 1] = t;
    }
    snprintf(menu.usb_label, sizeof(menu.usb_label), "%s", usb && net ? "Clé montée + réseau" : usb ? "Clé montée" : "Réseau");
    menu.usb_present = true;
    printf("USB: %d image(s)\n", menu.nfiles);
}

static bool panel_set_fdc(int type) {
    wd1793_flush(&state.oric.wd);
    if (!oric_set_fdc(&state.oric, (oric_fdc_type_t)type)) return false;
    menu.fdc = type;
    return true;
}

/*-- Profiles: built in, then those of ORIC.CFG (profil=) ------------------------*/

#define BUILTIN_PROFILES 4
static const oric_profile_t builtin_profiles[BUILTIN_PROFILES] = {
    {.label = "Oric Atmos (BASIC 1.1)", .fdc = ORIC_FDC_NONE},
    {.label = "Atmos + Microdisc (Sedoric)", .fdc = ORIC_FDC_MICRODISC},
    {.label = "Atmos + Jasmin (FT-DOS)", .fdc = ORIC_FDC_JASMIN},
    {.label = "Pravetz 8D (Disk II)", .fdc = ORIC_FDC_PRAVETZ},
};
static const char *const builtin_keys[BUILTIN_PROFILES] = {"atmos", "microdisc", "jasmin", "pravetz"};
static oric_profile_t usb_profiles[ORIC_CONFIG_PROFILES];
static int usb_nprofiles;
static const oric_profile_t *profile_at[ORIC_MENU_PROFILES];
static int nprofiles;
static uint8_t __not_in_flash() usb_rom[0x4000];   // ROM of a profile (rom=), loaded from the USB drive

static void profiles_list(void) {
    nprofiles = 0;
    for (int k = 0; k < BUILTIN_PROFILES; k++) {
        int fdc = builtin_profiles[k].fdc;
        if (fdc > 0 && !menu.fdc_available[fdc]) continue;   // ROM of the interface missing
        profile_at[nprofiles++] = &builtin_profiles[k];
    }
    for (int k = 0; k < usb_nprofiles && nprofiles < ORIC_MENU_PROFILES; k++) profile_at[nprofiles++] = &usb_profiles[k];
    for (int k = 0; k < ORIC_MENU_PROFILES; k++) menu.profile[k] = k < nprofiles ? profile_at[k]->label : NULL;
}

// Profile of a « demarrage= » value: label (any case) or key of a built-in profile; -1 if none
static int profile_find(const char *v) {
    for (int k = 0; k < nprofiles; k++) {
        if (!strcasecmp(v, profile_at[k]->label)) return k;
        for (int b = 0; b < BUILTIN_PROFILES; b++) {
            if (profile_at[k] == &builtin_profiles[b] && !strcasecmp(v, builtin_keys[b])) return k;
        }
    }
    return -1;
}

static bool rom_load(const char *name) {
    uint32_t n;
    int vol;
    const char *p = neo_storage_split(name, &vol);
    return neo_file_load(vol, p, usb_rom, sizeof(usb_rom), &n, true);
}

// Apply a profile: ROM, interface, drives and tape (those not given are emptied), then reset
static void profile_apply(int k) {
    char msg[96];
    if (k < 0 || k >= nprofiles) return;
    const oric_profile_t *p = profile_at[k];
    bool ok = true;
    if (p->rom[0]) {
        if (rom_load(p->rom)) {
            oric_set_rom(&state.oric, usb_rom);
        } else {
            snprintf(msg, sizeof(msg), "%.40s : ROM de 16 Ko introuvable", p->rom);
            oric_menu_message(&menu, true, msg);
            ok = false;
        }
    } else {
        oric_set_rom(&state.oric, oric_rom);
    }
    if (p->fdc >= 0) panel_set_fdc(p->fdc);
    for (int d = 0; d < ORIC_MENU_DRIVES; d++) {
        dsk_close(d);
        if (p->drive[d][0] && !dsk_insert(d, p->drive[d])) {
            snprintf(msg, sizeof(msg), "%.40s : disquette introuvable", p->drive[d]);
            oric_menu_message(&menu, true, msg);
            ok = false;
        }
    }
    tap_close();
    if (p->tape[0] && !tap_insert(p->tape)) {
        snprintf(msg, sizeof(msg), "%.40s : cassette introuvable", p->tape);
        oric_menu_message(&menu, true, msg);
        ok = false;
    }
    oric_reset(&state.oric);
    menu.profile_cur = k;
    if (ok) {
        snprintf(msg, sizeof(msg), "Profil %.60s : l'Oric redémarre", p->label);
        oric_menu_message(&menu, false, msg);
    }
    printf("Profile: %s\n", p->label);
}

// ORIC.CFG of the USB drive: user settings (interface, drives, tape), profiles
// and start choice; demarrage=choix opens the "Démarrer sur…" page
// Contents of ORIC.CFG, read whole (config_load, then config_save for the merge)
static char config_text[1536];

static void config_load(void) {
    char *text = config_text;
    uint32_t len;
    usb_nprofiles = 0;
    if (!neo_file_load(NEO_VOL_USB, ORIC_CONFIG_FILE, (uint8_t *)text, sizeof(config_text) - 1, &len, false)) {
        profiles_list();
        return;
    }
    text[len] = 0;
    char line[160];
    const char *next = text;
    int fdc = -1;
    static char drives[ORIC_MENU_DRIVES][ORIC_MENU_NAME_LEN];
    static char tape[ORIC_MENU_NAME_LEN];
    static char start[ORIC_CONFIG_LABEL_LEN];
    memset(drives, 0, sizeof(drives));
    tape[0] = 0;
    start[0] = 0;
    while (*next) {
        // One line (f_gets-like: kept with its end of line, cut at 159 characters)
        size_t n = 0;
        while (next[n] && next[n] != '\n') n++;
        const size_t take = n + (next[n] == '\n') < sizeof(line) - 1 ? n + (next[n] == '\n') : sizeof(line) - 1;
        memcpy(line, next, take);
        line[take] = 0;
        next += n + (next[n] == '\n');
        const char *v;
        if ((v = oric_config_value(line, "fdc"))) {
            fdc = oric_config_fdc(v);
        } else if ((v = oric_config_value(line, "tape"))) {
            oric_config_copy(tape, sizeof(tape), v);
        } else if ((v = oric_config_value(line, "profil"))) {
            if (usb_nprofiles < ORIC_CONFIG_PROFILES && oric_config_profile(v, &usb_profiles[usb_nprofiles])) usb_nprofiles++;
        } else if ((v = oric_config_value(line, "demarrage"))) {
            oric_config_copy(start, sizeof(start), v);
        } else if ((v = oric_config_value(line, "reseau"))) {
            oric_config_copy(net_server, sizeof(net_server), v);
        } else if ((v = oric_config_value(line, "wifi"))) {
            oric_config_copy(net_ssid, sizeof(net_ssid), v);
        } else if ((v = oric_config_value(line, "wifi_mdp"))) {
            oric_config_copy(net_pass, sizeof(net_pass), v);
        } else if ((v = oric_config_value(line, "volume"))) {
            const int level = oric_config_volume(v, HID_VOLUME_MAX);
            if (level >= 0) audio_set_volume((uint8_t)level, false);
        } else {
            for (int d = 0; d < ORIC_MENU_DRIVES; d++) {
                const char key[2] = {(char)('a' + d), 0};
                if ((v = oric_config_value(line, key))) oric_config_copy(drives[d], sizeof(drives[d]), v);
            }
        }
    }
    profiles_list();
    int k = start[0] ? profile_find(start) : -1;
    if (k >= 0) {
        profile_apply(k);
        return;
    }
    if (fdc >= 0) panel_set_fdc(fdc);
    for (int d = 0; d < ORIC_MENU_DRIVES; d++) {
        if (drives[d][0] && !dsk_insert(d, drives[d])) printf("ORIC.CFG: cannot open %s\n", drives[d]);
    }
    if (tape[0] && !tap_insert(tape)) printf("ORIC.CFG: cannot open %s\n", tape);
    oric_reset(&state.oric);   // Boot from the disk in drive A
    if (!strcasecmp(start, "choix")) {
        // "Démarrer sur…" page, right after the USB drive is mounted
        menu.message[0] = 0;
        oric_menu_open_profiles(&menu);
        panel_refresh();
        panel_open = true;
    }
    printf("ORIC.CFG applied\n");
}

static void config_save(void) {
    static char out[1536];
    char *old = config_text;
    uint32_t n = 0;
    if (!neo_file_load(NEO_VOL_USB, ORIC_CONFIG_FILE, (uint8_t *)old, sizeof(config_text) - 1, &n, false)) n = 0;
    old[n] = 0;
    const char *drives[ORIC_MENU_DRIVES] = {menu.drive[0], menu.drive[1], menu.drive[2], menu.drive[3]};
    size_t len = oric_config_merge(old, menu.fdc, drives, menu.tape, audio_volume_level(), out, sizeof(out));
    bool ok = neo_file_save(NEO_VOL_USB, ORIC_CONFIG_FILE, (const uint8_t *)out, (uint32_t)len);
    oric_menu_message(&menu, !ok, ok ? "Configuration enregistrée dans ORIC.CFG" : "ORIC.CFG : écriture impossible");
}

/*-- Network volume (TNFS through the Wi-Fi modem Neo6502picowifi) -----------*/
// ORIC.CFG: reseau=SERVER[:PORT] (TNFS server), and optionally wifi=NETWORK and
// wifi_mdp=PASSWORD (otherwise the network saved in the modem). Files on the
// server are listed and named "net:/NAME" (docs/RESEAU-NEO6502.md).
static neo_esp_t net_esp;
static neo_dgram_t net_dgram;
static neo_tnfs_t net_tnfs;
static uint32_t net_tried;   // Modem mount count of the last attempt
static bool net_usb_enabled;  // AT$TNFSUSB=1 sent once (the modem then restarts)

static void net_poll(void) {
    if (net_tnfs.mounted && !neo_cdc_ready()) {
        net_tnfs.mounted = false;   // Modem pulled out: the volume is no longer ready
        usb_scan();
        oric_menu_message(&menu, true, "Réseau : modem retiré");
        return;
    }
    if (!net_server[0] || net_tnfs.mounted || !neo_cdc_ready() || neo_cdc_mount_count() == net_tried) return;
    net_tried = neo_cdc_mount_count();   // One attempt per plugging of the modem (it blocks the emulation meanwhile)
    char host[64];
    snprintf(host, sizeof(host), "%s", net_server);
    char *colon = strchr(host, ':');
    const uint16_t port = colon ? (uint16_t)atoi(colon + 1) : NEO_TNFS_PORT;
    if (colon) *colon = 0;
    neo_esp_init(&net_esp, neo_cdc_write, neo_cdc_read_byte, (void *)0);
    // Second serial port of the modem (Neo6502picowifi eeda7d3, PicoWiFiModemUSB): TNFS in
    // frames there, the server given by AT$TNFS, and the AT port stays free (Minitel).
    // Otherwise (one port only, or not mounted: the USB host has 15 endpoints in all), AT UDP.
    bool relay = neo_cdc_port_ready(1);
    bool ok = neo_esp_prepare(&net_esp, net_ssid[0] ? net_ssid : 0, net_pass);
    if (ok && !relay && !net_usb_enabled && neo_esp_tnfs_usb(&net_esp) == 0) {
        // The modem has a TNFS port, disabled (default of Neo6502picowifi): enabled once,
        // in its flash, then it restarts; the next attempt comes with its new enumeration
        net_usb_enabled = true;
        if (neo_esp_enable_tnfs_usb(&net_esp)) {
            oric_menu_message(&menu, false, "Réseau : port TNFS du modem activé, redémarrage du modem");
            return;
        }
    }
    if (ok && relay) {
        neo_dgram_init(&net_dgram, neo_cdc_write, neo_cdc_read_byte, (void *)1);
        neo_tnfs_init(&net_tnfs, neo_dgram_xfer, &net_dgram);
        ok = neo_esp_tnfs_server(&net_esp, host, port);
    } else if (ok) {
        // One port only (old firmware, or the TNFS port did not mount: 15 endpoints in all)
        neo_tnfs_init(&net_tnfs, neo_esp_xfer, &net_esp);
        ok = neo_esp_udp_start(&net_esp, host, port);
    }
    ok = ok && neo_tnfs_mount(&net_tnfs, "/", 0, 0);
    if (ok) {
        neo_storage_set(NEO_VOL_NET, "Réseau", &neo_storage_tnfs_ops, &net_tnfs);
        usb_scan();
    }
    printf("Network: %s %s (%s)\n", net_server, ok ? "mounted" : "failed", relay ? "TNFS port" : "AT UDP");
    oric_menu_message(&menu, !ok, ok ? "Réseau monté" : "Réseau : serveur injoignable");
}

// Called every frame: once the USB drive is mounted, list its images and apply ORIC.CFG
static void usb_poll(void) {
    msc_poll();
    if (usb_scanned || !msc_inquiry_complete) return;
    usb_scanned = true;
    usb_scan();
    oric_tape_recorder(&state.oric, &rec_out);   // CSAVE to the USB drive
    config_load();
}

#ifdef ORIC_DIAG
/*-- Bench: files written to the USB drive over SWD (tools/oric_carte.py envoyer) -*/
// The host writes the name / a chunk, then the command; core 0 runs it in the
// main loop and clears diag_file_cmd (result in diag_file_status: FatFs code)
#define DIAG_FILE_CHUNK 2048   // tools/oric_carte.py reads the size from the ELF (was 8192: RAM of oric_diag)
enum { DIAG_FILE_CREATE = 1, DIAG_FILE_WRITE = 2, DIAG_FILE_CLOSE = 3, DIAG_FILE_RESCAN = 4 };
volatile uint32_t diag_file_cmd, diag_file_status, diag_file_len;
volatile char diag_file_name[64];
uint8_t diag_file_buf[DIAG_FILE_CHUNK];
static neo_file_t diag_file;

static void diag_file_service(void) {
    uint32_t cmd = diag_file_cmd;
    if (!cmd) return;
    __dmb();
    uint32_t r = 0;   // 0: done, 1: failed
    switch (cmd) {
        case DIAG_FILE_CREATE:
            neo_file_close(&diag_file);
            r = !neo_file_open(&diag_file, NEO_VOL_USB, (const char *)diag_file_name, NEO_WRITE | NEO_CREATE);
            break;
        case DIAG_FILE_WRITE:
            r = diag_file_len > DIAG_FILE_CHUNK || !neo_file_append(&diag_file, diag_file_buf, diag_file_len);
            break;
        case DIAG_FILE_CLOSE:
            r = !neo_file_close(&diag_file);
            break;
        case DIAG_FILE_RESCAN:
            usb_scanned = false;   // usb_poll lists the files again and applies ORIC.CFG
            break;
        default:
            r = 1;
            break;
    }
    diag_file_status = r;
    __dmb();
    diag_file_cmd = 0;
}
#endif

/*-- Tape banner ----------------------------------------------------------------*/
// While the tape motor runs: name and position of the tape in the bottom border
static void banner_update(void) {
    // Volume (multimedia keys of the keyboard): for 2 s, in place of the tape banner
    static uint32_t volume_serial;
    static int volume_frames;
    if (audio_volume_serial() != volume_serial) {
        volume_serial = audio_volume_serial();
        volume_frames = 100;
        const uint8_t back = (uint8_t)(banner_front ^ 1);
        oric_volume_banner(&banner_rows[back], audio_volume_level(), audio_volume_muted());
        __dmb();
        banner_front = back;
        banner_on = true;
        if (panel_open) panel_refresh();
        return;
    }
    if (volume_frames > 0) {
        if (--volume_frames > 0) {
            banner_on = true;
            return;
        }
        banner_on = false;   // End of the volume banner: the tape banner is drawn again
    }
    oric_tape_t *tp = &state.oric.tape;
    const bool on = menu.tape[0] && oric_tape_running(tp);
    if (on) {
        static uint32_t last;
        const int percent = oric_tape_percent(tp);
        if (!banner_on || (uint32_t)percent != last) {
            const uint8_t back = (uint8_t)(banner_front ^ 1);
            oric_tape_banner(&banner_rows[back], menu.tape, percent);
            __dmb();
            banner_front = back;
            last = (uint32_t)percent;
        }
    }
    banner_on = on;
}

/*-- Control panel (F1) --------------------------------------------------------*/
// Core 0 builds the surface (osd), core 1 draws it in place of the Oric picture
// while panel_open is set. The emulation is paused while the panel is open.

static void panel_refresh(void) {
    menu.fdc = (int)state.oric.fdc_type;
    menu.volume = audio_volume_level();
    menu.muted = audio_volume_muted();
    menu.tape_motor = state.oric.tape.motor;
    menu.tape_percent = oric_tape_percent(&state.oric.tape);
    // Wait until core 1 no longer shows the hidden surface (it takes the front one per frame)
    static uint32_t swapped_at;
    uint32_t t0 = time_us_32();
    while (panel_open && core1_frames == swapped_at && time_us_32() - t0 < 40000) tight_loop_contents();
    const uint8_t back = (uint8_t)(osd_front ^ 1);
    oric_menu_draw(&menu, &osd_surfaces[back]);
    __dmb();
    osd_front = back;
    swapped_at = core1_frames;
}

static void panel_toggle(void) {
    if (panel_open) {
        panel_open = false;
        return;
    }
    menu.message[0] = 0;
    menu.page = ORIC_PAGE_MAIN;
    menu.cursor = ORIC_ITEM_RESUME;
    panel_refresh();
    panel_open = true;
}

static void panel_action(oric_menu_action_t a) {
    char msg[96];
    switch (a.type) {
        case ORIC_ACT_SET_FDC:
            if (panel_set_fdc(a.file)) {
                snprintf(msg, sizeof(msg), "Interface %s : l'Oric redémarre", oric_menu_fdc_names[a.file]);
                oric_menu_message(&menu, false, msg);
            } else {
                oric_menu_message(&menu, true, "Interface indisponible (ROM absente)");
            }
            break;
        case ORIC_ACT_INSERT:
            if (dsk_insert(a.target, menu.files[a.file].name)) {
                snprintf(msg, sizeof(msg), "%.48s dans le lecteur %c%s", menu.files[a.file].name, 'A' + a.target,
                         a.target == 0 ? " : Redémarrer pour démarrer dessus" : "");
                oric_menu_message(&menu, false, msg);
            } else {
                snprintf(msg, sizeof(msg), "%.48s : image MFM_DISK illisible", menu.files[a.file].name);
                oric_menu_message(&menu, true, msg);
            }
            break;
        case ORIC_ACT_EJECT:
            dsk_close(a.target);
            snprintf(msg, sizeof(msg), "Lecteur %c vide", 'A' + a.target);
            oric_menu_message(&menu, false, msg);
            break;
        case ORIC_ACT_TAPE_INSERT:
            if (tap_insert(menu.files[a.file].name)) {
                snprintf(msg, sizeof(msg), "%.48s en place : taper CLOAD\"\"", menu.files[a.file].name);
                oric_menu_message(&menu, false, msg);
            } else {
                oric_menu_message(&menu, true, "Cassette illisible");
            }
            break;
        case ORIC_ACT_TAPE_EJECT:
            tap_close();
            oric_menu_message(&menu, false, "Cassette éjectée");
            break;
        case ORIC_ACT_RESET:
            oric_reset(&state.oric);
            panel_open = false;
            return;
        case ORIC_ACT_SAVE:
            if (menu.usb_present) config_save();
            else oric_menu_message(&menu, true, "Pas de clé USB");
            break;
        case ORIC_ACT_RESUME:
            panel_open = false;
            return;
        case ORIC_ACT_PROFILE:
            profile_apply(a.file);
            break;
        case ORIC_ACT_VOLUME:
            audio_set_volume((uint8_t)menu.volume, menu.muted);
            break;
        default:
            break;
    }
    panel_refresh();
}

// Keys while the panel is open (codes of hid_app.c: ASCII, or HID usage | 0x100)
static bool panel_key(int code) {
    int key;
    switch (code) {
        case 0x152: key = OSD_KEY_UP; break;
        case 0x151: key = OSD_KEY_DOWN; break;
        case 0x150: key = OSD_KEY_LEFT; break;
        case 0x14F: key = OSD_KEY_RIGHT; break;
        case 0x0D: key = OSD_KEY_ENTER; break;
        case 0x1B: key = OSD_KEY_ESC; break;
        case 0x7F:
        case 0x08: key = OSD_KEY_DEL; break;
        case 0x14B: key = OSD_KEY_PGUP; break;
        case 0x14E: key = OSD_KEY_PGDN; break;
        case 0x14A: key = OSD_KEY_HOME; break;
        case 0x14D: key = OSD_KEY_END; break;
        default:
            if (code <= ' ' || code >= 0x7F) return false;
            key = code;
            break;
    }
    panel_action(oric_menu_key(&menu, key));
    return true;
}

// Core 1: each line converted to three 1-bit planes (red, green, blue) and
// encoded with tmds_encode_1bpp, as the Telestrat and the BBC do (the 8 Oric
// colours are 0x00 or 0xFF per channel). PicoDVI shows every TMDS buffer twice
// (DVI_VERTICAL_REPEAT = 2): FRAME_HEIGHT / 2 buffers per frame.
#define DISPLAY_LINES (FRAME_HEIGHT / 2)
#define ORIC_TOP      ((DISPLAY_LINES - ORIC_SCREEN_HEIGHT) / 2)
#define ORIC_LEFT     ((FRAME_WIDTH - ORIC_PLANES_PIXELS) / 2)

static uint32_t __not_in_flash() planes[3][FRAME_WIDTH / 32];

static inline void __not_in_flash_func(encode_planes)(uint32_t *tmdsbuf) {
    // TMDS lanes: 0 blue, 1 green, 2 red
    tmds_encode_1bpp(planes[2], tmdsbuf, FRAME_WIDTH);
    tmds_encode_1bpp(planes[1], tmdsbuf + FRAME_WIDTH / DVI_SYMBOLS_PER_WORD, FRAME_WIDTH);
    tmds_encode_1bpp(planes[0], tmdsbuf + 2 * FRAME_WIDTH / DVI_SYMBOLS_PER_WORD, FRAME_WIDTH);
}

#ifdef ORIC_DIAG
volatile uint32_t diag_frame_us;       // Core 1: last frame (us), Oric picture or control panel
volatile uint32_t diag_line_us_max;    // Core 1: slowest line of the last frame (us)
#endif

// Words of the planes outside the Oric picture (the panel may have drawn there):
// oric_planes_line rewrites every word it covers. No memset: it lives in flash, and
// core 1 must not wait for the flash while core 0 uses it (tools/core1_flash.py);
// volatile stores so that GCC does not turn the loops back into a memset call.
#define ORIC_PLANE_WORDS  (FRAME_WIDTH / 32)
#define ORIC_FIRST_WORD   (ORIC_LEFT / 32)
#define ORIC_END_WORD     ((ORIC_LEFT + ORIC_PLANES_PIXELS + 31) / 32)
static inline void __not_in_flash_func(clear_plane_margins)(void) {
    for (int p = 0; p < 3; p++) {
        volatile uint32_t *w = planes[p];
        for (int i = 0; i < ORIC_FIRST_WORD; i++) w[i] = 0;
        for (int i = ORIC_END_WORD; i < ORIC_PLANE_WORDS; i++) w[i] = 0;
    }
}

static inline void __not_in_flash_func(render_frame)() {
#ifdef ORIC_DIAG
    uint32_t t0 = time_us_32(), worst = 0;
#endif
    const bool panel = panel_open;
    const osd_surface_t *osd = &osd_surfaces[osd_front];
    const osd_row_t *banner = banner_on ? &banner_rows[banner_front] : NULL;
    core1_frames++;
    for (int y = 0; y < DISPLAY_LINES; y++) {
        uint32_t *tmdsbuf;
        queue_remove_blocking_u32(&dvi0.q_tmds_free, &tmdsbuf);
#ifdef ORIC_DIAG
        uint32_t tl = time_us_32();
#endif
        if (panel) {
            osd_render_line(osd, y, planes[0], planes[1], planes[2]);
            encode_planes(tmdsbuf);
        } else if (banner && y >= ORIC_TOP + ORIC_SCREEN_HEIGHT && y < ORIC_TOP + ORIC_SCREEN_HEIGHT + 8) {
            const int by = y - ORIC_TOP - ORIC_SCREEN_HEIGHT;
            osd_render_cells(banner->ch, banner->attr, banner->big, by, by, planes[0], planes[1], planes[2]);
            encode_planes(tmdsbuf);
        } else if (y < ORIC_TOP || y >= ORIC_TOP + ORIC_SCREEN_HEIGHT) {
            copy_tmdsbuf(tmdsbuf, empty_tmdsbuf);
        } else {
            clear_plane_margins();
            oric_planes_line(&state.oric.fb[(y - ORIC_TOP) * ORIC_PLANES_BYTES_PER_LINE], planes[0], planes[1], planes[2],
                             ORIC_LEFT);
            encode_planes(tmdsbuf);
        }
#ifdef ORIC_DIAG
        uint32_t dt = time_us_32() - tl;
        if (dt > worst) worst = dt;
#endif
        queue_add_blocking_u32(&dvi0.q_tmds_valid, &tmdsbuf);
    }
#ifdef ORIC_DIAG
    diag_frame_us = time_us_32() - t0;
    diag_line_us_max = worst;
#endif
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

// The emulation loop, with oric_tick inlined, runs from RAM too (unless ORIC_COLD)
#ifdef ORIC_COLD
static void __attribute__((noinline)) run_ticks(uint32_t n) {
#else
static void __no_inline_not_in_flash_func(run_ticks)(uint32_t n) {
#endif
    oric_t *sys = &state.oric;
    for (uint32_t i = 0; i < n; i++) {
        _oric_tick(sys);   // Body inlined: no function call per emulated cycle
    }
}

#ifdef ORIC_DIAG
// Bus cycles alone: the 65C02 runs whatever the RAM holds (reset afterwards)
#ifdef ORIC_COLD
static void __attribute__((noinline)) bench_bus(uint32_t n) {
#else
static void __no_inline_not_in_flash_func(bench_bus)(uint32_t n) {
#endif
    oric_t *sys = &state.oric;
    for (uint32_t i = 0; i < n; i++) {
        MOS6502CPU_TICK(&sys->cpu);
        const uint16_t a = sys->cpu.addr;
        if (sys->cpu.rw) {
            MOS6502CPU_SET_DATA(&sys->cpu, sys->ram[a & 0xBFFF]);
        } else {
            sys->ram[a & 0xBFFF] = MOS6502CPU_GET_DATA(&sys->cpu);
        }
    }
}

static void diag_bench_run(void) {
    uint32_t t = time_us_32();
    bench_bus(40000);
    diag_bench[0] = time_us_32() - t;
    t = time_us_32();
    run_ticks(40000);
    diag_bench[1] = time_us_32() - t;
}
#endif

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

    oric_planes_init();                     // Before core 1 converts the first lines
    memset(planes, 0, sizeof(planes));
    encode_planes(empty_tmdsbuf);           // Black line (borders)

    printf("Core 1 start\n");
    hw_set_bits(&bus_ctrl_hw->priority, BUSCTRL_BUS_PRIORITY_PROC1_BITS);
    multicore_launch_core1(core1_main);

    app_init();
#ifdef ORIC_BUS_PIO
    bus_pio_start();   // GPIO 0-10 and PHI2 to the state machine (after wdc65C02cpu_init)
#endif
#ifdef ORIC_DIAG
    diag_bench_run();
    oric_reset(&state.oric);
#endif

    uint32_t deadline = time_us_32();
    while (1) {
        uint32_t start_time_in_micros = time_us_32();

        uint32_t num_ticks = panel_open ? 0 : 19968;   // Paused while the control panel is open
        run_ticks(num_ticks);
#ifdef ORIC_DIAG
        if (num_ticks) {
            uint32_t tick_us = time_us_32() - start_time_in_micros;
            diag_tick_us += tick_us;
            diag_tick_frames++;
            if (tick_us > diag_tick_max) diag_tick_max = tick_us;
        }
#endif
        usb_poll();
        net_poll();
        banner_update();
#ifdef ORIC_DIAG
        diag_file_service();
#endif
        while (panel_keys_head != panel_keys_tail) {
            int code = panel_keys[panel_keys_head & 15];
            panel_keys_head++;
            if (panel_open) panel_key(code);
        }
        if (panel_open) {
            static uint32_t refresh;
            if ((++refresh & 15) == 0) panel_refresh();   // Tape position, motor
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
