// test_hid_media.c — multimedia keys of a USB keyboard (devices/hid_media.h):
// HID report descriptors (array and variable fields) and output volume.
// Checks taken from ~/Neo6502TeleStrat (tests/test_telestrat.c, test_hid_media).
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

#include <stdio.h>
#include "devices/hid_media.h"

static int tests_run, tests_failed;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        tests_run++;                                      \
        if (!(cond)) {                                    \
            tests_failed++;                               \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);   \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
        }                                                 \
    } while (0)

// Touches multimédia du clavier USB (v0.16.9) : descripteurs HID et volume
static void test_hid_media(void) {
    hid_media_t m;
    // Forme tableau : TUD_HID_REPORT_DESC_CONSUMER de TinyUSB (identifiant 3),
    // après TUD_HID_REPORT_DESC_SYSTEM_CONTROL (identifiant 2)
    static const uint8_t arr[] = {
        0x05, 0x01, 0x09, 0x80, 0xA1, 0x01, 0x85, 0x02, 0x15, 0x01, 0x25, 0x03, 0x95, 0x01, 0x75, 0x02,
        0x09, 0x81, 0x09, 0x82, 0x09, 0x83, 0x81, 0x00, 0x95, 0x01, 0x75, 0x06, 0x81, 0x03, 0xC0,
        0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x03, 0x15, 0x00, 0x26, 0xFF, 0x03, 0x19, 0x00,
        0x2A, 0xFF, 0x03, 0x95, 0x01, 0x75, 0x10, 0x81, 0x00, 0xC0};
    CHECK(hid_media_parse(&m, arr, sizeof(arr)) && m.num_fields == 1 && m.ids, "tableau : un champ Consumer");
    CHECK(hid_media_keys(&m, (const uint8_t[]){0x03, 0xE9, 0x00}, 3) == HID_MEDIA_VOL_UP, "tableau : Volume +");
    CHECK(hid_media_keys(&m, (const uint8_t[]){0x03, 0xEA, 0x00}, 3) == HID_MEDIA_VOL_DOWN, "tableau : Volume -");
    CHECK(hid_media_keys(&m, (const uint8_t[]){0x03, 0xE2, 0x00}, 3) == HID_MEDIA_MUTE, "tableau : Muet");
    CHECK(hid_media_keys(&m, (const uint8_t[]){0x03, 0x00, 0x00}, 3) == 0, "tableau : relâché");
    CHECK(hid_media_keys(&m, (const uint8_t[]){0x03, 0xCD, 0x00}, 3) == 0, "tableau : Lecture/Pause ignorée");
    CHECK(hid_media_keys(&m, (const uint8_t[]){0x02, 0xE9}, 2) == 0, "tableau : autre identifiant ignoré");
    // Sans identifiant de rapport
    CHECK(hid_media_parse(&m, arr + 31 + 0, 6) == false, "descripteur tronqué : aucune touche");
    static const uint8_t noid[] = {0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x15, 0x00, 0x26, 0xFF, 0x03, 0x19, 0x00,
                                   0x2A, 0xFF, 0x03, 0x95, 0x02, 0x75, 0x10, 0x81, 0x00, 0xC0};
    CHECK(hid_media_parse(&m, noid, sizeof(noid)) && !m.ids, "sans identifiant");
    CHECK(hid_media_keys(&m, (const uint8_t[]){0xCD, 0x00, 0xE2, 0x00}, 4) == HID_MEDIA_MUTE, "sans identifiant : 2e élément");
    // Forme variable : un bit par usage (B5 B6 B7 CD E2 E9 EA, 0x0223), identifiant 4
    static const uint8_t var[] = {0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x04, 0x15, 0x00, 0x25, 0x01,
                                  0x75, 0x01, 0x95, 0x08, 0x09, 0xB5, 0x09, 0xB6, 0x09, 0xB7, 0x09, 0xCD,
                                  0x09, 0xE2, 0x09, 0xE9, 0x09, 0xEA, 0x0A, 0x23, 0x02, 0x81, 0x02, 0xC0};
    CHECK(hid_media_parse(&m, var, sizeof(var)) && m.num_fields == 1 && !m.field[0].array, "variable : un champ");
    CHECK(hid_media_keys(&m, (const uint8_t[]){0x04, 0x20}, 2) == HID_MEDIA_VOL_UP, "variable : Volume +");
    CHECK(hid_media_keys(&m, (const uint8_t[]){0x04, 0x50}, 2) == (HID_MEDIA_MUTE | HID_MEDIA_VOL_DOWN),
          "variable : Muet et Volume -");
    CHECK(hid_media_keys(&m, (const uint8_t[]){0x04, 0x8F}, 2) == 0, "variable : autres touches ignorées");
    // Clavier sans touche multimédia (rapport boot)
    static const uint8_t kbd[] = {0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7,
                                  0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0xC0};
    CHECK(!hid_media_parse(&m, kbd, sizeof(kbd)), "clavier ordinaire : aucune touche multimédia");

    // Volume
    hid_volume_t v;
    hid_volume_init(&v);
    CHECK(v.level == HID_VOLUME_MAX && hid_volume_apply(&v, 200) == 200, "volume au maximum : échantillon inchangé");
    CHECK(!hid_volume_keys(&v, HID_MEDIA_VOL_UP), "Volume + au maximum : sans effet");
    CHECK(hid_volume_keys(&v, HID_MEDIA_VOL_DOWN) && v.level == HID_VOLUME_MAX - 1 &&
          hid_volume_apply(&v, 200) == 141, "Volume - : un pas (200 -> %d)", hid_volume_apply(&v, 200));
    CHECK(hid_volume_keys(&v, HID_MEDIA_MUTE) && v.muted && hid_volume_apply(&v, 255) == 0, "Muet : silence");
    CHECK(hid_volume_keys(&v, HID_MEDIA_VOL_UP) && !v.muted && v.level == HID_VOLUME_MAX, "Volume + lève la coupure");
    hid_volume_keys(&v, HID_MEDIA_MUTE);
    CHECK(hid_volume_keys(&v, HID_MEDIA_MUTE) && !v.muted && v.level == HID_VOLUME_MAX, "Muet deux fois : rétabli");
    for (int i = 0; i < 20; i++) hid_volume_keys(&v, HID_MEDIA_VOL_DOWN);
    CHECK(v.level == 0 && hid_volume_apply(&v, 255) == 0 && !hid_volume_keys(&v, HID_MEDIA_VOL_DOWN),
          "Volume - jusqu'à 0 : silence, puis sans effet");
    uint16_t prev = 0;
    bool rising = true;
    for (int i = 1; i <= HID_VOLUME_MAX; i++) {
        hid_volume_keys(&v, HID_MEDIA_VOL_UP);
        rising &= hid_volume_gain(&v) > prev;
        prev = hid_volume_gain(&v);
    }
    CHECK(rising && prev == 256, "gain croissant à chaque pas, 256 au maximum");
}

int main(void) {
    test_hid_media();
    printf("%d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
