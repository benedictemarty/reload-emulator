// test_oric_menu.c
//
// Tests of the Oric control panel (src/osd/oric_menu.h), of ORIC.CFG
// (src/osd/oric_config.h) and of the OSD surface rendering (src/osd/osd.h).
//
//   oric_menu_test [preview-dir]   with a directory: also writes PPM previews
//                                  of the main page and of the selectors
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

#include "osd/oric_menu.h"
#include "osd/oric_config.h"
#include "systems/oric_planes.h"

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
static oric_menu_t menu;

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

static void add_file(const char* name, uint32_t size, uint8_t kind) {
    oric_menu_file_t* f = &menu.files[menu.nfiles++];
    snprintf(f->name, sizeof(f->name), "%s", name);
    f->size = size;
    f->kind = kind;
}

static void setup(void) {
    oric_menu_init(&menu);
    menu.version = "v1.0";
    menu.fdc = 2;   // Microdisc
    menu.fdc_available[1] = true;
    menu.fdc_available[2] = true;
    menu.usb_present = true;
    add_file("SEDORIC.DSK", 537856, ORIC_FILE_DSK);
    add_file("FTDOS.DSK", 262656, ORIC_FILE_DSK);
    add_file("GAME.TAP", 8014, ORIC_FILE_TAP);
    add_file("ZORGONS.DSK", 1049856, ORIC_FILE_DSK);
    add_file("SIEVE.TAP", 723, ORIC_FILE_TAP);
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
    static uint32_t line32[OSD_WIDTH / 4];
    uint8_t* line = (uint8_t*)line32;
    for (int y = 0; y < OSD_LINES; y++) {
        osd_render_line(&surf, y, line);
        for (int rep = 0; rep < 2; rep++) {   // Each buffer line shown twice, as on the Neo6502
            for (int x = 0; x < OSD_WIDTH; x++) fwrite(rgb[line[x] & 7], 1, 3, f);
        }
    }
    fclose(f);
}

static void test_render(void) {
    osd_clear(&surf, OSD_ATTR(OSD_WHITE, OSD_BLUE));
    osd_putc(&surf, 0, 0, 'A', OSD_ATTR(OSD_YELLOW, OSD_BLUE));
    osd_putc(&surf, 0, 1, ' ', OSD_ATTR(OSD_WHITE, OSD_RED | OSD_DITHER));
    static uint32_t line32[OSD_WIDTH / 4];
    uint8_t* line = (uint8_t*)line32;
    int ink = 0, paper = 0;
    for (int y = 0; y < 8; y++) {
        osd_render_line(&surf, y, line);
        for (int x = 0; x < 8; x++) {
            bool on = (osd_font['A'][y] >> x) & 1;
            if (on && line[x] == OSD_YELLOW) ink++;
            if (!on && line[x] == OSD_BLUE) paper++;
        }
        // Dithered background: red one pixel out of two, shifted every line
        for (int x = 8; x < 16; x++) {
            uint8_t want = ((x - 8) ^ y) & 1 ? OSD_BLACK : OSD_RED;
            if (line[x] != want) paper -= 1000;
        }
    }
    int on_total = 0;
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) on_total += (osd_font['A'][y] >> x) & 1;
    }
    CHECK(on_total > 0 && ink == on_total && paper == 64 - on_total);
    // UTF-8 -> Latin-1 cells
    const char* p = "é—";
    CHECK(osd_next_char(&p) == 0xE9 && osd_next_char(&p) == OSD_EMDASH && *p == 0);
    CHECK(osd_strlen("Clé USB") == 7);
    // Big letters: two cells, pixels doubled
    osd_puts_big(&surf, 1, 0, "I", OSD_ATTR(OSD_WHITE, OSD_BLUE));
    CHECK(surf.big[1][0] == OSD_BIG_LEFT && surf.big[1][1] == OSD_BIG_RIGHT);
}

static void test_navigation(const char* dir) {
    setup();
    oric_menu_draw(&menu, &surf);
    CHECK(screen_contains("panneau de contr"));
    CHECK(surf.ch[1][3] == 'O' && surf.big[1][3] == OSD_BIG_LEFT);   // "ORIC / PRAVETZ / NOVA" in big letters
    CHECK(screen_contains("PPRRAAVVEETTZZ") && screen_contains("NNOOVVAA"));
    CHECK(screen_contains("Interface disque"));
    CHECK(screen_contains("Microdisc"));
    CHECK(screen_contains("Disquettes"));
    CHECK(screen_contains("Cassette"));
    CHECK(screen_contains("2 .tap"));
    CHECK(screen_contains("Reprendre"));
    write_ppm(dir, "oric_menu_main.ppm");

    // Escape resumes; the cursor starts on "Reprendre"
    CHECK(oric_menu_key(&menu, OSD_KEY_ESC).type == ORIC_ACT_RESUME);
    CHECK(oric_menu_key(&menu, OSD_KEY_ENTER).type == ORIC_ACT_RESUME);
    // Down wraps to the interface, then drive A
    oric_menu_key(&menu, OSD_KEY_DOWN);
    CHECK(menu.cursor == ORIC_ITEM_FDC);
    oric_menu_key(&menu, OSD_KEY_DOWN);
    CHECK(menu.cursor == ORIC_ITEM_DRIVE0);

    // Drive A: selector with the .dsk files only, row 0 = eject
    oric_menu_key(&menu, OSD_KEY_ENTER);
    CHECK(menu.page == ORIC_PAGE_BROWSE && menu.browse_count == 3);
    oric_menu_draw(&menu, &surf);
    CHECK(screen_contains("Disquette pour le lecteur A"));
    CHECK(screen_contains("ZORGONS.DSK") && !screen_contains("GAME.TAP"));
    write_ppm(dir, "oric_menu_disk.ppm");
    // Jump to the initial typed, then insert
    oric_menu_key(&menu, 'z');
    CHECK(menu.browse_cursor == 3);
    oric_menu_action_t a = oric_menu_key(&menu, OSD_KEY_ENTER);
    CHECK(a.type == ORIC_ACT_INSERT && a.target == 0 && a.file == 3 && menu.page == ORIC_PAGE_MAIN);
    snprintf(menu.drive[0], sizeof(menu.drive[0]), "ZORGONS.DSK");

    // The same image in drive B is refused
    oric_menu_key(&menu, OSD_KEY_DOWN);
    oric_menu_key(&menu, OSD_KEY_ENTER);
    oric_menu_key(&menu, 'Z');
    a = oric_menu_key(&menu, OSD_KEY_ENTER);
    CHECK(a.type == ORIC_ACT_NONE && menu.message_error && strstr(menu.message, "lecteur A"));
    // Eject from the selector (row 0), and with Suppr on the main page
    oric_menu_key(&menu, OSD_KEY_ENTER);
    oric_menu_key(&menu, OSD_KEY_HOME);
    a = oric_menu_key(&menu, OSD_KEY_ENTER);
    CHECK(a.type == ORIC_ACT_EJECT && a.target == 1);
    oric_menu_key(&menu, OSD_KEY_UP);
    a = oric_menu_key(&menu, OSD_KEY_DEL);
    CHECK(a.type == ORIC_ACT_EJECT && a.target == 0);

    // Cassette: .tap files; cursor on the tape in place
    snprintf(menu.tape, sizeof(menu.tape), "SIEVE.TAP");
    menu.tape_percent = 40;
    menu.tape_motor = true;
    oric_menu_key(&menu, OSD_KEY_RIGHT);
    CHECK(menu.cursor == ORIC_ITEM_TAPE);
    oric_menu_key(&menu, OSD_KEY_ENTER);
    CHECK(menu.browse_count == 2 && menu.browse_cursor == 2);
    oric_menu_draw(&menu, &surf);
    CHECK(screen_contains("Cassette (la m\xEA" "me : rembobin\xE9" "e)"));   // Latin-1 cells
    CHECK(screen_contains("GAME.TAP"));
    write_ppm(dir, "oric_menu_tape.ppm");
    oric_menu_key(&menu, OSD_KEY_UP);
    a = oric_menu_key(&menu, OSD_KEY_ENTER);
    CHECK(a.type == ORIC_ACT_TAPE_INSERT && a.file == 2);
    oric_menu_key(&menu, OSD_KEY_LEFT);
    CHECK(menu.cursor == ORIC_ITEM_FDC);

    // Interface: the list of the 4, a missing ROM is refused
    oric_menu_key(&menu, OSD_KEY_ENTER);
    CHECK(menu.page == ORIC_PAGE_BROWSE && menu.browse_count == 4 && menu.browse_cursor == 2);
    oric_menu_draw(&menu, &surf);
    CHECK(screen_contains("ROM absente"));
    write_ppm(dir, "oric_menu_fdc.ppm");
    oric_menu_key(&menu, OSD_KEY_DOWN);   // Jasmin: no ROM
    a = oric_menu_key(&menu, OSD_KEY_ENTER);
    CHECK(a.type == ORIC_ACT_NONE && menu.message_error && strstr(menu.message, "Jasmin"));
    oric_menu_key(&menu, OSD_KEY_ENTER);
    oric_menu_key(&menu, 'p');
    a = oric_menu_key(&menu, OSD_KEY_ENTER);
    CHECK(a.type == ORIC_ACT_SET_FDC && a.file == 1);

    // Without Microdisc / Jasmin, the drives are not available
    menu.fdc = 1;
    oric_menu_key(&menu, OSD_KEY_DOWN);
    a = oric_menu_key(&menu, OSD_KEY_ENTER);
    CHECK(a.type == ORIC_ACT_NONE && menu.page == ORIC_PAGE_MAIN && menu.message_error);

    // Buttons
    oric_menu_key(&menu, OSD_KEY_END);
    CHECK(menu.cursor == ORIC_ITEM_RESUME);
    oric_menu_key(&menu, OSD_KEY_LEFT);
    CHECK(oric_menu_key(&menu, OSD_KEY_ENTER).type == ORIC_ACT_SAVE);
    oric_menu_key(&menu, OSD_KEY_LEFT);
    CHECK(oric_menu_key(&menu, OSD_KEY_ENTER).type == ORIC_ACT_RESET);

    // Many files: scrolling keeps the cursor visible
    setup();
    for (int i = 0; i < 40; i++) {
        char name[16];
        snprintf(name, sizeof(name), "D%02d.DSK", i);
        add_file(name, 1000, ORIC_FILE_DSK);
    }
    menu.cursor = ORIC_ITEM_DRIVE0;
    oric_menu_key(&menu, OSD_KEY_ENTER);
    oric_menu_key(&menu, OSD_KEY_END);
    CHECK(menu.browse_cursor == menu.browse_count && menu.browse_scroll == menu.browse_cursor - ORIC_BROWSE_VISIBLE + 1);
    oric_menu_key(&menu, OSD_KEY_PGUP);
    CHECK(menu.browse_cursor >= menu.browse_scroll && menu.browse_cursor < menu.browse_scroll + ORIC_BROWSE_VISIBLE);
    oric_menu_draw(&menu, &surf);
    write_ppm(dir, "oric_menu_scroll.ppm");
}

static void test_config(void) {
    CHECK(oric_config_fdc("microdisc") == 2 && oric_config_fdc("jasmin\r\n") == 3 && oric_config_fdc("aucune") == 0);
    CHECK(oric_config_fdc("pravetz8d") == -1 && oric_config_fdc("x") == -1);
    CHECK(oric_config_value("a=GAME.DSK", "a") && !strcmp(oric_config_value("a=GAME.DSK", "a"), "GAME.DSK"));
    CHECK(oric_config_value("ab=X", "a") == NULL && oric_config_value("tape=T.TAP", "tap") == NULL);
    char v[32];
    oric_config_copy(v, sizeof(v), "SEDORIC.DSK  \r\n");
    CHECK(!strcmp(v, "SEDORIC.DSK"));
    oric_config_copy(v, 5, "LONGNAME.DSK");
    CHECK(!strcmp(v, "LONG"));
    const char* old = "# ma configuration\r\nfdc=jasmin\r\na=OLD.DSK\r\nautre=1\r\ntape=OLD.TAP\n";
    const char* drives[4] = {"NEW.DSK", "", NULL, "D.DSK"};
    char out[512];
    size_t n = oric_config_merge(old, 2, drives, "NEW.TAP", out, sizeof(out));
    CHECK(n == strlen(out));
    CHECK(!strcmp(out, "# ma configuration\nautre=1\nfdc=microdisc\na=NEW.DSK\nd=D.DSK\ntape=NEW.TAP\n"));
    // No old file, nothing inserted
    const char* none[4] = {0};
    oric_config_merge(NULL, 0, none, "", out, sizeof(out));
    CHECK(!strcmp(out, "fdc=aucune\n"));
    // Truncated output stays a string
    n = oric_config_merge(old, 2, drives, "NEW.TAP", out, 16);
    CHECK(n == 15 && strlen(out) == 15);
}

// The 1-bit planes of the RP2040 (tmds_encode_1bpp) give the same picture as the index rendering
static int planes_mismatches(void) {
    static uint32_t line32[OSD_WIDTH / 4];
    static uint32_t r[OSD_COLS / 4], g[OSD_COLS / 4], b[OSD_COLS / 4];
    const uint8_t* line = (const uint8_t*)line32;
    int bad = 0;
    for (int y = 0; y < OSD_LINES; y++) {
        osd_render_line(&surf, y, (uint8_t*)line32);
        osd_render_line_planes(&surf, y, r, g, b);
        for (int x = 0; x < OSD_WIDTH; x++) {
            int c = (int)((r[x >> 5] >> (x & 31)) & 1) | (int)(((g[x >> 5] >> (x & 31)) & 1) << 1) |
                    (int)(((b[x >> 5] >> (x & 31)) & 1) << 2);
            if (c != line[x]) bad++;
        }
    }
    return bad;
}

static void test_planes(void) {
    setup();
    oric_menu_draw(&menu, &surf);
    CHECK(planes_mismatches() == 0);
    menu.cursor = ORIC_ITEM_FDC;
    oric_menu_key(&menu, OSD_KEY_ENTER);
    oric_menu_draw(&menu, &surf);
    CHECK(planes_mismatches() == 0);
    // Dithered backgrounds too
    osd_clear(&surf, OSD_ATTR(OSD_YELLOW, OSD_BLUE | OSD_DITHER));
    osd_puts_big(&surf, 3, 2, "Test é", OSD_ATTR(OSD_WHITE, OSD_MAGENTA | OSD_DITHER));
    CHECK(planes_mismatches() == 0);

    // Oric picture: 240 pixels of 4 bits -> tripled in the planes from x0, borders cleared
    static uint8_t src[ORIC_PLANES_BYTES_PER_LINE];
    for (int i = 0; i < ORIC_PLANES_BYTES_PER_LINE; i++) src[i] = (uint8_t)(i * 37 + 11);
    oric_planes_init();
    static uint32_t pr[OSD_WIDTH / 32], pg[OSD_WIDTH / 32], pb[OSD_WIDTH / 32];
    const unsigned x0 = (OSD_WIDTH - ORIC_PLANES_PIXELS) / 2;
    memset(pr, 0, sizeof(pr));
    memset(pg, 0, sizeof(pg));
    memset(pb, 0, sizeof(pb));
    oric_planes_line(src, pr, pg, pb, x0);
    int bad = 0;
    for (int x = 0; x < OSD_WIDTH; x++) {
        int want = 0;
        if (x >= (int)x0 && x < (int)x0 + ORIC_PLANES_PIXELS) {
            int px = (x - (int)x0) / 3;
            uint8_t byte = src[px / 2];
            want = (px & 1) ? (byte & 15) : (byte >> 4);
            want &= 7;
        }
        int c = (int)((pr[x >> 5] >> (x & 31)) & 1) | (int)(((pg[x >> 5] >> (x & 31)) & 1) << 1) |
                (int)(((pb[x >> 5] >> (x & 31)) & 1) << 2);
        if (c != want) bad++;
    }
    CHECK(bad == 0);
}

static void test_profiles(const char* dir) {
    oric_profile_t p;
    CHECK(oric_config_profile("Oric-1 BASIC 1.0 ; rom=BASIC10.ROM ; tape=JEU.TAP\r\n", &p));
    CHECK(!strcmp(p.label, "Oric-1 BASIC 1.0") && !strcmp(p.rom, "BASIC10.ROM") && !strcmp(p.tape, "JEU.TAP") && p.fdc == -1);
    CHECK(oric_config_profile("Nova 64;rom=NOVA64.ROM;fdc=microdisc;a=SEDORIC.DSK;c=DATA.DSK", &p));
    CHECK(p.fdc == 2 && !strcmp(p.drive[0], "SEDORIC.DSK") && p.drive[1][0] == 0 && !strcmp(p.drive[2], "DATA.DSK"));
    CHECK(!oric_config_profile(";rom=X.ROM", &p));   // No label
    CHECK(oric_config_profile("Seul", &p) && !strcmp(p.label, "Seul") && !p.rom[0]);
    // The ORIC.CFG merge keeps the profiles and the start choice
    const char* old = "profil=Nova 64;rom=NOVA64.ROM\ndemarrage=choix\nfdc=jasmin\n";
    const char* none[4] = {0};
    char out[256];
    oric_config_merge(old, 2, none, "", out, sizeof(out));
    CHECK(!strcmp(out, "profil=Nova 64;rom=NOVA64.ROM\ndemarrage=choix\nfdc=microdisc\n"));

    // Menu: "Profil" item, "Démarrer sur…" selector
    setup();
    menu.profile[0] = "Oric Atmos (BASIC 1.1)";
    menu.profile[1] = "Atmos + Microdisc (Sedoric)";
    menu.profile[2] = "Nova 64";
    oric_menu_draw(&menu, &surf);
    CHECK(screen_contains("Profil") && screen_contains("utilisateur"));
    oric_menu_open_profiles(&menu);
    CHECK(menu.page == ORIC_PAGE_BROWSE && menu.browse_count == 3 && menu.browse_cursor == 0);
    oric_menu_draw(&menu, &surf);
    CHECK(screen_contains("Nova 64") && screen_contains("Atmos + Microdisc"));
    write_ppm(dir, "oric_menu_profils.ppm");
    oric_menu_key(&menu, 'n');
    oric_menu_action_t a = oric_menu_key(&menu, OSD_KEY_ENTER);
    CHECK(a.type == ORIC_ACT_PROFILE && a.file == 2);
    menu.profile_cur = 2;
    oric_menu_draw(&menu, &surf);
    CHECK(screen_contains("Nova 64") && !screen_contains("utilisateur"));
    // From the main page: the Profil item, reached from the tape with Down
    menu.cursor = ORIC_ITEM_TAPE;
    oric_menu_key(&menu, OSD_KEY_DOWN);
    CHECK(menu.cursor == ORIC_ITEM_PROFILE);
    oric_menu_key(&menu, OSD_KEY_ENTER);
    CHECK(menu.page == ORIC_PAGE_BROWSE && menu.browse_cursor == 2);
    oric_menu_key(&menu, OSD_KEY_ESC);
    CHECK(menu.page == ORIC_PAGE_MAIN);
    oric_menu_key(&menu, OSD_KEY_LEFT);
    CHECK(menu.cursor == ORIC_ITEM_FDC);
}

static void test_banner(const char* dir) {
    static osd_row_t row;
    oric_tape_banner(&row, "AIGLE.TAP", 40);
    char text[OSD_COLS + 1];
    for (int c = 0; c < OSD_COLS; c++) text[c] = (char)row.ch[c];
    text[OSD_COLS] = 0;
    CHECK(strstr(text, "Lecture") && strstr(text, "AIGLE.TAP") && strstr(text, " 40 %"));
    int full = 0, shade = 0;
    for (int c = 0; c < OSD_COLS; c++) {
        full += row.ch[c] == OSD_FULL;
        shade += row.ch[c] == OSD_SHADE;
    }
    CHECK(full == 12 && shade == 18);   // 40 % of 30 cells
    // Rendered like a surface row (planes), preview in a 8-line strip
    static uint32_t r[OSD_COLS / 4], g[OSD_COLS / 4], b[OSD_COLS / 4];
    int lit = 0;
    for (int y = 0; y < 8; y++) {
        osd_render_cells_planes(row.ch, row.attr, row.big, y, y, r, g, b);
        for (int w = 0; w < OSD_COLS / 4; w++) lit += __builtin_popcount(b[w]);   // Blue background
    }
    CHECK(lit > OSD_WIDTH * 8 / 2);
    if (dir) {
        osd_clear(&surf, OSD_ATTR(OSD_WHITE, OSD_BLACK));
        memcpy(surf.ch[29], row.ch, OSD_COLS);
        memcpy(surf.attr[29], row.attr, OSD_COLS);
        write_ppm(dir, "oric_banner.ppm");
    }
}

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : NULL;
    test_render();
    test_navigation(dir);
    test_config();
    test_planes();
    test_profiles(dir);
    test_banner(dir);
    printf("%d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
