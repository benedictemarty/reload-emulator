#pragma once

// prodos_hdd.h — ProDOS hard disk drive (block device behind prodos_hdc.h)
//
// Image file through neo_storage (devices/neo_storage.h, include before this one with
// NEO_STORAGE_IMPL in one file): the USB drive on the board (FatFs driver of the
// socle), a directory on PC (POSIX driver), or the network with a "net:" path (TNFS).
// An image that cannot be opened for writing is opened read-only (write-protected).
// Or an image in memory (internal flash), always write-protected.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PRODOS_HDD_BYTES_PER_BLOCK 512

// ProDOS hard disk error codes
#define PRODOS_HDD_ERR_OK    (0x00)
#define PRODOS_HDD_ERR_IO    (0x27)
#define PRODOS_HDD_ERR_NODEV (0x28)
#define PRODOS_HDD_ERR_WPROT (0x2B)

#define PRODOS_HDD_IMAGE_TYPE_INTERNAL (0)
#define PRODOS_HDD_IMAGE_TYPE_FILE     (1)

// ProDOS hard disk drive state
typedef struct {
    bool valid;
    neo_file_t file;
    uint8_t* po_image;
    uint8_t image_type;
    uint32_t image_blocks;
    bool image_loaded;
    bool write_protected;
} prodos_hdd_t;

// ProDOS hard disk drive interface

// Initialize a new hard disk drive
void prodos_hdd_init(prodos_hdd_t* sys);

// Discard the hard disk drive
void prodos_hdd_discard(prodos_hdd_t* sys);

// Reset the hard disk drive
void prodos_hdd_reset(prodos_hdd_t* sys);

// Insert a disk image file (neo_storage path: "NAME.hdv" on the USB drive, "net:/NAME.hdv")
bool prodos_hdd_insert_disk_file(prodos_hdd_t* sys, const char* path);

// Insert a disk image in memory (internal flash)
bool prodos_hdd_insert_disk_internal(prodos_hdd_t* sys, uint8_t* po_image, uint32_t po_image_size);

// Remove the disk
void prodos_hdd_remove_disk(prodos_hdd_t* sys);

// Return true if the disk is currently inserted
bool prodos_hdd_is_disk_inserted(prodos_hdd_t* sys);

// Return the number of blocks
uint32_t prodos_hdd_get_blocks(prodos_hdd_t* sys);

// Read a block from the hard disk drive
uint8_t prodos_hdd_read_block(prodos_hdd_t* sys, uint16_t buffer, uint32_t block, mem_t* mem);

// Write a block to the hard disk drive
uint8_t prodos_hdd_write_block(prodos_hdd_t* sys, uint16_t buffer, uint32_t block, mem_t* mem);

#ifdef __cplusplus
}  // extern "C"
#endif

/*-- IMPLEMENTATION ----------------------------------------------------------*/
#ifdef CHIPS_IMPL
#include <string.h>
#ifndef CHIPS_ASSERT
#include <assert.h>
#define CHIPS_ASSERT(c) assert(c)
#endif

void prodos_hdd_init(prodos_hdd_t* sys) {
    CHIPS_ASSERT(sys && !sys->valid);
    memset(sys, 0, sizeof(prodos_hdd_t));
    sys->valid = true;
    sys->image_type = PRODOS_HDD_IMAGE_TYPE_INTERNAL;
    sys->image_loaded = false;
}

void prodos_hdd_discard(prodos_hdd_t* sys) {
    CHIPS_ASSERT(sys && sys->valid);
    prodos_hdd_remove_disk(sys);
    sys->valid = false;
}

void prodos_hdd_reset(prodos_hdd_t* sys) { CHIPS_ASSERT(sys && sys->valid); }

bool prodos_hdd_insert_disk_file(prodos_hdd_t* sys, const char* path) {
    CHIPS_ASSERT(sys && sys->valid);
    prodos_hdd_remove_disk(sys);
    bool ro = false;
    if (!neo_file_open_path(&sys->file, path, NEO_READ | NEO_WRITE)) {
        if (!neo_file_open_path(&sys->file, path, NEO_READ)) {
            printf("ProDOS: cannot open %s\r\n", path);
            return false;
        }
        ro = true;   // Read-only file or volume
    }
    sys->image_type = PRODOS_HDD_IMAGE_TYPE_FILE;
    sys->image_blocks = sys->file.size / PRODOS_HDD_BYTES_PER_BLOCK;
    sys->image_loaded = true;
    sys->write_protected = ro;
    return true;
}

bool prodos_hdd_insert_disk_internal(prodos_hdd_t* sys, uint8_t* po_image, uint32_t po_image_size) {
    CHIPS_ASSERT(sys && sys->valid);
    prodos_hdd_remove_disk(sys);
    sys->po_image = po_image;
    sys->image_type = PRODOS_HDD_IMAGE_TYPE_INTERNAL;
    sys->image_blocks = po_image_size / PRODOS_HDD_BYTES_PER_BLOCK;
    sys->image_loaded = true;
    sys->write_protected = true;
    return true;
}

void prodos_hdd_remove_disk(prodos_hdd_t* sys) {
    CHIPS_ASSERT(sys && sys->valid);
    if (sys->image_type == PRODOS_HDD_IMAGE_TYPE_FILE) neo_file_close(&sys->file);
    sys->image_type = PRODOS_HDD_IMAGE_TYPE_INTERNAL;
    sys->image_loaded = false;
    sys->image_blocks = 0;
}

bool prodos_hdd_is_disk_inserted(prodos_hdd_t* sys) {
    CHIPS_ASSERT(sys && sys->valid);
    return sys->image_loaded;
}

uint32_t prodos_hdd_get_blocks(prodos_hdd_t* sys) {
    CHIPS_ASSERT(sys && sys->valid);
    return sys->image_blocks;
}

uint8_t prodos_hdd_read_block(prodos_hdd_t* sys, uint16_t buffer, uint32_t block, mem_t* mem) {
    CHIPS_ASSERT(sys && sys->valid);
    if (!sys->image_loaded) return PRODOS_HDD_ERR_NODEV;
    if (block >= sys->image_blocks) return PRODOS_HDD_ERR_IO;
#ifdef PRODOS_HDD_COUNT_READS
    PRODOS_HDD_COUNT_READS++;   // Bench: blocks read (APPLE_DIAG)
#endif
    if (sys->image_type == PRODOS_HDD_IMAGE_TYPE_FILE) {
        // Through a 512-byte buffer: the Apple RAM buffer may cross a memory page
        uint8_t buf[PRODOS_HDD_BYTES_PER_BLOCK];
        if (!neo_file_read(&sys->file, block * PRODOS_HDD_BYTES_PER_BLOCK, buf, PRODOS_HDD_BYTES_PER_BLOCK)) {
            printf("ProDOS: read error, block %u\r\n", (unsigned)block);
            return PRODOS_HDD_ERR_IO;
        }
        mem_write_range(mem, buffer, buf, PRODOS_HDD_BYTES_PER_BLOCK);
    } else {
        mem_write_range(mem, buffer, sys->po_image + block * PRODOS_HDD_BYTES_PER_BLOCK, PRODOS_HDD_BYTES_PER_BLOCK);
    }
    return PRODOS_HDD_ERR_OK;
}

uint8_t prodos_hdd_write_block(prodos_hdd_t* sys, uint16_t buffer, uint32_t block, mem_t* mem) {
    CHIPS_ASSERT(sys && sys->valid);
    if (!sys->image_loaded) return PRODOS_HDD_ERR_NODEV;
    if (block >= sys->image_blocks) return PRODOS_HDD_ERR_IO;
    if (sys->write_protected) return PRODOS_HDD_ERR_WPROT;
    uint8_t buf[PRODOS_HDD_BYTES_PER_BLOCK];
    for (int i = 0; i < PRODOS_HDD_BYTES_PER_BLOCK; i++) buf[i] = mem_rd(mem, (uint16_t)(buffer + i));
    if (!neo_file_write(&sys->file, block * PRODOS_HDD_BYTES_PER_BLOCK, buf, PRODOS_HDD_BYTES_PER_BLOCK)) {
        printf("ProDOS: write error, block %u\r\n", (unsigned)block);
        return PRODOS_HDD_ERR_IO;
    }
    return PRODOS_HDD_ERR_OK;
}

#endif  // CHIPS_IMPL
