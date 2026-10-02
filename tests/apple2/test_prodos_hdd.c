// test_prodos_hdd.c — ProDOS hard disk on neo_storage (src/devices/prodos_hdd.h)
//
// POSIX driver of the socle on a temporary directory: read and write of blocks into the
// Apple memory (buffer across memory pages, as with the IIe's 512-byte pages), read-only
// image, out-of-range block, missing image, removal.

#define CHIPS_IMPL
#define MEM_PAGE_SHIFT (9U)   // As the Apple IIe
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "chips/chips_common.h"
#include "chips/mem.h"
#define NEO_STORAGE_IMPL
#include "devices/neo_storage.h"
#include "../../platforms/pc/neo_storage_posix.h"
#include "devices/prodos_hdd.h"

static int tests_run, tests_failed;
#define CHECK(c)                                                         \
    do {                                                                 \
        tests_run++;                                                     \
        if (!(c)) {                                                      \
            tests_failed++;                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
        }                                                                \
    } while (0)

#define BLOCKS 4
static char dir[64], path[96];
static uint8_t ram[0x10000];
static mem_t mem;
static prodos_hdd_t hdd;

static uint8_t pattern(uint32_t block, int i) { return (uint8_t)(block * 37 + i * 7); }

static void make_image(void) {
    FILE* f = fopen(path, "wb");
    for (uint32_t b = 0; b < BLOCKS; b++)
        for (int i = 0; i < PRODOS_HDD_BYTES_PER_BLOCK; i++) fputc(pattern(b, i), f);
    fclose(f);
}

static void test_read_write(void) {
    make_image();
    memset(&hdd, 0, sizeof(hdd));
    prodos_hdd_init(&hdd);
    CHECK(prodos_hdd_insert_disk_file(&hdd, "disk.hdv"));
    CHECK(prodos_hdd_is_disk_inserted(&hdd) && prodos_hdd_get_blocks(&hdd) == BLOCKS && !hdd.write_protected);
    // Block 2 into $11F0-$13EF: three 512-byte memory pages
    CHECK(prodos_hdd_read_block(&hdd, 0x11F0, 2, &mem) == PRODOS_HDD_ERR_OK);
    bool same = true;
    for (int i = 0; i < PRODOS_HDD_BYTES_PER_BLOCK; i++) same &= ram[0x11F0 + i] == pattern(2, i);
    CHECK(same);
    CHECK(prodos_hdd_read_block(&hdd, 0x2000, BLOCKS, &mem) == PRODOS_HDD_ERR_IO);   // Out of range
    // Block 1 written from $21F0 (pages again), then read back from the file
    for (int i = 0; i < PRODOS_HDD_BYTES_PER_BLOCK; i++) ram[0x21F0 + i] = (uint8_t)(0xA5 ^ i);
    CHECK(prodos_hdd_write_block(&hdd, 0x21F0, 1, &mem) == PRODOS_HDD_ERR_OK);
    prodos_hdd_remove_disk(&hdd);
    CHECK(!prodos_hdd_is_disk_inserted(&hdd));
    CHECK(prodos_hdd_read_block(&hdd, 0x2000, 0, &mem) == PRODOS_HDD_ERR_NODEV);
    FILE* f = fopen(path, "rb");
    uint8_t buf[PRODOS_HDD_BYTES_PER_BLOCK * BLOCKS];
    CHECK(f && fread(buf, 1, sizeof(buf), f) == sizeof(buf));
    if (f) fclose(f);
    same = true;
    for (int i = 0; i < PRODOS_HDD_BYTES_PER_BLOCK; i++) {
        same &= buf[PRODOS_HDD_BYTES_PER_BLOCK + i] == (uint8_t)(0xA5 ^ i);   // Block 1 written
        same &= buf[2 * PRODOS_HDD_BYTES_PER_BLOCK + i] == pattern(2, i);     // Block 2 untouched
    }
    CHECK(same);
    prodos_hdd_discard(&hdd);
}

static void test_read_only_and_missing(void) {
    make_image();
    chmod(path, 0444);
    memset(&hdd, 0, sizeof(hdd));
    prodos_hdd_init(&hdd);
    CHECK(prodos_hdd_insert_disk_file(&hdd, "disk.hdv"));
    CHECK(hdd.write_protected);   // Opened read-only
    CHECK(prodos_hdd_read_block(&hdd, 0x3000, 0, &mem) == PRODOS_HDD_ERR_OK && ram[0x3001] == pattern(0, 1));
    CHECK(prodos_hdd_write_block(&hdd, 0x3000, 0, &mem) == PRODOS_HDD_ERR_WPROT);
    CHECK(!prodos_hdd_insert_disk_file(&hdd, "absent.hdv"));
    CHECK(!prodos_hdd_is_disk_inserted(&hdd));
    prodos_hdd_discard(&hdd);
    chmod(path, 0644);
}

int main(void) {
    snprintf(dir, sizeof(dir), "/tmp/prodos_hdd_XXXXXX");
    if (!mkdtemp(dir)) return 1;
    snprintf(path, sizeof(path), "%s/disk.hdv", dir);
    neo_storage_set(NEO_VOL_USB, "Fichiers", &neo_storage_posix_ops, dir);
    mem_init(&mem);
    mem_map_ram(&mem, 0, 0x0000, 0x10000, ram);
    test_read_write();
    if (getuid() != 0) test_read_only_and_missing();   // root ignores the read-only mode
    remove(path);
    rmdir(dir);
    printf("%d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
