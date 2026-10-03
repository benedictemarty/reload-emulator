// test_osd_browser.c — sélecteur commun des menus (src/osd/osd_browser.h)

#include <stdio.h>
#include <string.h>

#include "osd/osd_browser.h"

static int tests_run, tests_failed;
#define CHECK(c)                                                         \
    do {                                                                 \
        tests_run++;                                                     \
        if (!(c)) {                                                      \
            tests_failed++;                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
        }                                                                \
    } while (0)

static const char* names[] = {"ALPHA.DSK", "beta.dsk", "BRAVO.DSK", "CHARLIE.DSK", "delta.dsk"};
static const char* name(void* user, int k) { (void)user; return names[k]; }

static void test_moves(void) {
    osd_browser_t b;
    osd_browser_open(&b, 5, 1, 0, 3);   // « Éjecter » + 5 entrées, 3 lignes visibles
    CHECK(osd_browser_entry(&b) == -1);
    CHECK(osd_browser_key(&b, OSD_KEY_UP, name, 0) == OSD_BROWSER_NONE && b.cursor == 5);   // En boucle
    CHECK(b.scroll == 3);                                                                 // Rendu visible
    CHECK(osd_browser_key(&b, OSD_KEY_DOWN, name, 0) == OSD_BROWSER_NONE && b.cursor == 0 && b.scroll == 0);
    osd_browser_key(&b, OSD_KEY_PGDN, name, 0);
    CHECK(b.cursor == 3 && b.scroll == 1);
    osd_browser_key(&b, OSD_KEY_PGDN, name, 0);
    CHECK(b.cursor == 5);   // Borné
    osd_browser_key(&b, OSD_KEY_HOME, name, 0);
    CHECK(b.cursor == 0 && b.scroll == 0);
    osd_browser_key(&b, OSD_KEY_END, name, 0);
    CHECK(b.cursor == 5 && osd_browser_entry(&b) == 4);
    CHECK(osd_browser_key(&b, OSD_KEY_ENTER, name, 0) == OSD_BROWSER_PICK);
    CHECK(osd_browser_key(&b, OSD_KEY_ESC, name, 0) == OSD_BROWSER_BACK);
    CHECK(osd_browser_key(&b, OSD_KEY_LEFT, name, 0) == OSD_BROWSER_BACK);
}

static void test_letters(void) {
    osd_browser_t b;
    osd_browser_open(&b, 5, 1, 0, 3);
    osd_browser_key(&b, 'b', name, 0);   // Sans tenir compte de la casse : « beta »
    CHECK(osd_browser_entry(&b) == 1);
    osd_browser_key(&b, 'B', name, 0);   // L'entrée suivante en B : « BRAVO »
    CHECK(osd_browser_entry(&b) == 2);
    osd_browser_key(&b, 'b', name, 0);   // Puis on revient à « beta »
    CHECK(osd_browser_entry(&b) == 1);
    osd_browser_key(&b, 'z', name, 0);   // Aucune : le curseur ne bouge pas
    CHECK(osd_browser_entry(&b) == 1);
}

static void test_empty_and_open(void) {
    osd_browser_t b;
    osd_browser_open(&b, 0, 0, 0, 16);   // Liste vide sans ligne fixe
    osd_browser_key(&b, OSD_KEY_DOWN, name, 0);
    CHECK(b.cursor == 0 && b.scroll == 0);   // L'Oric laissait -1 (ligne « Éjecter » parasite)
    osd_browser_open(&b, 40, 1, 30, 16);     // Ouvert loin dans la liste : visible tout de suite
    CHECK(b.cursor == 30 && b.scroll == 15);
}

static void test_hid_keys(void) {
    CHECK(osd_key_from_hid(0x152) == OSD_KEY_UP && osd_key_from_hid(0x0D) == OSD_KEY_ENTER);
    CHECK(osd_key_from_hid(0x7F) == OSD_KEY_DEL && osd_key_from_hid(0x08) == OSD_KEY_DEL);
    CHECK(osd_key_from_hid('a') == 'a' && osd_key_from_hid(' ') == -1 && osd_key_from_hid(0x13A) == -1);
}

int main(void) {
    test_moves();
    test_letters();
    test_empty_and_open();
    test_hid_keys();
    printf("%d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
