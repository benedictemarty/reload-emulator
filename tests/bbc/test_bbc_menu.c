// test_bbc_menu.c
//
// Tests of the BBC control panel (src/osd/bbc_menu.h): navigation, disc
// selector, actions, drawing, the OSD line shown by each BBC line buffer at
// 800x480, and BBC.CFG (src/osd/bbc_config.h).
//
//   bbc_menu_test [preview-dir]   with a directory: also writes PPM previews
//                                 of the main page and of the selector
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "osd/bbc_menu.h"
#include "osd/bbc_config.h"

static int tests_run, tests_failed;

#define CHECK(cond)                                                             \
    do {                                                                        \
        tests_run++;                                                            \
        if (!(cond)) {                                                          \
            tests_failed++;                                                     \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
        }                                                                       \
    } while (0)

static osd_surface_t surf;
static bbc_menu_t menu;

// Text of a surface row (Latin-1 cells), for the checks
static const char* row_text(int row) {
    static char buf[OSD_COLS + 1];
    for (int c = 0; c < OSD_COLS; c++) buf[c] = (char)surf.ch[row][c];
    buf[OSD_COLS] = 0;
    return buf;
}

static bool screen_contains(const char* text) {
    for (int r = 0; r < OSD_ROWS; r++) {
        if (strstr(row_text(r), text)) return true;
    }
    return false;
}

static void add_file(const char* name, uint32_t size, uint8_t src) {
    bbc_menu_file_t* f = &menu.files[menu.nfiles++];
    f->name = name;   // String literals: kept
    f->size = size;
    f->src = src;
}

static void setup(void) {
    bbc_menu_init(&menu);
    menu.version = "v1.0";
    menu.usb_present = true;
    add_file("ELITE.SSD", 102400, BBC_SRC_USB);
    add_file("REPTON.SSD", 51200, BBC_SRC_USB);
    add_file("WELCOME.DSD", 409600, BBC_SRC_USB);
    add_file("Spellbinder", 102400, BBC_SRC_FLASH);
    add_file("Knight Lore", 102400, BBC_SRC_FLASH);
}

static void write_ppm(const char* dir, const char* name) {
    if (!dir) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    static const uint8_t rgb[8][3] = {{0, 0, 0}, {255, 0, 0}, {0, 255, 0}, {255, 255, 0},
                                      {0, 0, 255}, {255, 0, 255}, {0, 255, 255}, {255, 255, 255}};
    fprintf(f, "P6\n%d %d\n255\n", OSD_WIDTH, OSD_LINES * 2);
    static uint32_t r[OSD_COLS / 4], g[OSD_COLS / 4], b[OSD_COLS / 4];
    for (int y = 0; y < OSD_LINES; y++) {
        osd_render_line(&surf, y, r, g, b);
        for (int rep = 0; rep < 2; rep++) {   // Each buffer line shown twice, as on the Neo6502
            for (int x = 0; x < OSD_WIDTH; x++) {
                int i = (int)(((r[x >> 5] >> (x & 31)) & 1) | (((g[x >> 5] >> (x & 31)) & 1) << 1) |
                              (((b[x >> 5] >> (x & 31)) & 1) << 2));
                fwrite(rgb[i], 1, 3, f);
            }
        }
    }
    fclose(f);
}

static void test_main_page(const char* dir) {
    setup();
    snprintf(menu.drive[0], sizeof(menu.drive[0]), "Spellbinder");
    menu.drive_src[0] = BBC_SRC_FLASH;
    menu.drive_ro[0] = true;
    bbc_menu_draw(&menu, &surf);
    write_ppm(dir, "bbc_menu.ppm");
    CHECK(screen_contains("Model B"));
    CHECK(screen_contains("Disques"));
    CHECK(screen_contains("Spellbinder"));
    CHECK(screen_contains("3 images .ssd / .dsd"));
    CHECK(screen_contains("2 images en flash"));
    CHECK(screen_contains("Reprendre"));
    CHECK(screen_contains("F11 : ce menu"));
    // The cursor starts on Reprendre: Entrée and Échap resume
    CHECK(bbc_menu_key(&menu, OSD_KEY_ENTER).type == BBC_ACT_RESUME);
    CHECK(bbc_menu_key(&menu, OSD_KEY_ESC).type == BBC_ACT_RESUME);
}

static void test_navigation(void) {
    setup();
    // Buttons: Left / Right move between them, Up / Down go round the items
    bbc_menu_key(&menu, OSD_KEY_LEFT);
    CHECK(menu.cursor == BBC_ITEM_SAVE);
    CHECK(bbc_menu_key(&menu, OSD_KEY_ENTER).type == BBC_ACT_SAVE);
    bbc_menu_key(&menu, OSD_KEY_LEFT);
    CHECK(bbc_menu_key(&menu, OSD_KEY_ENTER).type == BBC_ACT_BOOT);
    bbc_menu_key(&menu, OSD_KEY_LEFT);
    CHECK(bbc_menu_key(&menu, OSD_KEY_ENTER).type == BBC_ACT_BREAK);
    bbc_menu_key(&menu, OSD_KEY_LEFT);
    CHECK(menu.cursor == BBC_ITEM_BREAK);   // Leftmost button
    bbc_menu_key(&menu, OSD_KEY_HOME);
    CHECK(menu.cursor == BBC_ITEM_DRIVE0);
    bbc_menu_key(&menu, OSD_KEY_UP);
    CHECK(menu.cursor == BBC_ITEM_RESUME);   // Wraps round
    bbc_menu_key(&menu, OSD_KEY_DOWN);
    bbc_menu_key(&menu, OSD_KEY_DOWN);
    bbc_menu_key(&menu, OSD_KEY_DOWN);
    bbc_menu_key(&menu, OSD_KEY_DOWN);
    CHECK(menu.cursor == BBC_ITEM_AUTOBOOT);
    // Démarrage: Entrée toggles
    CHECK(menu.autoboot);
    CHECK(bbc_menu_key(&menu, OSD_KEY_ENTER).type == BBC_ACT_AUTOBOOT);
    CHECK(!menu.autoboot);
    // Volume: Left / Right, Entrée mutes
    bbc_menu_key(&menu, OSD_KEY_DOWN);
    CHECK(menu.cursor == BBC_ITEM_VOLUME);
    CHECK(bbc_menu_key(&menu, OSD_KEY_LEFT).type == BBC_ACT_VOLUME);
    CHECK(menu.volume == HID_VOLUME_MAX - 1);
    CHECK(bbc_menu_key(&menu, OSD_KEY_ENTER).type == BBC_ACT_VOLUME && menu.muted);
    bbc_menu_key(&menu, OSD_KEY_RIGHT);
    CHECK(!menu.muted && menu.volume == HID_VOLUME_MAX);
    bbc_menu_draw(&menu, &surf);
    CHECK(screen_contains("rer seulement"));
}

static void test_selector(const char* dir) {
    setup();
    bbc_menu_key(&menu, OSD_KEY_HOME);   // Drive 0
    bbc_menu_key(&menu, OSD_KEY_ENTER);
    CHECK(menu.page == BBC_PAGE_BROWSE);
    CHECK(menu.browse.cursor == 0);   // Empty drive: on « Éjecter »
    bbc_menu_draw(&menu, &surf);
    write_ppm(dir, "bbc_menu_selector.ppm");
    CHECK(screen_contains("Disque pour le lecteur 0"));
    CHECK(screen_contains("REPTON.SSD"));
    CHECK(screen_contains("flash"));
    // Initial letter: K -> Knight Lore (flash)
    bbc_menu_key(&menu, 'K');
    bbc_menu_action_t a = bbc_menu_key(&menu, OSD_KEY_ENTER);
    CHECK(a.type == BBC_ACT_INSERT && a.target == 0 && a.file == 4);
    CHECK(menu.page == BBC_PAGE_MAIN);
    // The platform inserts it; the same image is refused in drive 1
    snprintf(menu.drive[0], sizeof(menu.drive[0]), "Knight Lore");
    menu.drive_src[0] = BBC_SRC_FLASH;
    bbc_menu_key(&menu, OSD_KEY_DOWN);   // Drive 1
    bbc_menu_key(&menu, OSD_KEY_ENTER);
    bbc_menu_key(&menu, 'K');
    bbc_menu_draw(&menu, &surf);
    CHECK(screen_contains("en 0"));
    a = bbc_menu_key(&menu, OSD_KEY_ENTER);
    CHECK(a.type == BBC_ACT_NONE && menu.message_error);
    // Drive 0 again: the cursor is on the image in place
    bbc_menu_key(&menu, OSD_KEY_UP);
    bbc_menu_key(&menu, OSD_KEY_ENTER);
    CHECK(menu.browse.cursor == 5);
    // Échap: back to the main page, nothing done
    CHECK(bbc_menu_key(&menu, OSD_KEY_ESC).type == BBC_ACT_NONE && menu.page == BBC_PAGE_MAIN);
    // Suppr on drive 0 ejects; on an empty drive does nothing
    a = bbc_menu_key(&menu, OSD_KEY_DEL);
    CHECK(a.type == BBC_ACT_EJECT && a.target == 0);
    bbc_menu_key(&menu, OSD_KEY_DOWN);
    CHECK(bbc_menu_key(&menu, OSD_KEY_DEL).type == BBC_ACT_NONE);
    // « Éjecter » in the selector of drive 0
    bbc_menu_key(&menu, OSD_KEY_UP);
    bbc_menu_key(&menu, OSD_KEY_ENTER);
    bbc_menu_key(&menu, OSD_KEY_HOME);
    a = bbc_menu_key(&menu, OSD_KEY_ENTER);
    CHECK(a.type == BBC_ACT_EJECT && a.target == 0);
}

static void test_states(const char* dir) {
    setup();
    static const char* const names[3] = {"BBC0001.STA", "BBC0002.STA", "BBC0003.STA"};
    for (int i = 0; i < 3; i++) menu.states[menu.nstates++] = names[i];
    bbc_menu_key(&menu, OSD_KEY_HOME);
    bbc_menu_key(&menu, OSD_KEY_DOWN);
    bbc_menu_key(&menu, OSD_KEY_DOWN);
    CHECK(menu.cursor == BBC_ITEM_STATE);
    bbc_menu_draw(&menu, &surf);
    write_ppm(dir, "bbc_menu_states_main.ppm");
    CHECK(screen_contains("3 sur la cl"));
    bbc_menu_key(&menu, OSD_KEY_ENTER);
    CHECK(menu.page == BBC_PAGE_BROWSE && menu.browse.cursor == 0);
    bbc_menu_draw(&menu, &surf);
    write_ppm(dir, "bbc_menu_states.ppm");
    CHECK(screen_contains("Nouvel instantan"));
    CHECK(screen_contains("BBC0002.STA"));
    // « Nouvel instantané »: save
    bbc_menu_action_t a = bbc_menu_key(&menu, OSD_KEY_ENTER);
    CHECK(a.type == BBC_ACT_STATE_SAVE && menu.page == BBC_PAGE_MAIN);
    // A file: load
    bbc_menu_key(&menu, OSD_KEY_ENTER);
    bbc_menu_key(&menu, OSD_KEY_DOWN);
    bbc_menu_key(&menu, OSD_KEY_DOWN);
    a = bbc_menu_key(&menu, OSD_KEY_ENTER);
    CHECK(a.type == BBC_ACT_STATE_LOAD && a.file == 1);
    // Without a USB drive: refused with a message
    menu.usb_present = false;
    a = bbc_menu_key(&menu, OSD_KEY_ENTER);
    CHECK(a.type == BBC_ACT_NONE && menu.page == BBC_PAGE_MAIN && menu.message_error);
}

static void test_empty(void) {
    bbc_menu_init(&menu);
    bbc_menu_key(&menu, OSD_KEY_HOME);
    bbc_menu_key(&menu, OSD_KEY_ENTER);
    bbc_menu_draw(&menu, &surf);
    CHECK(screen_contains("Aucune image"));
    // « Éjecter » on an empty drive: nothing
    CHECK(bbc_menu_key(&menu, OSD_KEY_ENTER).type == BBC_ACT_NONE);
    bbc_menu_draw(&menu, &surf);
    CHECK(screen_contains("Aucune cl"));
}

// 256 BBC line buffers at 800x480: every menu line shown, in order, each by one or
// two buffers, and the buffers cover the 480 output lines
static void test_osd_lines(void) {
    int shown[OSD_LINES] = {0};
    int prev = -1, ok = 1, out = 0;
    for (int y = 0; y < 256; y++) {
        int l = bbc_menu_osd_line(y);
        if (l < prev || l >= OSD_LINES) ok = 0;
        shown[l]++;
        prev = l;
        out += (y & 7) == 7 ? 1 : 2;
    }
    CHECK(ok);
    CHECK(out == 2 * OSD_LINES);
    int missing = 0;
    for (int l = 0; l < OSD_LINES; l++) missing += shown[l] == 0;
    CHECK(missing == 0);
    CHECK(bbc_menu_osd_line(0) == 0 && bbc_menu_osd_line(255) == OSD_LINES - 1);
}

static void test_config(void) {
    bbc_config_t c;
    // Absent keys
    bbc_config_parse("", &c, 8);
    CHECK(!c.drive_set[0] && !c.drive_set[1] && c.autoboot == -1 && c.volume == -1);
    // Values, CRLF, comments and unknown keys
    bbc_config_parse("# mes jeux\r\nlecteur0=ELITE.SSD  \r\nlecteur1=@Battlezone\r\ninsertion=inserer\r\nvolume=5\r\nautre=1\r\n", &c,
                     8);
    CHECK(!strcmp(c.drive[0], "ELITE.SSD") && !strcmp(c.drive[1], "@Battlezone"));
    CHECK(c.drive_set[0] && c.drive_set[1] && c.autoboot == 0 && c.volume == 5);
    bbc_config_parse("lecteur1=\ninsertion=demarrer\nvolume=12\n", &c, 8);
    CHECK(!c.drive_set[0] && c.drive_set[1] && c.drive[1][0] == 0 && c.autoboot == 1 && c.volume == -1);
    bbc_config_parse("insertion=peut-etre\n", &c, 8);
    CHECK(c.autoboot == -1);
    // Rewrite: other lines kept, ours replaced, read back the same
    static char out[512];
    const char* drives[2] = {"REPTON.SSD", ""};
    size_t n = bbc_config_merge("# mes jeux\r\nlecteur0=ELITE.SSD\r\nautre=1\r\nvolume=2\r\n", drives, true, 7, out, sizeof(out));
    CHECK(n == strlen(out));
    CHECK(!strcmp(out, "# mes jeux\nautre=1\nlecteur0=REPTON.SSD\nlecteur1=\ninsertion=demarrer\nvolume=7\n"));
    bbc_config_parse(out, &c, 8);
    CHECK(!strcmp(c.drive[0], "REPTON.SSD") && c.drive_set[1] && !c.drive[1][0] && c.autoboot == 1 && c.volume == 7);
    // New file: a header comment
    n = bbc_config_merge(NULL, drives, false, 8, out, sizeof(out));
    CHECK(out[0] == '#' && strstr(out, "insertion=inserer\n") && strstr(out, "volume=8\n"));
    // Small buffer: truncated, still terminated
    n = bbc_config_merge(NULL, drives, false, 8, out, 16);
    CHECK(n == 15 && out[15] == 0);
}

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : NULL;
    test_main_page(dir);
    test_navigation();
    test_selector(dir);
    test_states(dir);
    test_empty();
    test_osd_lines();
    test_config();
    printf("%d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
