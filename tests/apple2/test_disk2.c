// test_disk2.c — Disk II (src/devices/disk2_fdc.h, disk2_fdd.h)
//
// Selecting drive 2 stays inside fdd[] (it used to overwrite the next structure), the
// write-protect sense is reported on bit 7, an image of the wrong size is refused.

#define CHIPS_IMPL
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "devices/disk2_fdd.h"
#include "devices/disk2_fdc.h"

static int tests_run, tests_failed;
#define CHECK(c)                                                         \
    do {                                                                 \
        tests_run++;                                                     \
        if (!(c)) {                                                      \
            tests_failed++;                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
        }                                                                \
    } while (0)

// Guard words around the controller: they must survive every access
typedef struct {
    uint32_t before[4];
    disk2_fdc_t fdc;
    uint32_t after[4];
} guarded_t;

static void test_drive_select(void) {
    static guarded_t g;
    memset(&g, 0xA5, sizeof(g));
    memset(&g.fdc, 0, sizeof(g.fdc));
    disk2_fdc_init(&g.fdc);

    static uint8_t image[DISK2_FDD_NIB_IMAGE_SIZE];
    CHECK(disk2_fdd_insert_disk_sized(&g.fdc.fdd[0], image, sizeof(image)));

    // $C0EA / $C0EB: drive 1 / drive 2; then the sequence a DOS 3.3 "CATALOG,D2" does
    disk2_fdc_read_byte(&g.fdc, 0xB);
    CHECK(g.fdc.selected_drive < DISK2_FDC_MAX_DRIVES);
    disk2_fdc_write_byte(&g.fdc, 0x9, 0x00);   // motor on
    disk2_fdc_read_byte(&g.fdc, 0xC);          // Q6L: read
    for (int i = 0; i < 100; i++) disk2_fdc_read_byte(&g.fdc, 0xC);
    disk2_fdc_read_byte(&g.fdc, 0xE);          // Q7L
    disk2_fdc_tick(&g.fdc);
    // Drive 2 is empty: reads give 0xFF, and drive 1 is untouched
    CHECK(!disk2_fdd_is_disk_inserted(&g.fdc.fdd[1]));
    CHECK(disk2_fdd_is_disk_inserted(&g.fdc.fdd[0]));
    disk2_fdc_read_byte(&g.fdc, 0xA);
    CHECK(g.fdc.selected_drive == 0);

    for (int i = 0; i < 4; i++) {
        CHECK(g.before[i] == 0xA5A5A5A5u);
        CHECK(g.after[i] == 0xA5A5A5A5u);
    }
    disk2_fdc_discard(&g.fdc);
}

static void test_write_protect_sense(void) {
    disk2_fdd_t fdd;
    memset(&fdd, 0, sizeof(fdd));
    disk2_fdd_init(&fdd);
    // Q6H (control_bits = 1) = sense write protect; the image is write protected
    fdd.control_bits = 1;
    CHECK((disk2_fdd_read_byte(&fdd) & 0x80) != 0);
    fdd.write_protected = false;
    CHECK((disk2_fdd_read_byte(&fdd) & 0x80) == 0);
    disk2_fdd_discard(&fdd);
}

static void test_image_size(void) {
    disk2_fdd_t fdd;
    memset(&fdd, 0, sizeof(fdd));
    disk2_fdd_init(&fdd);
    static uint8_t small[DISK2_FDD_DSK_IMAGE_SIZE];   // a raw .dsk is shorter than our NIB layout
    CHECK(!disk2_fdd_insert_disk_sized(&fdd, small, sizeof(small)));
    CHECK(!disk2_fdd_is_disk_inserted(&fdd));
    CHECK(!disk2_fdd_insert_disk_sized(&fdd, NULL, DISK2_FDD_NIB_IMAGE_SIZE));
    static uint8_t ok[DISK2_FDD_NIB_IMAGE_SIZE];
    CHECK(disk2_fdd_insert_disk_sized(&fdd, ok, sizeof(ok)));
    CHECK(disk2_fdd_is_disk_inserted(&fdd));
    disk2_fdd_discard(&fdd);
}

int main(void) {
    test_drive_select();
    test_write_protect_sense();
    test_image_size();
    printf("test_disk2: %d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
