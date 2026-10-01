// test_neo_storage.c — volumes de fichiers communs (src/devices/neo_storage.h)
//
// Pilote en mémoire et pilote POSIX (platforms/pc/neo_storage_posix.h) dans un
// dossier temporaire ; rappels en flux branchés sur la cassette de l'Oric.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NEO_STORAGE_IMPL
#include "devices/neo_storage.h"
#include "../../platforms/pc/neo_storage_posix.h"
#include "devices/oric_tape.h"

static int tests_run, tests_failed;
#define CHECK(c)                                                         \
    do {                                                                 \
        tests_run++;                                                     \
        if (!(c)) {                                                      \
            tests_failed++;                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
        }                                                                \
    } while (0)

static int listed;
static char listed_names[8][NEO_STORAGE_NAME_MAX];
static bool count_cb(const neo_dirent_t* e, void* user) {
    (void)user;
    if (listed < 8) snprintf(listed_names[listed], NEO_STORAGE_NAME_MAX, "%s", e->name);
    listed++;
    return true;
}
static bool stop_cb(const neo_dirent_t* e, void* user) {
    (void)e;
    (*(int*)user)++;
    return false;
}

static void test_split(void) {
    int vol = -1;
    CHECK(!strcmp(neo_storage_split("net:/SED.DSK", &vol), "/SED.DSK") && vol == NEO_VOL_NET);
    CHECK(!strcmp(neo_storage_split("SED.DSK", &vol), "SED.DSK") && vol == NEO_VOL_USB);
    CHECK(!strcmp(neo_storage_split("netx.dsk", &vol), "netx.dsk") && vol == NEO_VOL_USB);
}

static void test_volumes(void) {
    CHECK(!neo_storage_volume(0) && !neo_storage_ready(0));
    CHECK(!neo_storage_volume(-1) && !neo_storage_volume(NEO_STORAGE_VOLUMES));
    neo_file_t f;
    CHECK(!neo_file_open(&f, 0, "X", NEO_READ) && !neo_file_is_open(&f));
    CHECK(neo_file_close(&f));   // Closing a closed file: no effect
}

static void test_mem(void) {
    static uint8_t rom[4] = {1, 2, 3, 4}, ram[16];
    neo_mem_file_t files[] = {{"ROM.BIN", rom, 4, 0}, {"SAVE.TAP", ram, 0, sizeof(ram)}};
    neo_mem_volume_t mv = {files, 2};
    neo_storage_set(1, "Mémoire", &neo_storage_mem_ops, &mv);
    CHECK(neo_storage_ready(1) && !strcmp(neo_storage_volume(1)->label, "Mémoire"));
    neo_file_t f;
    uint8_t b[4];
    CHECK(neo_file_open(&f, 1, "/ROM.BIN", NEO_READ) && f.size == 4);
    CHECK(neo_file_read(&f, 1, b, 3) && b[0] == 2 && b[2] == 4);
    CHECK(!neo_file_read(&f, 2, b, 3));        // Past the end: refused, nothing read beyond
    CHECK(!neo_file_write(&f, 0, b, 1));       // Opened for reading
    CHECK(neo_file_close(&f));
    CHECK(!neo_file_open(&f, 1, "ROM.BIN", NEO_WRITE));   // Read-only file
    CHECK(!neo_file_open(&f, 1, "ABSENT", NEO_READ));
    // Sequential writing (CSAVE) then reading back
    CHECK(neo_file_open(&f, 1, "SAVE.TAP", NEO_WRITE | NEO_CREATE));
    CHECK(neo_file_append(&f, (const uint8_t*)"ab", 2) && neo_file_append(&f, (const uint8_t*)"cd", 2));
    CHECK(f.size == 4 && f.pos == 4);
    CHECK(!neo_file_append(&f, ram, 13));      // Beyond the capacity
    neo_file_close(&f);
    uint32_t n;
    uint8_t back[16];
    CHECK(neo_file_load(1, "SAVE.TAP", back, sizeof(back), &n, false) && n == 4 && !memcmp(back, "abcd", 4));
    CHECK(!neo_file_load(1, "SAVE.TAP", back, 8, &n, true) && n == 0);   // Exact size required
    CHECK(neo_file_load(1, "ROM.BIN", back, 4, &n, true) && n == 4);
    listed = 0;
    CHECK(neo_storage_list(1, "/", count_cb, 0) && listed == 2 && !strcmp(listed_names[1], "SAVE.TAP"));
    int seen = 0;
    CHECK(neo_storage_list(1, "/", stop_cb, &seen) && seen == 1);   // The callback stops the listing
    CHECK(!neo_storage_list(1, "/SUB", count_cb, 0));
    CHECK(!neo_file_remove(1, "SAVE.TAP"));   // The memory volume cannot remove
    neo_storage_set(1, 0, 0, 0);
    CHECK(!neo_storage_volume(1));
}

static void test_posix(void) {
    char dir[] = "/tmp/neo_storage_XXXXXX";
    CHECK(mkdtemp(dir) != 0);
    neo_storage_set(NEO_VOL_USB, "Clé USB", &neo_storage_posix_ops, dir);
    // Save, load, rewrite in place (as the WD1793 does on a streamed image)
    uint8_t img[1000];
    for (int i = 0; i < 1000; i++) img[i] = (uint8_t)(i * 7);
    CHECK(neo_file_save(NEO_VOL_USB, "/TEST.DSK", img, sizeof(img)));
    neo_file_t f;
    CHECK(neo_file_open(&f, NEO_VOL_USB, "TEST.DSK", NEO_READ | NEO_WRITE) && f.size == 1000);
    const uint8_t patch[3] = {0xAA, 0xBB, 0xCC};
    CHECK(neo_file_write_cb(&f, 500, patch, 3));
    uint8_t b[5];
    CHECK(neo_file_read_cb(&f, 499, b, 5) && b[0] == img[499] && b[1] == 0xAA && b[3] == 0xCC && b[4] == img[503]);
    CHECK(!neo_file_read_cb(&f, 998, b, 5));
    CHECK(neo_file_close(&f));
    CHECK(neo_file_save(NEO_VOL_USB, "B.TAP", (const uint8_t*)"x", 1));
    listed = 0;
    CHECK(neo_storage_list(NEO_VOL_USB, "/", count_cb, 0) && listed == 2);
    CHECK(!neo_file_open(&f, NEO_VOL_USB, "NONE.TAP", NEO_READ));
    // Removal
    CHECK(neo_file_remove(NEO_VOL_USB, "/B.TAP") && !neo_file_open(&f, NEO_VOL_USB, "B.TAP", NEO_READ));
    CHECK(!neo_file_remove(NEO_VOL_USB, "B.TAP"));   // Already gone
    CHECK(neo_file_save(NEO_VOL_USB, "B.TAP", (const uint8_t*)"x", 1));
    // Streamed tape: a .tap read by the Oric tape reader through neo_file_read_cb
    static const uint8_t tap[] = {0x16, 0x16, 0x16, 0x24, 0, 0, 0x80, 0, 0x05, 0x01, 0x05, 0x00, 0, 'A', 0, 0x42, 0x43};
    CHECK(neo_file_save(NEO_VOL_USB, "T.TAP", tap, sizeof(tap)));
    CHECK(neo_file_open(&f, NEO_VOL_USB, "T.TAP", NEO_READ));
    static oric_tape_t t;
    memset(&t, 0, sizeof(t));
    oric_tape_insert(&t, f.size, neo_file_read_cb, &f);
    CHECK(t.inserted && t.len == sizeof(tap));
    neo_file_close(&f);
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    CHECK(system(cmd) == 0);
}

int main(void) {
    test_split();
    test_volumes();
    test_mem();
    test_posix();
    printf("%d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
