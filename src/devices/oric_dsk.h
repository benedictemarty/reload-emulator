#pragma once

// oric_dsk.h
//
// Oric "MFM_DISK" disk images (.dsk) as used by the Microdisc and Jasmin
// floppy disk interfaces (Sedoric, OricDOS, FT-DOS...), accessed track by track.
//
// Layout (checked on real images and in Oricutron's disk.c):
//   - 256 byte header: "MFM_DISK", sides (uint32 LE), tracks per side
//     (uint32 LE), geometry (uint32 LE, only 1 is supported), zero padding;
//   - then the raw decoded MFM tracks, 6400 bytes each, side 0 tracks first,
//     then side 1 tracks (geometry 1).
// A track holds gaps, sync bytes and sectors: ID field A1 A1 A1 FE, track,
// side, sector, size code (128 << n bytes), CRC (2 bytes); data field
// A1 A1 A1 FB (or F8 = deleted data), data, CRC (2 bytes).
// Some images are shorter than their header says (second side missing):
// tracks beyond the end of the file read as unformatted.
//
// The image is either in memory (whole file) or streamed through read/write
// callbacks (e.g. a file on a USB drive): only one track (6400 bytes) is
// then kept in memory, written back when another track is loaded or on flush.
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
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ORIC_DSK_HEADER_SIZE 256
#define ORIC_DSK_TRACK_SIZE  6400
#define ORIC_DSK_MAX_SECTORS 32
#define ORIC_DSK_MAX_TRACKS  128

// Streamed access: read/write `len` bytes at byte offset `offset` of the image file
typedef bool (*oric_dsk_read_t)(void* ctx, uint32_t offset, uint8_t* buf, uint32_t len);
typedef bool (*oric_dsk_write_t)(void* ctx, uint32_t offset, const uint8_t* buf, uint32_t len);

typedef struct {
    uint8_t* data;            // Whole image in memory (writable), or 0 when streamed
    uint32_t size;            // Image size in bytes
    oric_dsk_read_t read;     // Streamed access (data == 0)
    oric_dsk_write_t write;   // Optional: streamed images without it are write-protected
    void* ctx;
    int sides;
    int tracks;
    bool write_protected;
    bool modified;            // Written since insertion
} oric_dsk_t;

// A sector found on the current track: byte positions inside the track
typedef struct {
    uint16_t id;              // Position of the FE mark (track, side, sector, size, CRC follow)
    int16_t data;             // Position of the FB/F8 mark, or -1 (no data field)
} oric_dsk_sector_t;

// Track cache (one per controller: one drive is active at a time)
typedef struct {
    oric_dsk_t* disk;         // Disk the cached track belongs to (0 = none)
    int track;
    int side;
    bool dirty;               // Streamed track modified, to be written back
    uint8_t* raw;             // Track bytes (inside disk->data, or buf)
    int num_sectors;
    oric_dsk_sector_t sectors[ORIC_DSK_MAX_SECTORS];
    uint8_t buf[ORIC_DSK_TRACK_SIZE];
} oric_dsk_track_t;

// Check a header; returns false if it is not a supported MFM_DISK image
bool oric_dsk_parse_header(const uint8_t* header, uint32_t size, int* sides, int* tracks);
// Open an in-memory image (the buffer must stay valid and writable)
bool oric_dsk_open_mem(oric_dsk_t* dsk, uint8_t* data, uint32_t size, bool write_protected);
// Open a streamed image; `header` holds the first 256 bytes of the file
bool oric_dsk_open_streamed(oric_dsk_t* dsk, const uint8_t* header, uint32_t size, oric_dsk_read_t read,
                            oric_dsk_write_t write, void* ctx);
// Byte offset of a track in the image file
uint32_t oric_dsk_track_offset(const oric_dsk_t* dsk, int track, int side);

// Make `track`/`side` of `dsk` the cached track (writes the previous one back if needed).
// Returns false if the track does not exist (no sectors then).
bool oric_dsk_load_track(oric_dsk_track_t* t, oric_dsk_t* dsk, int track, int side);
// Rescan the sectors of the cached track (after a write track)
void oric_dsk_scan_track(oric_dsk_track_t* t);
// Mark the cached track modified
void oric_dsk_track_modified(oric_dsk_track_t* t);
// Write back the cached track if it was modified (streamed images)
bool oric_dsk_flush(oric_dsk_track_t* t);
// Forget the cached track (after flushing it) if it belongs to `dsk` (or any disk if dsk == 0)
void oric_dsk_invalidate(oric_dsk_track_t* t, oric_dsk_t* dsk);

// CRC-16/CCITT as computed by the WD179x (x^16 + x^12 + x^5 + 1)
uint16_t oric_dsk_crc(uint16_t crc, uint8_t value);

#ifdef __cplusplus
}  // extern "C"
#endif

/*-- IMPLEMENTATION ----------------------------------------------------------*/
#ifdef CHIPS_IMPL
#include <string.h>

static uint32_t _oric_dsk_le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool oric_dsk_parse_header(const uint8_t* header, uint32_t size, int* sides, int* tracks) {
    if (size < ORIC_DSK_HEADER_SIZE || memcmp(header, "MFM_DISK", 8) != 0) {
        return false;
    }
    uint32_t s = _oric_dsk_le32(header + 8);
    uint32_t t = _oric_dsk_le32(header + 12);
    uint32_t g = _oric_dsk_le32(header + 16);
    if (s < 1 || s > 2 || t < 1 || t > ORIC_DSK_MAX_TRACKS || g != 1) {
        return false;
    }
    *sides = (int)s;
    *tracks = (int)t;
    return true;
}

bool oric_dsk_open_mem(oric_dsk_t* dsk, uint8_t* data, uint32_t size, bool write_protected) {
    memset(dsk, 0, sizeof(*dsk));
    if (!data || !oric_dsk_parse_header(data, size, &dsk->sides, &dsk->tracks)) {
        return false;
    }
    dsk->data = data;
    dsk->size = size;
    dsk->write_protected = write_protected;
    return true;
}

bool oric_dsk_open_streamed(oric_dsk_t* dsk, const uint8_t* header, uint32_t size, oric_dsk_read_t read,
                            oric_dsk_write_t write, void* ctx) {
    memset(dsk, 0, sizeof(*dsk));
    if (!read || !oric_dsk_parse_header(header, size, &dsk->sides, &dsk->tracks)) {
        return false;
    }
    dsk->size = size;
    dsk->read = read;
    dsk->write = write;
    dsk->ctx = ctx;
    dsk->write_protected = (write == 0);
    return true;
}

uint32_t oric_dsk_track_offset(const oric_dsk_t* dsk, int track, int side) {
    return ORIC_DSK_HEADER_SIZE + (uint32_t)(side * dsk->tracks + track) * ORIC_DSK_TRACK_SIZE;
}

uint16_t oric_dsk_crc(uint16_t crc, uint8_t value) {
    crc ^= (uint16_t)value << 8;
    for (int i = 0; i < 8; i++) {
        crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

void oric_dsk_scan_track(oric_dsk_track_t* t) {
    t->num_sectors = 0;
    if (!t->raw) {
        return;
    }
    const uint8_t* p = t->raw;
    int i = 0;
    while (t->num_sectors < ORIC_DSK_MAX_SECTORS) {
        // ID address mark
        while (i + 3 < ORIC_DSK_TRACK_SIZE && !(p[i] == 0xA1 && p[i + 1] == 0xA1 && p[i + 2] == 0xA1 && p[i + 3] == 0xFE)) {
            i++;
        }
        if (i + 3 >= ORIC_DSK_TRACK_SIZE) {
            break;
        }
        i += 3;
        if (i + 7 > ORIC_DSK_TRACK_SIZE) {
            break;
        }
        oric_dsk_sector_t* s = &t->sectors[t->num_sectors++];
        s->id = (uint16_t)i;
        s->data = -1;
        uint32_t len = 128u << (p[i + 4] & 3);
        i += 7;
        // Data address mark (before the next ID mark)
        int j = i;
        while (j < ORIC_DSK_TRACK_SIZE && p[j] != 0xFB && p[j] != 0xF8) {
            if (j + 3 < ORIC_DSK_TRACK_SIZE && p[j] == 0xA1 && p[j + 1] == 0xA1 && p[j + 2] == 0xA1 && p[j + 3] == 0xFE) {
                break;
            }
            j++;
        }
        if (j >= ORIC_DSK_TRACK_SIZE || (p[j] != 0xFB && p[j] != 0xF8)) {
            i = j;
            continue;
        }
        if (j + 1 + (int)len + 2 <= ORIC_DSK_TRACK_SIZE) {
            s->data = (int16_t)j;
        }
        i = j + 1 + (int)len;
    }
}

bool oric_dsk_flush(oric_dsk_track_t* t) {
    bool ok = true;
    if (t->dirty && t->disk && !t->disk->data && t->disk->write) {
        ok = t->disk->write(t->disk->ctx, oric_dsk_track_offset(t->disk, t->track, t->side), t->buf, ORIC_DSK_TRACK_SIZE);
    }
    t->dirty = false;
    return ok;
}

void oric_dsk_invalidate(oric_dsk_track_t* t, oric_dsk_t* dsk) {
    if (dsk && t->disk != dsk) {
        return;
    }
    oric_dsk_flush(t);
    t->disk = 0;
    t->raw = 0;
    t->num_sectors = 0;
}

bool oric_dsk_load_track(oric_dsk_track_t* t, oric_dsk_t* dsk, int track, int side) {
    if (t->disk == dsk && dsk && t->track == track && t->side == side) {
        return t->raw != 0;
    }
    oric_dsk_invalidate(t, 0);
    t->disk = dsk;
    t->track = track;
    t->side = side;
    if (!dsk || track < 0 || track >= dsk->tracks || side < 0 || side >= dsk->sides) {
        return false;
    }
    uint32_t off = oric_dsk_track_offset(dsk, track, side);
    if (off + ORIC_DSK_TRACK_SIZE > dsk->size) {
        return false;   // Truncated image
    }
    if (dsk->data) {
        t->raw = dsk->data + off;
    } else {
        if (!dsk->read(dsk->ctx, off, t->buf, ORIC_DSK_TRACK_SIZE)) {
            return false;
        }
        t->raw = t->buf;
    }
    oric_dsk_scan_track(t);
    return true;
}

void oric_dsk_track_modified(oric_dsk_track_t* t) {
    if (t->disk) {
        t->disk->modified = true;
        if (!t->disk->data) {
            t->dirty = true;
        }
    }
}

#endif  // CHIPS_IMPL
