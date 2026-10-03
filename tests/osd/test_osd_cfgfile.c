// test_osd_cfgfile.c — fichier de configuration commun des menus (src/osd/osd_cfgfile.h)

#include <stdio.h>
#include <string.h>

#include "osd/osd_cfgfile.h"

static int tests_run, tests_failed;
#define CHECK(c)                                                         \
    do {                                                                 \
        tests_run++;                                                     \
        if (!(c)) {                                                      \
            tests_failed++;                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
        }                                                                \
    } while (0)

static void test_read(void) {
    const char* text = "# commentaire\r\nvideo=640\r\nmode=pal \nnom=  JEU.DSK  \n\nfin=1";
    int lines = 0, video = -1, mode = -1;
    char nom[16] = "";
    static const char* const modes[2] = {"ntsc", "pal"};
    for (const char* l = text; l; l = osd_cfg_next(l)) {
        const char* v;
        lines++;
        if ((v = osd_cfg_value(l, "video"))) video = osd_cfg_int(v, 999);
        if ((v = osd_cfg_value(l, "mode"))) mode = osd_cfg_choice(v, modes, 2);
        if ((v = osd_cfg_value(l, "nom"))) osd_cfg_copy(nom, sizeof(nom), v);
    }
    CHECK(lines == 6);
    CHECK(video == 640 && mode == 1);
    CHECK(!strcmp(nom, "  JEU.DSK"));   // Espaces de tête gardés, de fin retirés (comme ORIC.CFG)
    CHECK(osd_cfg_int("1000", 999) == -1 && osd_cfg_int("12x", 99) == -1 && osd_cfg_int("", 9) == -1);
    CHECK(osd_cfg_choice("palx", modes, 2) == -1 && osd_cfg_value("videox=1", "video") == NULL);
}

static void test_fields(void) {
    char f[16];
    const char* next = osd_cfg_field(" Mon profil ; a=X.DSK;rom=R.ROM\r\n", f, sizeof(f));
    CHECK(!strcmp(f, "Mon profil") && next);
    next = osd_cfg_field(next, f, sizeof(f));
    CHECK(!strcmp(f, "a=X.DSK") && next);
    next = osd_cfg_field(next, f, sizeof(f));
    CHECK(!strcmp(f, "rom=R.ROM") && next == NULL);
}

static void test_write(void) {
    char out[64];
    osd_cfg_writer_t w;
    static const char* const ours[2] = {"video", "mode"};
    osd_cfg_writer_init(&w, out, sizeof(out));
    osd_cfg_keep(&w, "# c\r\nvideo=800\nautre=1\nmode=ntsc\n", ours, 2);
    osd_cfg_put(&w, "video=%d\n", 640);
    CHECK(!strcmp(out, "# c\nautre=1\nvideo=640\n") && w.len == strlen(out));
    osd_cfg_writer_init(&w, out, 8);   // Tronqué, toujours terminé
    osd_cfg_put(&w, "%s", "0123456789");
    CHECK(w.len == 7 && strlen(out) == 7);
    osd_cfg_put(&w, "x");
    CHECK(w.len == 7);
}

int main(void) {
    test_read();
    test_fields();
    test_write();
    printf("%d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
