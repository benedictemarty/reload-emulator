// test_osd_panel.c — style commun des panneaux (src/osd/osd_panel.h)

#include <stdio.h>
#include <string.h>

#include "osd/osd_panel.h"

static int tests_run, tests_failed;
#define CHECK(c)                                                         \
    do {                                                                 \
        tests_run++;                                                     \
        if (!(c)) {                                                      \
            tests_failed++;                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
        }                                                                \
    } while (0)

static osd_surface_t s;

static void test_size(void) {
    char b[16];
    osd_panel_size(b, sizeof(b), 0);
    CHECK(!strcmp(b, "0 Ko"));
    osd_panel_size(b, sizeof(b), 1);
    CHECK(!strcmp(b, "1 Ko"));   // Rounded up
    osd_panel_size(b, sizeof(b), 1024 * 1024 + 512 * 1024);
    CHECK(!strcmp(b, "1,5 Mo"));
}

static void test_item_and_button(void) {
    osd_clear(&s, OSD_PANEL_BG);
    osd_panel_item_bar(&s, 5, 10, 20, true);
    CHECK(s.ch[5][10] == OSD_TRI_R && s.attr[5][10] == OSD_PANEL_SEL_ACC && s.attr[5][29] == OSD_PANEL_SEL);
    osd_panel_item_bar(&s, 6, 10, 20, false);
    CHECK(s.ch[6][10] == ' ' && s.attr[6][10] == OSD_PANEL);
    CHECK(osd_panel_err(true) == OSD_PANEL_SEL_ERR && osd_panel_acc(false) == OSD_PANEL_ACC);
    osd_panel_button(&s, 8, 0, 10, "OK", true);
    CHECK(s.ch[8][4] == 'O' && s.attr[8][0] == OSD_PANEL_KEY);   // Centred
}

static void test_popup_and_scrollbar(void) {
    osd_clear(&s, OSD_PANEL_BG);
    osd_panel_popup(&s, 2, 10, 8, 30, OSD_FLOP_L, "Titre");
    CHECK(s.attr[2 + 8][10 + 30] == OSD_PANEL);        // Shadow, bottom right
    CHECK(s.attr[3][11] == OSD_PANEL_POPUP);           // Inside
    CHECK(s.ch[2][13] == OSD_FLOP_L && s.ch[2][16] == 'T');
    uint8_t base, dim;
    osd_panel_popup_row(&s, 4, 10, 30, true, &base, &dim);
    CHECK(base == OSD_PANEL_POPUP_SEL && dim == OSD_PANEL_POPUP_SEL_DIM && s.attr[4][12] == base);
    // Nothing when everything fits; thumb at the top, then at the bottom
    osd_panel_scrollbar(&s, 3, 38, 5, 0, 5);
    CHECK(s.ch[3][38] != OSD_SHADE && s.ch[3][38] != OSD_FULL);
    osd_panel_scrollbar(&s, 3, 38, 5, 0, 20);
    CHECK(s.ch[3][38] == OSD_FULL && s.ch[7][38] == OSD_SHADE);
    osd_panel_scrollbar(&s, 3, 38, 5, 15, 20);
    CHECK(s.ch[7][38] == OSD_FULL);
}

static void test_footer_and_message(void) {
    osd_clear(&s, OSD_PANEL_BG);
    const char* const help[2][2] = {{" Entrée ", "valider"}, {" Échap ", "retour"}};
    osd_panel_footer(&s, 20, 2, help, 2, 80, "F1");
    CHECK(s.attr[20][2] == OSD_PANEL_KEY && s.attr[21][0] == OSD_PANEL && s.ch[20][80] == 'F');
    osd_panel_message(&s, 18, 3, true, "Erreur", -1);
    CHECK(s.ch[18][3] == OSD_CROSS && s.attr[18][5] == OSD_ATTR(OSD_RED, OSD_BLACK));
}

int main(void) {
    test_size();
    test_item_and_button();
    test_popup_and_scrollbar();
    test_footer_and_message();
    printf("%d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
