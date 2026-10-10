// bbc.c
//
// BBC Micro Model B on the Olimex Neo6502: the real 65C02 executes the MOS,
// the RP2040 emulates memory, 6845/ULA video, VIAs, keyboard and SN76489.
//
// Core 0: CPU bus (PIO state machine, chips/wdc65C02bus.h; -DBBC_BUS_SIO: bit-banged),
// 2 MHz, USB host (keyboard, gamepad = joystick 1).
// Discs: .ssd/.dsd files in the root of a USB drive (FAT, sectors read on
// demand through FatFs, writes go back to the file) or images compiled in
// flash (src/images/bbc_images.h, read-only);
// F11 = control panel (drives 0 and 1: USB files, then flash images; BREAK,
// SHIFT+BREAK, volume, snapshots BBCnnnn.STA; src/osd/bbc_menu.h); Pause = Neo6502 firmware.
// Core 1: DVI 800x480 (or 960x544) @ 60 Hz, BBC 640x256 centred, lines
//         doubled by PicoDVI: 800x480 at 345 MHz (default,
//         stretched blanking) or 295.2 MHz (-DBBC_VIDEO_480) shows the last
//         line of each 8-line row once, so that the 256 lines fit;
//         960x544 at 372 MHz (-DBBC_VIDEO_544) doubles all of them.
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
// Byte -> pixels tables built 16 entries per line (~5 us; a whole table
// costs ~90 us in the 1 bpp modes, the core 1 budget is 67 us per line)
#define BBC_LUT_PROGRESSIVE 16
// 10 cached tables: title screens often use several palettes per frame (Battlezone:
// 9 in a cycle; with 8 tables, least recently used out, every palette missed its table
// and most lines were drawn pixel by pixel, up to ~100 us: red lines in bursts)
#define BBC_LUT_SLOTS 10
#ifdef BBC_DIAG
volatile uint32_t diag_lut_builds;
#define BBC_LUT_BUILD_HOOK() (diag_lut_builds++)
#endif
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
// Real 65C02: PIO bus by default (-DBBC_BUS_SIO for the bit-banged bus)
#ifndef BBC_BUS_SIO
#define BBC_BUS_PIO 1
#define WDC65C02_BUS_PIO 1
#endif
#include "chips/wdc65C02bus.h"
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
#include "systems/bbc_state.h"

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
#define NEO_STORAGE_IMPL
#include "devices/neo_storage.h"
extern const neo_storage_ops_t neo_storage_fatfs_ops;   // neo_storage_fatfs.c (socle)

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
static uint32_t usb_sizes[USB_MAX_FILES];
static int usb_num_files = 0;
static bool usb_scanned = false;
static neo_file_t usb_file[2];   // Disc image of each drive on the USB drive (neo_storage, volume NEO_VOL_USB)
// Image of each drive: index in the list of the menu (USB files, then flash images), -1: empty.
// The USB files are listed once, when the drive is mounted, before any USB image is inserted.
static int drive_image[2] = {-1, -1};

extern bool msc_inquiry_complete;
extern void msc_poll(void);

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

// Sector reader / writer for the WD1770: 256 bytes at `offset` of the USB file of a drive (ctx)
static bool usb_read_sector(void* ctx, uint32_t offset, uint8_t* buf) {
    return neo_file_read((neo_file_t*)ctx, offset, buf, 256);
}

static bool usb_write_sector(void* ctx, uint32_t offset, const uint8_t* buf) {
    return neo_file_write((neo_file_t*)ctx, offset, buf, 256);   // Durable on return (the driver syncs)
}

static bool usb_scan_entry(const neo_dirent_t* e, void* user) {
    (void)user;
    if (usb_num_files >= USB_MAX_FILES) return false;
    if (e->dir || strlen(e->name) > 12) return true;
    if (has_ext(e->name, ".ssd") || has_ext(e->name, ".dsd")) {
        strcpy(usb_files[usb_num_files], e->name);
        usb_sizes[usb_num_files] = e->size;
        usb_num_files++;
    }
    return true;
}

static void usb_scan(void) {
    usb_num_files = 0;
    if (!neo_storage_list(NEO_VOL_USB, "/", usb_scan_entry, 0)) return;
    printf("USB: %d disc image(s)\n", usb_num_files);
}

static int num_images(void) { return usb_num_files + BBC_NUM_IMAGES; }

static const char* image_name(int index) {
    if (index < usb_num_files) return usb_files[index];
#ifdef BBC_HAVE_IMAGES
    const char* n = bbc_disc_images[index - usb_num_files].name;
    return n ? n : "(flash)";
#else
    return "";
#endif
}

static void eject_drive(int drive) {
    neo_file_close(&usb_file[drive]);
    wd1770_eject(&state.bbc.fdc, drive);
    drive_image[drive] = -1;
}

// Insert image `index` in `drive`: USB file (read and write) or flash image (read-only)
static bool insert_image(int drive, int index) {
    if (index < 0 || index >= num_images()) return false;
    eject_drive(drive);
    if (index < usb_num_files) {
        const char* name = usb_files[index];
        if (!neo_file_open_path(&usb_file[drive], name, NEO_READ | NEO_WRITE)) {
            printf("USB: cannot open %s\n", name);
            return false;
        }
        wd1770_insert_streamed(&state.bbc.fdc, drive, usb_file[drive].size, has_ext(name, ".dsd") ? 2 : 1,
                               usb_read_sector, usb_write_sector, &usb_file[drive]);
        printf("Drive %d: %s (%u bytes)\n", drive, name, (unsigned)usb_file[drive].size);
    } else {
#ifdef BBC_HAVE_IMAGES
        const bbc_disc_image_t* im = &bbc_disc_images[index - usb_num_files];
        bbc_insert_disc(&state.bbc, drive, (uint8_t*)im->data, im->size, im->sides, true);
        printf("Drive %d: flash disc %d (%u bytes)\n", drive, index - usb_num_files, (unsigned)im->size);
#endif
    }
    drive_image[drive] = index;
    return true;
}

static uint32_t boot_shift_release;   // emu_frames value at which SHIFT goes up (SHIFT+BREAK)
static uint32_t emu_frames;

// SHIFT+BREAK: SHIFT held while the MOS restarts (auto-boot from !BOOT)
static void boot_disc(void) {
    bbc_key_down(&state.bbc, BBC_KEY_Shift);
    bbc_reset(&state.bbc);
    boot_shift_release = emu_frames + 50;
}

/*-- Control panel (F11) -------------------------------------------------------*/
// Full-screen panel in the style of the Telestrat and Oric ones (src/osd/bbc_menu.h):
// drives 0 and 1, USB drive, flash images, boot on insertion, volume, BREAK /
// SHIFT+BREAK / save / resume. The emulation is paused while it is open. Core 1
// draws the surface (osd_render_line) in place of the BBC picture; core 0 redraws it
// just after core 1 has finished a frame, during the vertical blanking (one surface
// of 9 KB: two do not fit in the RAM; at worst the first lines of the banner, drawn
// ahead by core 1, are those of the previous state).
#ifdef BBC_VIDEO_544
#define OSD_COLS 120   // 960 x 544
#define OSD_ROWS 34
#else
#define OSD_COLS 100   // 800 x 480
#define OSD_ROWS 30
#endif
// Plain panel backgrounds: the dithered ones (one pixel out of two) gave red
// streaks on the Neo6502 HDMI link (Oric, bmarty's screen, 2026-09-30)
#define OSD_NO_DITHER 1
#define OSD_HOT          __attribute__((section(".time_critical.osd")))
// Font in RAM: in flash, core 1 waited for it while core 0 redrew the panel (red lines)
#define OSD_FONT_SECTION __attribute__((section(".time_critical.osd_font")))
#define BBC_MENU_FILES 40   // USB_MAX_FILES + the flash images (RAM: the tables above)
#define BBC_MENU_STATES 16  // Snapshots listed by the panel
#include "osd/bbc_menu.h"
#include "osd/bbc_config.h"

static bbc_menu_t menu;
static osd_surface_t __not_in_flash() osd_surface;
static volatile uint32_t core1_frames;   // Frames drawn by core 1
static volatile bool menu_open;

static void state_scan(bool names);

// State of the machine -> menu
static void menu_fill(void) {
    menu.nfiles = 0;
    for (int i = 0; i < num_images() && menu.nfiles < BBC_MENU_FILES; i++) {
        bbc_menu_file_t* f = &menu.files[menu.nfiles++];
        f->name = image_name(i);
        if (i < usb_num_files) {
            f->size = usb_sizes[i];
            f->src = BBC_SRC_USB;
        } else {
#ifdef BBC_HAVE_IMAGES
            f->size = bbc_disc_images[i - usb_num_files].size;
#endif
            f->src = BBC_SRC_FLASH;
        }
    }
    for (int d = 0; d < BBC_MENU_DRIVES; d++) {
        const int i = drive_image[d];
        snprintf(menu.drive[d], sizeof(menu.drive[d]), "%.39s", i >= 0 ? image_name(i) : "");
        menu.drive_src[d] = i >= usb_num_files ? BBC_SRC_FLASH : BBC_SRC_USB;
        menu.drive_ro[d] = i >= usb_num_files;
    }
    menu.usb_present = usb_scanned;
    menu.volume = audio_volume_level();
    menu.muted = audio_volume_muted();
}

static void menu_refresh(void) {
    if (menu_open) {
        // Wait for the end of a frame of core 1 (at most 50 ms)
        const uint32_t f = core1_frames, t0 = time_us_32();
        while (core1_frames == f && time_us_32() - t0 < 50000) tight_loop_contents();
    }
    bbc_menu_draw(&menu, &osd_surface);
}

static void menu_close(void) { menu_open = false; }

static void menu_toggle(void) {
    if (menu_open) {
        menu_close();
        return;
    }
    menu.page = BBC_PAGE_MAIN;
    menu.message[0] = 0;
    state_scan(false);   // Count of the main page (the names: when the selector opens)
    menu_fill();
    menu_refresh();
    menu_open = true;
}

/*-- BBC.CFG (src/osd/bbc_config.h) --------------------------------------------*/
// No buffer of its own (RAM): the file is read into the panel surface, unused while the
// panel is closed, and rewritten through the captured BBC lines, unused while the panel
// is open (emulation paused, core 1 draws the panel).
#define CONFIG_TEXT_MAX 1536
_Static_assert(sizeof(osd_surface) > CONFIG_TEXT_MAX, "BBC.CFG: read buffer");
_Static_assert(sizeof(state.bbc.lines) > 2 * CONFIG_TEXT_MAX, "BBC.CFG: rewrite buffers");

static int ascii_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static bool same_name(const char* a, const char* b) {
    while (*a && ascii_lower(*a) == ascii_lower(*b)) a++, b++;
    return !*a && !*b;
}

// Image of a name of BBC.CFG (USB file, or "@Name" for a flash image), -1 if unknown
static int config_find_image(const char* name) {
    if (name[0] == BBC_CONFIG_FLASH) {
        for (int i = usb_num_files; i < num_images(); i++) {
            if (same_name(image_name(i), name + 1)) return i;
        }
        return -1;
    }
    for (int i = 0; i < usb_num_files; i++) {
        if (same_name(usb_files[i], name)) return i;
    }
    return -1;
}

// Applies BBC.CFG of the USB drive (panel closed); false if there is none
static bool config_apply(void) {
    char* text = (char*)&osd_surface;
    uint32_t n = 0;
    if (!neo_file_load(NEO_VOL_USB, BBC_CONFIG_FILE, (uint8_t*)text, CONFIG_TEXT_MAX - 1, &n, false)) return false;
    text[n] = 0;
    bbc_config_t c;
    bbc_config_parse(text, &c, HID_VOLUME_MAX);
    if (c.volume >= 0) audio_set_volume((uint8_t)c.volume, false);
    if (c.autoboot >= 0) menu.autoboot = c.autoboot;
    bool boot = false;
    for (int d = 0; d < 2; d++) {
        if (!c.drive_set[d]) continue;
        if (!c.drive[d][0]) {
            eject_drive(d);
            continue;
        }
        const int i = config_find_image(c.drive[d]);
        if (i >= 0 && insert_image(d, i)) {
            boot |= d == 0;
        } else {
            printf("BBC.CFG: no image %s\n", c.drive[d]);
        }
    }
    if (boot && menu.autoboot) boot_disc();
    printf("BBC.CFG applied\n");
    return true;
}

static void config_save(void) {
    char* old = (char*)state.bbc.lines;
    char* out = old + CONFIG_TEXT_MAX;
    uint32_t n = 0;
    if (!neo_file_load(NEO_VOL_USB, BBC_CONFIG_FILE, (uint8_t*)old, CONFIG_TEXT_MAX - 1, &n, false)) n = 0;
    old[n] = 0;
    char names[2][BBC_CONFIG_NAME_LEN + 1];
    for (int d = 0; d < 2; d++) {
        const int i = drive_image[d];
        snprintf(names[d], sizeof(names[d]), "%s%.39s", i >= usb_num_files ? "@" : "", i >= 0 ? image_name(i) : "");
    }
    const char* drives[2] = {names[0], names[1]};
    const size_t len = bbc_config_merge(old, drives, menu.autoboot, audio_volume_level(), out, CONFIG_TEXT_MAX);
    const bool ok = usb_scanned && neo_file_save(NEO_VOL_USB, BBC_CONFIG_FILE, (const uint8_t*)out, (uint32_t)len);
    memset(state.bbc.lines, 0, sizeof(state.bbc.lines));   // Blank until the emulation captures them again
    bbc_menu_message(&menu, !ok, ok ? "Configuration enregistrée dans BBC.CFG" : "BBC.CFG : écriture impossible (clé USB ?)");
}

/*-- Snapshots BBCnnnn.STA (systems/bbc_state.h) ------------------------------*/
// Taken and resumed from the panel (emulation paused). The names of the selector live in
// the captured BBC lines, unused while the panel is open, after the BBC.CFG buffers.
#define STATE_NAMES_OFFSET 4096
_Static_assert(sizeof(state.bbc.lines) >= STATE_NAMES_OFFSET + BBC_MENU_STATES * 13, "snapshot names");
static int state_count, state_last;   // Snapshots on the drive, highest number
static bool state_keep_names;

static char (*state_names(void))[13] { return (char (*)[13])((uint8_t*)state.bbc.lines + STATE_NAMES_OFFSET); }

static bool state_scan_entry(const neo_dirent_t* e, void* user) {
    (void)user;
    const char* n = e->name;
    if (e->dir || strlen(n) != 11 || ascii_lower(n[0]) != 'b' || ascii_lower(n[1]) != 'b' || ascii_lower(n[2]) != 'c' ||
        !has_ext(n, ".sta")) {
        return true;
    }
    int num = 0;
    for (int i = 3; i < 7; i++) {
        if (n[i] < '0' || n[i] > '9') return true;
        num = num * 10 + (n[i] - '0');
    }
    if (num > state_last) state_last = num;
    if (state_count < BBC_MENU_STATES) {
        if (state_keep_names) {
            strcpy(state_names()[state_count], n);
            menu.states[state_count] = state_names()[state_count];
        }
        state_count++;
    }
    return true;
}

// Snapshots of the USB drive: count (and names, only while the panel is open)
static void state_scan(bool names) {
    state_count = state_last = 0;
    state_keep_names = names;
    if (usb_scanned) neo_storage_list(NEO_VOL_USB, "/", state_scan_entry, 0);
    menu.nstates = state_count;
}

typedef struct {
    neo_file_t f;
    uint32_t offset;
} state_io_t;
static state_io_t state_io;   // File of the snapshot being written or read

static bool state_write(void* ctx, const void* data, uint32_t len) {
    state_io_t* io = (state_io_t*)ctx;
    const bool ok = neo_file_write(&io->f, io->offset, (const uint8_t*)data, len);
    io->offset += len;
    return ok;
}

static bool state_read(void* ctx, void* data, uint32_t len) {
    state_io_t* io = (state_io_t*)ctx;
    const bool ok = neo_file_read(&io->f, io->offset, (uint8_t*)data, len);
    io->offset += len;
    return ok;
}

static void state_save(void) {
    char name[16], msg[96];
    state_scan(false);
    snprintf(name, sizeof(name), "BBC%04d.STA", (state_last + 1) % 10000);
    state_io_t* const iop = &state_io;
    memset(iop, 0, sizeof(*iop));
    if (!neo_file_open_path(&iop->f, name, NEO_WRITE | NEO_CREATE)) {
        bbc_menu_message(&menu, true, "Instantané : écriture impossible (clé USB ?)");
        return;
    }
    const bool ok = bbc_state_save(&state.bbc, state_write, iop);
    neo_file_close(&iop->f);
    if (!ok) {
        neo_file_remove(NEO_VOL_USB, name);   // No half-written snapshot
        bbc_menu_message(&menu, true, state.bbc.fdc.state != WD1770_IDLE ? "Instantané refusé : accès disque en cours"
                                                                         : "Instantané : échec de l'écriture");
    } else {
        snprintf(msg, sizeof(msg), "Instantané %s enregistré (%u Ko)", name, (unsigned)(iop->offset + 1023) / 1024);
        bbc_menu_message(&menu, false, msg);
    }
    memset(state.bbc.lines, 0, sizeof(state.bbc.lines));   // Lines captured during the capture
    state_scan(false);
}

// Resumes snapshot `index` of the selector; true: the panel can close
static bool state_load(int index) {
    char name[16];
    snprintf(name, sizeof(name), "%s", menu.states[index]);
    state_io_t* const iop = &state_io;
    memset(iop, 0, sizeof(*iop));
    if (!neo_file_open_path(&iop->f, name, NEO_READ)) {
        bbc_menu_message(&menu, true, "Instantané : lecture impossible");
        return false;
    }
    const bool header = bbc_state_check(state_read, iop);
    iop->offset = 0;
    const bool ok = header && bbc_state_load(&state.bbc, state_read, iop);
    neo_file_close(&iop->f);
    memset(state.bbc.lines, 0, sizeof(state.bbc.lines));
    if (!header) {
        bbc_menu_message(&menu, true, "Pas un instantané de ce BBC (version ou modèle)");
        return false;
    }
    if (!ok) {
        // Part of the machine already replaced: start again
        bbc_reset(&state.bbc);
        bbc_menu_message(&menu, true, "Instantané illisible : machine redémarrée");
        return false;
    }
    printf("Snapshot %s resumed\n", name);
    return true;
}

void hid_raw_key_down(uint8_t keycode);
void hid_raw_key_up(uint8_t keycode);

static void menu_action(bbc_menu_action_t a) {
    char msg[96];
    switch (a.type) {
        case BBC_ACT_INSERT:
            if (!insert_image(a.target, a.file)) {
                bbc_menu_message(&menu, true, "Image illisible");
                break;
            }
            if (a.target == 0 && menu.autoboot) {
                menu_close();
                boot_disc();
                return;
            }
            snprintf(msg, sizeof(msg), "%.40s dans le lecteur %d", image_name(a.file), a.target);
            bbc_menu_message(&menu, false, msg);
            break;
        case BBC_ACT_EJECT:
            eject_drive(a.target);
            snprintf(msg, sizeof(msg), "Lecteur %d vide", a.target);
            bbc_menu_message(&menu, false, msg);
            break;
        case BBC_ACT_BREAK:
            menu_close();
            hid_raw_key_down(HID_KEY_F12);   // BREAK, released after the minimum hold
            hid_raw_key_up(HID_KEY_F12);
            return;
        case BBC_ACT_BOOT:
            menu_close();
            boot_disc();
            return;
        case BBC_ACT_SAVE:
            config_save();
            break;
        case BBC_ACT_RESUME:
            menu_close();
            return;
        case BBC_ACT_VOLUME:
            audio_set_volume((uint8_t)menu.volume, menu.muted);
            break;
        case BBC_ACT_STATE_SAVE:
            state_save();
            break;
        case BBC_ACT_STATE_LOAD:
            if (state_load(a.file)) {
                menu_close();
                return;
            }
            break;
        default:
            break;
    }
    menu_fill();
    menu_refresh();
}

// HID usage of the keyboard -> code of osd_key_from_hid (ASCII, or usage | 0x100)
static int menu_key_code(uint8_t k) {
    if (k >= HID_KEY_A && k <= HID_KEY_Z) return 'A' + (k - HID_KEY_A);
    if (k >= HID_KEY_1 && k <= HID_KEY_9) return '1' + (k - HID_KEY_1);
    switch (k) {
        case HID_KEY_0: return '0';
        case HID_KEY_ENTER:
        case HID_KEY_KEYPAD_ENTER: return 0x0D;
        case HID_KEY_ESCAPE: return 0x1B;
        case HID_KEY_DELETE:
        case HID_KEY_BACKSPACE: return 0x7F;
        case HID_KEY_ARROW_UP:
        case HID_KEY_ARROW_DOWN:
        case HID_KEY_ARROW_LEFT:
        case HID_KEY_ARROW_RIGHT:
        case HID_KEY_PAGE_UP:
        case HID_KEY_PAGE_DOWN:
        case HID_KEY_HOME:
        case HID_KEY_END: return 0x100 | k;
        default: return -1;
    }
}

// Keys while the menu is open (HID usage codes)
static void menu_key(uint8_t keycode) {
    if (keycode == HID_KEY_F11) {
        menu_close();
        return;
    }
    const int key = osd_key_from_hid(menu_key_code(keycode));
    if (key < 0) return;
    // Snapshot selector about to open: its names (in the captured lines, unused now)
    if (key == OSD_KEY_ENTER && menu.page == BBC_PAGE_MAIN && menu.cursor == BBC_ITEM_STATE) state_scan(true);
    menu_action(bbc_menu_key(&menu, key));
}

// Called every frame: once the USB drive is mounted, list its images, then apply BBC.CFG
// or else insert the first one (panel closed: BBC.CFG is read into its surface)
static void usb_poll(void) {
    if (usb_scanned || !msc_inquiry_complete || menu_open) return;
    usb_scanned = true;
    // Flash images in the drives: same images, new indices after the USB files
    int flash[2];
    for (int d = 0; d < 2; d++) flash[d] = drive_image[d] >= 0 ? drive_image[d] - usb_num_files : -1;
    usb_scan();
    for (int d = 0; d < 2; d++) {
        if (flash[d] >= 0) drive_image[d] = usb_num_files + flash[d];
    }
    if (!config_apply() && usb_num_files > 0) {
        insert_image(0, 0);
    }
}

extern volatile uint32_t diag_layout[4];

void app_init(void) {
    (void)diag_layout[0];   // Kept by the linker for the host tools
    bbc_desc_t desc = bbc_desc();
    neo_storage_set(NEO_VOL_USB, "Clé USB", &neo_storage_fatfs_ops, "");   // Ready once a drive is mounted
    bbc_init(&state.bbc, &desc);
    bbc_set_sample_rate_q8(&state.bbc, audio_pwm_rate_q8(22050));   // The rate the PWM really consumes
    bbc_reset(&state.bbc);
#ifdef BBC_HAVE_IMAGES
    insert_image(0, usb_num_files);   // First flash image until a USB drive shows up
#endif
}

#if defined(BBC_VIDEO_480)
// TMDS bit clock 295.2 MHz, DVDD 1.2V (Neo6502 timing shared with the Oric build)
#define FRAME_WIDTH  800
#define FRAME_HEIGHT 480
#define VREG_VSEL    VREG_VOLTAGE_1_20
#define DVI_TIMING   dvi_timing_800x480p_60hz
#define BBC_DISPLAY_TOP 0
#elif defined(BBC_VIDEO_544)
// TMDS bit clock (and system clock) 372 MHz, DVDD 1.3V (reload-emulator's
// setting for the other RP2040 boards): the 256 BBC lines fit (8 blank lines
// above and below), but not every screen accepts 960x544
#define FRAME_WIDTH  960
#define FRAME_HEIGHT 544
#define VREG_VSEL    VREG_VOLTAGE_1_30
#define DVI_TIMING   dvi_timing_960x544p_60hz
#define BBC_DISPLAY_TOP (-8)
#elif defined(BBC_VIDEO_480_372)
// 800x480 (same line and frame rates as the Neo6502 builds) clocked at 372 MHz
// by stretching the horizontal blanking: refused by bmarty's screen (or link)
#include "neo_dvi_timings.h"
#define FRAME_WIDTH  800
#define FRAME_HEIGHT 480
#define VREG_VSEL    VREG_VOLTAGE_1_30
#define DVI_TIMING   dvi_timing_800x480p_60hz_372
#define BBC_DISPLAY_TOP 0
#else
// Default: the 800x480 mode of the Neo6502 builds (same line and frame rates,
// same 800 active pixels and 480 lines) at 345 MHz instead of 295.2, the
// horizontal blanking stretched to 1160 pixels per line: 172 cycles per 2 MHz
// cycle (~165 needed), TMDS bit rate 17 % higher (bmarty's screen accepts 330
// and 345 MHz, not 372). DVDD 1.25V.
#include "neo_dvi_timings.h"
#define FRAME_WIDTH  800
#define FRAME_HEIGHT 480
#define VREG_VSEL    VREG_VOLTAGE_1_25
#define DVI_TIMING   dvi_timing_800x480p_60hz_345
#define BBC_DISPLAY_TOP 0
#endif
// BBC lines shown: BBC_DISPLAY_TOP .. BBC_DISPLAY_TOP + BBC_DISPLAY_LINES - 1, one TMDS buffer
// each, shown by PicoDVI on two output lines, except at 800x480 where 256 lines need 512
// output lines: the last line of every 8-line character row is shown once (dvi/dvi_line_repeat.h)
#if FRAME_HEIGHT >= 2 * BBC_SCREEN_HEIGHT
#define BBC_DISPLAY_LINES (FRAME_HEIGHT / 2)
#else
#define BBC_DISPLAY_LINES BBC_SCREEN_HEIGHT
_Static_assert(2 * BBC_SCREEN_HEIGHT - BBC_SCREEN_HEIGHT / 8 == FRAME_HEIGHT, "800x480: 256 BBC lines in 480 output lines");
#endif
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
static uint32_t key_down_frame[128];
static bool key_release_pending[128];

// Layout for host tools (SWD screen capture, co-simulation): RAM and captured
// lines of bbc_t, in every build
volatile uint32_t diag_layout[4] = {offsetof(bbc_t, ram), offsetof(bbc_t, lines), sizeof(bbc_line_t), offsetof(bbc_t, crtc_reg)};

#ifdef BBC_DIAG
// Remote typing for bench tests over SWD: the host writes BBC key codes into
// diag_keyq (bit 7 = with SHIFT, 0x7F = BREAK, 0x7E = next disc, 0x7D = F11 menu) and advances diag_keyq_tail; each key is held
// then released for DIAG_KEY_FRAMES emulated frames
#define DIAG_KEY_FRAMES 4
volatile uint8_t diag_keyq[256];
volatile uint32_t diag_keyq_head, diag_keyq_tail;

void hid_raw_key_down(uint8_t keycode);

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
        if (key == 0xFF) {
            // SHIFT+BREAK: SHIFT stays down while the MOS restarts (auto-boot)
            phase = 3;
            timer = 50;
            return;
        }
        if (key & 0x80) bbc_key_up(&state.bbc, BBC_KEY_Shift);
        phase = 2;
        timer = DIAG_KEY_FRAMES;
    } else if (phase == 3) {
        bbc_key_up(&state.bbc, BBC_KEY_Shift);
        phase = 2;
        timer = DIAG_KEY_FRAMES;
    } else if (diag_keyq_head != diag_keyq_tail) {
        key = diag_keyq[diag_keyq_head & 255];
        diag_keyq_head++;
        if (key == 0x7E) {
            // Next disc image
            if (num_images() > 0) insert_image(0, (drive_image[0] + 1) % num_images());
            timer = DIAG_KEY_FRAMES;
            return;
        }
        if (key == 0x7D || menu_open) {
            // 0x7D = F11 (disc menu); while it is open, BBC keys drive it
            uint8_t hid = key == 0x7D ? HID_KEY_F11 : key == BBC_KEY_Up ? HID_KEY_ARROW_UP
                        : key == BBC_KEY_Down ? HID_KEY_ARROW_DOWN : key == BBC_KEY_Return ? HID_KEY_ENTER
                        : key == BBC_KEY_Space ? HID_KEY_SPACE : key == BBC_KEY_Escape ? HID_KEY_ESCAPE : 0;
            if (hid) hid_raw_key_down(hid);
            timer = DIAG_KEY_FRAMES;
            return;
        }
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
    if (menu_open) {
        menu_key(keycode);
        return;
    }
    if (keycode == HID_KEY_F11) {
        menu_toggle();   // Disc menu
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

// One TMDS buffer per BBC line, shown on two output lines (one for the last line of a
// character row at 800x480): a line must be ready every two output lines (2 x 29.7 us at
// 960x544, 2 x 33.6 us at 800x480), the queued buffers absorbing the shorter ones
// Menu line shown by the buffer of display line y: at 800x480 the BBC lines shown once
// (dvi/dvi_line_repeat.h) are matched with the menu lines (each on two output lines)
#if FRAME_HEIGHT >= 2 * BBC_SCREEN_HEIGHT
#define MENU_LINE(y) (y)
#else
#define MENU_LINE(y) bbc_menu_osd_line(y)
#endif

// The panel draws over the whole width; the BBC picture only rewrites its 640 pixels
static bool planes_margins_dirty;

static inline void __not_in_flash_func(render_frame)() {
    const bool show_menu = menu_open;
    const osd_surface_t *osd = &osd_surface;
    if (!show_menu && planes_margins_dirty) {
        for (int ch = 0; ch < 3; ch++) {
            uint8_t *p = (uint8_t *)planes[ch];
            memset(p, 0, BBC_EMPTY_COLUMNS / 8);
            memset(p + (BBC_EMPTY_COLUMNS + BBC_SCREEN_WIDTH) / 8, 0, BBC_EMPTY_COLUMNS / 8);
        }
        planes_margins_dirty = false;
    }
    for (int y = 0; y < BBC_DISPLAY_LINES; y++) {
        int src_line = BBC_DISPLAY_TOP + y;
        uint32_t *tmdsbuf;
        queue_remove_blocking_u32(&dvi0.q_tmds_free, &tmdsbuf);
#ifdef BBC_DIAG
        uint32_t t0 = time_us_32();
#endif
        uint8_t *pp[3] = {(uint8_t *)planes[0] + BBC_EMPTY_COLUMNS / 8, (uint8_t *)planes[1] + BBC_EMPTY_COLUMNS / 8,
                          (uint8_t *)planes[2] + BBC_EMPTY_COLUMNS / 8};
        if (show_menu) {
            osd_render_line(osd, MENU_LINE(y), planes[0], planes[1], planes[2]);
            planes_margins_dirty = true;
        } else if (src_line >= 0 && src_line < BBC_SCREEN_HEIGHT) {
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
    core1_frames++;
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
    // Dedicated spin locks for the DVI queues: next_striped_spin_lock_num() shares
    // 16-23 round-robin with the other SDK users (critical sections of the audio
    // queue, FatFs, TinyUSB); core 1 then waited for core 0 with interrupts
    // masked and a scanline came late (found by the Neo6502Trinity project, T-77)
    dvi_init(&dvi0, spin_lock_claim_unused(true), spin_lock_claim_unused(true));

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

    uint32_t deadline = time_us_32();
    while (1) {
        uint32_t start_time_in_micros = time_us_32();

        if (!menu_open) {   // The emulation is paused while the control panel is open
#ifdef BBC_DIAG
            uint32_t t_run = time_us_32();
            run_ticks(num_ticks);
            diag_run_us += time_us_32() - t_run;
#else
            run_ticks(num_ticks);
#endif
        }

        emu_frames++;
        tuh_task();
        msc_poll();   // Mounts a USB drive outside tuh_task
        release_pending_keys();
        if (boot_shift_release && emu_frames >= boot_shift_release) {
            bbc_key_up(&state.bbc, BBC_KEY_Shift);
            boot_shift_release = 0;
        }
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

        // Absolute deadlines: a frame that ran late is caught up by the next
        // ones (the average speed stays exact); more than 5 frames behind, the
        // schedule restarts from now instead of racing
        deadline += frame_us;
        int32_t ahead = (int32_t)(deadline - time_us_32());
        if (ahead > 0) {
            sleep_us((uint64_t)ahead);
        } else if (ahead < -5 * (int32_t)frame_us) {
            deadline = time_us_32();
        }
    }

    __builtin_unreachable();
}
