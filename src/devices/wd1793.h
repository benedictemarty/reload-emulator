#pragma once

// wd1793.h
//
// Western Digital WD1793 floppy disk controller working on Oric MFM_DISK
// images (devices/oric_dsk.h), as fitted to the Oric Microdisc and Jasmin
// interfaces. Up to 4 drives; the interface selects the drive and the side.
//
// Written from the WD179x data sheet (command types I-IV, status bits,
// write track control bytes F5/F6/F7); the behaviours left open by the data
// sheet follow Oricutron (the reference Oric emulator, disk.c), which runs
// Sedoric and FT-DOS:
//   - data transfers are paced by the CPU: the next DRQ comes 32 us after
//     the CPU has read/written the data register (a byte takes 32 us in MFM
//     at 250 kbit/s), so a slow CPU never loses data;
//   - type I commands complete after 20 us (no real head movement time) and
//     set "head loaded" and "index" in the status;
//   - a multiple sector command stops without error after the last sector
//     of the track;
//   - force interrupt always raises INTRQ;
//   - a type I command with no disk returns "not ready" + "seek error";
//   - "not ready" is set while a type II or III command is busy (also in
//     Phosphoric; FT-DOS relies on it).
// Not implemented: CRC checking of the image data, index pulse timing,
// motor/settle delays.
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

#include "devices/oric_dsk.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WD1793_MAX_DRIVES   4
#define WD1793_MAX_HEAD_POS 84   // Physical head stop

// Status register bits
#define WD1793_ST_BUSY       (1 << 0)
#define WD1793_ST_INDEX_DRQ  (1 << 1)   // Type I: index pulse; types II/III: DRQ
#define WD1793_ST_TRACK0_LOST (1 << 2)  // Type I: track 0; types II/III: lost data
#define WD1793_ST_CRC        (1 << 3)
#define WD1793_ST_SEEK_RNF   (1 << 4)   // Type I: seek error; types II/III: record not found
#define WD1793_ST_HEAD_REC   (1 << 5)   // Type I: head loaded; read: record type (deleted data)
#define WD1793_ST_WPROT      (1 << 6)
#define WD1793_ST_NOT_READY  (1 << 7)

// Delays in microseconds (see the notes above)
#define WD1793_TYPE1_US      20
#define WD1793_FIRST_DRQ_US  60
#define WD1793_BYTE_US       32
#define WD1793_NEXT_SECTOR_US 180

typedef enum {
    WD1793_IDLE,
    WD1793_WAIT,            // Wait `wait_us`, then go to `next`
    WD1793_CPU,             // DRQ raised, waiting for the CPU to access the data register
    WD1793_READ_BYTE,       // Present the next byte (read sector / read address / read track)
    WD1793_WRITE_DRQ,       // Ask for the next byte (write sector / write track)
    WD1793_SECTOR_DONE,     // End of a sector: next one if multiple
    WD1793_END,             // Command complete: INTRQ
} wd1793_state_t;

typedef enum {
    WD1793_OP_NONE,
    WD1793_OP_READ_SECTOR,
    WD1793_OP_WRITE_SECTOR,
    WD1793_OP_READ_ADDRESS,
    WD1793_OP_READ_TRACK,
    WD1793_OP_WRITE_TRACK,
} wd1793_op_t;

typedef struct {
    bool valid;
    // Registers
    uint8_t status;
    uint8_t track;
    uint8_t sector;
    uint8_t data;
    uint8_t command;
    // Pins
    bool drq;
    bool intrq;
    // Selection (set by the interface)
    int drive;
    int side;
    // Drives
    uint8_t head[WD1793_MAX_DRIVES];          // Physical head position
    bool present[WD1793_MAX_DRIVES];
    oric_dsk_t disk[WD1793_MAX_DRIVES];
    bool step_in;                             // Direction of the last step
    // Command execution
    wd1793_state_t state;
    wd1793_state_t next;
    wd1793_op_t op;
    uint32_t wait_us;
    uint8_t end_status;                       // Status bits set at the end of the command
    int sec_index;                            // Rotational position: index of the last sector passed
    int pos;                                  // Byte position in the track of the transfer
    int count;                                // Bytes left in the transfer
    uint16_t crc;
    bool crc_preset;                          // Write track: inside a run of F5 bytes
    // Track cache (shared by the drives)
    oric_dsk_track_t cache;
} wd1793_t;

void wd1793_init(wd1793_t* fdc);
// Master reset (MR pin): aborts the command, clears DRQ/INTRQ
void wd1793_reset(wd1793_t* fdc);
// Insert an in-memory MFM_DISK image (buffer kept, written in place); false if not a valid image
bool wd1793_insert_mem(wd1793_t* fdc, int drive, uint8_t* data, uint32_t size, bool write_protected);
// Insert a streamed image; `header` = first 256 bytes of the file
bool wd1793_insert_streamed(wd1793_t* fdc, int drive, const uint8_t* header, uint32_t size, oric_dsk_read_t read,
                            oric_dsk_write_t write, void* ctx);
void wd1793_eject(wd1793_t* fdc, int drive);
bool wd1793_disk_present(const wd1793_t* fdc, int drive);
// Write back the cached track of a streamed image
bool wd1793_flush(wd1793_t* fdc);
// Drive (0-3) and side (0-1) selected by the interface
void wd1793_select(wd1793_t* fdc, int drive, int side);
// Registers (reg & 3): 0 status/command, 1 track, 2 sector, 3 data
uint8_t wd1793_read(wd1793_t* fdc, uint8_t reg);
void wd1793_write(wd1793_t* fdc, uint8_t reg, uint8_t value);
// Advance one microsecond
void wd1793_tick(wd1793_t* fdc);

#ifdef __cplusplus
}
#endif

/*-- IMPLEMENTATION ----------------------------------------------------------*/
#ifdef CHIPS_IMPL
#ifndef CHIPS_HOT
#define CHIPS_HOT
#endif
#include <string.h>

void wd1793_init(wd1793_t* fdc) {
    memset(fdc, 0, sizeof(*fdc));
    fdc->valid = true;
    wd1793_reset(fdc);
}

void wd1793_reset(wd1793_t* fdc) {
    fdc->status = 0;
    fdc->track = 0;
    fdc->sector = 1;
    fdc->data = 0;
    fdc->command = 0;
    fdc->drq = false;
    fdc->intrq = false;
    fdc->state = WD1793_IDLE;
    fdc->op = WD1793_OP_NONE;
    fdc->wait_us = 0;
}

bool wd1793_insert_mem(wd1793_t* fdc, int drive, uint8_t* data, uint32_t size, bool write_protected) {
    if (drive < 0 || drive >= WD1793_MAX_DRIVES) return false;
    wd1793_eject(fdc, drive);
    fdc->present[drive] = oric_dsk_open_mem(&fdc->disk[drive], data, size, write_protected);
    return fdc->present[drive];
}

bool wd1793_insert_streamed(wd1793_t* fdc, int drive, const uint8_t* header, uint32_t size, oric_dsk_read_t read,
                            oric_dsk_write_t write, void* ctx) {
    if (drive < 0 || drive >= WD1793_MAX_DRIVES) return false;
    wd1793_eject(fdc, drive);
    fdc->present[drive] = oric_dsk_open_streamed(&fdc->disk[drive], header, size, read, write, ctx);
    return fdc->present[drive];
}

void wd1793_eject(wd1793_t* fdc, int drive) {
    if (drive < 0 || drive >= WD1793_MAX_DRIVES) return;
    oric_dsk_invalidate(&fdc->cache, &fdc->disk[drive]);
    memset(&fdc->disk[drive], 0, sizeof(oric_dsk_t));
    fdc->present[drive] = false;
}

bool wd1793_disk_present(const wd1793_t* fdc, int drive) {
    return drive >= 0 && drive < WD1793_MAX_DRIVES && fdc->present[drive];
}

bool wd1793_flush(wd1793_t* fdc) { return oric_dsk_flush(&fdc->cache); }

void wd1793_select(wd1793_t* fdc, int drive, int side) {
    fdc->drive = drive & (WD1793_MAX_DRIVES - 1);
    fdc->side = side & 1;
}

static void _wd1793_set_drq(wd1793_t* fdc, bool on) {
    fdc->drq = on;
    if (on) {
        fdc->status |= WD1793_ST_INDEX_DRQ;
    } else {
        fdc->status &= (uint8_t)~WD1793_ST_INDEX_DRQ;
    }
}

static void _wd1793_wait(wd1793_t* fdc, uint32_t us, wd1793_state_t next) {
    fdc->wait_us = us;
    fdc->next = next;
    fdc->state = WD1793_WAIT;
}

// Load the track under the head of the selected drive; false if unformatted / no disk
static bool _wd1793_track(wd1793_t* fdc) {
    if (!fdc->present[fdc->drive]) return false;
    return oric_dsk_load_track(&fdc->cache, &fdc->disk[fdc->drive], fdc->head[fdc->drive], fdc->side);
}

// Search the sector matching the track and sector registers over two revolutions
static int _wd1793_find_sector(wd1793_t* fdc) {
    if (!_wd1793_track(fdc)) return -1;
    oric_dsk_track_t* t = &fdc->cache;
    int n = t->num_sectors;
    for (int k = 0; k < 2 * n; k++) {
        fdc->sec_index = (fdc->sec_index + 1) % n;
        const uint8_t* id = t->raw + t->sectors[fdc->sec_index].id;
        if (id[1] != fdc->track || id[3] != fdc->sector) continue;
        if ((fdc->command & 0x02) && id[2] != ((fdc->command >> 3) & 1)) continue;   // Side compare (C flag)
        if (t->sectors[fdc->sec_index].data < 0) continue;
        return fdc->sec_index;
    }
    return -1;
}

static void _wd1793_start_sector(wd1793_t* fdc, int s) {
    oric_dsk_track_t* t = &fdc->cache;
    const uint8_t* id = t->raw + t->sectors[s].id;
    fdc->pos = t->sectors[s].data;     // Data mark
    fdc->count = 128 << (id[4] & 3);
    if (fdc->op == WD1793_OP_READ_SECTOR) {
        if (t->raw[fdc->pos] == 0xF8) {
            fdc->end_status |= WD1793_ST_HEAD_REC;   // Deleted data mark
        }
        fdc->pos++;
    } else {
        // Write sector: data mark, then the data (CRC preset over A1 A1 A1 + mark)
        uint8_t mark = (fdc->command & 0x01) ? 0xF8 : 0xFB;
        t->raw[fdc->pos++] = mark;
        fdc->crc = 0xFFFF;
        for (int i = 0; i < 3; i++) fdc->crc = oric_dsk_crc(fdc->crc, 0xA1);
        fdc->crc = oric_dsk_crc(fdc->crc, mark);
        oric_dsk_track_modified(t);
    }
}

// Status bits of a type I command at its end
static uint8_t _wd1793_type1_status(wd1793_t* fdc, bool seek_error) {
    uint8_t st = 0;
    if (!fdc->present[fdc->drive]) {
        return WD1793_ST_NOT_READY | WD1793_ST_SEEK_RNF;
    }
    st |= WD1793_ST_HEAD_REC | WD1793_ST_INDEX_DRQ;
    if (fdc->head[fdc->drive] == 0) st |= WD1793_ST_TRACK0_LOST;
    if (fdc->disk[fdc->drive].write_protected) st |= WD1793_ST_WPROT;
    if (seek_error) st |= WD1793_ST_SEEK_RNF;
    return st;
}

static void _wd1793_type1(wd1793_t* fdc, uint8_t cmd) {
    uint8_t* head = &fdc->head[fdc->drive];
    switch (cmd & 0xE0) {
        case 0x00:
            if (cmd & 0x10) {
                // Seek: track register -> data register
                int delta = (int)fdc->data - (int)fdc->track;
                int h = (int)*head + delta;
                if (delta != 0) fdc->step_in = delta > 0;
                *head = (uint8_t)(h < 0 ? 0 : (h > WD1793_MAX_HEAD_POS ? WD1793_MAX_HEAD_POS : h));
                fdc->track = fdc->data;
            } else {
                // Restore
                *head = 0;
                fdc->track = 0;
                fdc->step_in = false;
            }
            break;
        default: {
            // Step (2x/3x), step in (4x/5x), step out (6x/7x); u = bit 4 updates the track register
            bool in = ((cmd & 0xE0) == 0x40) ? true : (((cmd & 0xE0) == 0x60) ? false : fdc->step_in);
            fdc->step_in = in;
            if (in) {
                if (*head < WD1793_MAX_HEAD_POS) (*head)++;
                if (cmd & 0x10) fdc->track++;
            } else {
                if (*head > 0) (*head)--;
                if (cmd & 0x10) fdc->track--;
            }
            break;
        }
    }
    bool seek_error = false;
    if (cmd & 0x04) {
        // Verify: an ID field of the track register's track must be under the head
        seek_error = true;
        if (_wd1793_track(fdc)) {
            oric_dsk_track_t* t = &fdc->cache;
            for (int i = 0; i < t->num_sectors; i++) {
                if (t->raw[t->sectors[i].id + 1] == fdc->track) {
                    seek_error = false;
                    break;
                }
            }
        }
    }
    fdc->end_status = _wd1793_type1_status(fdc, seek_error);
    _wd1793_wait(fdc, WD1793_TYPE1_US, WD1793_END);
}

static void _wd1793_command(wd1793_t* fdc, uint8_t cmd) {
#ifdef WD1793_TRACE
    fprintf(stderr, "wd1793: cmd %02X track=%d sector=%d data=%d drive=%d side=%d head=%d\n", cmd, fdc->track, fdc->sector,
            fdc->data, fdc->drive, fdc->side, fdc->head[fdc->drive]);
#endif
    if ((cmd & 0xF0) == 0xD0) {
        // Type IV: force interrupt
        fdc->state = WD1793_IDLE;
        fdc->op = WD1793_OP_NONE;
        fdc->status = 0;
        _wd1793_set_drq(fdc, false);
        fdc->intrq = true;
        oric_dsk_flush(&fdc->cache);
        return;
    }
    if (fdc->status & WD1793_ST_BUSY) {
        return;   // Only force interrupt is accepted while busy
    }
    fdc->command = cmd;
    fdc->intrq = false;
    _wd1793_set_drq(fdc, false);
    fdc->status = WD1793_ST_BUSY;
    fdc->end_status = 0;
    fdc->op = WD1793_OP_NONE;

    if (cmd < 0x80) {
        _wd1793_type1(fdc, cmd);
        return;
    }

    // Types II and III: "not ready" stays set while busy, as in Oricutron and Phosphoric
    // (FT-DOS waits for a status >= $81 after starting a read)
    fdc->status = WD1793_ST_BUSY | WD1793_ST_NOT_READY;
    bool present = fdc->present[fdc->drive];
    bool wp = present && fdc->disk[fdc->drive].write_protected;
    switch (cmd & 0xF0) {
        case 0x80:
        case 0x90:
        case 0xA0:
        case 0xB0: {
            // Type II: read sector / write sector
            fdc->op = (cmd & 0x20) ? WD1793_OP_WRITE_SECTOR : WD1793_OP_READ_SECTOR;
            if (fdc->op == WD1793_OP_WRITE_SECTOR && wp) {
                fdc->end_status = WD1793_ST_WPROT;
                _wd1793_wait(fdc, WD1793_TYPE1_US, WD1793_END);
                return;
            }
            int s = _wd1793_find_sector(fdc);
            if (s < 0) {
                fdc->end_status = WD1793_ST_SEEK_RNF | (present ? 0 : WD1793_ST_NOT_READY);
                _wd1793_wait(fdc, WD1793_TYPE1_US, WD1793_END);
                return;
            }
            _wd1793_start_sector(fdc, s);
            _wd1793_wait(fdc, WD1793_FIRST_DRQ_US,
                         fdc->op == WD1793_OP_READ_SECTOR ? WD1793_READ_BYTE : WD1793_WRITE_DRQ);
            return;
        }
        case 0xC0: {
            // Read address: ID field of the next sector passing under the head
            fdc->op = WD1793_OP_READ_ADDRESS;
            if (!_wd1793_track(fdc) || fdc->cache.num_sectors == 0) {
                fdc->end_status = WD1793_ST_SEEK_RNF | (present ? 0 : WD1793_ST_NOT_READY);
                _wd1793_wait(fdc, WD1793_TYPE1_US, WD1793_END);
                return;
            }
            fdc->sec_index = (fdc->sec_index + 1) % fdc->cache.num_sectors;
            fdc->pos = fdc->cache.sectors[fdc->sec_index].id + 1;
            fdc->count = 6;
            _wd1793_wait(fdc, WD1793_FIRST_DRQ_US, WD1793_READ_BYTE);
            return;
        }
        case 0xE0:
        case 0xF0:
            // Read track / write track (format)
            fdc->op = (cmd & 0x10) ? WD1793_OP_WRITE_TRACK : WD1793_OP_READ_TRACK;
            if (!_wd1793_track(fdc)) {
                fdc->end_status = WD1793_ST_SEEK_RNF | (present ? 0 : WD1793_ST_NOT_READY);
                _wd1793_wait(fdc, WD1793_TYPE1_US, WD1793_END);
                return;
            }
            if (fdc->op == WD1793_OP_WRITE_TRACK && wp) {
                fdc->end_status = WD1793_ST_WPROT;
                _wd1793_wait(fdc, WD1793_TYPE1_US, WD1793_END);
                return;
            }
            fdc->pos = 0;
            fdc->count = ORIC_DSK_TRACK_SIZE;
            fdc->crc = 0xFFFF;
            fdc->crc_preset = false;
            _wd1793_wait(fdc, WD1793_FIRST_DRQ_US,
                         fdc->op == WD1793_OP_READ_TRACK ? WD1793_READ_BYTE : WD1793_WRITE_DRQ);
            return;
        default:
            break;
    }
}

static void _wd1793_end(wd1793_t* fdc) {
#ifdef WD1793_TRACE
    fprintf(stderr, "wd1793: end status=%02X\n", fdc->end_status);
#endif
    if (fdc->op == WD1793_OP_WRITE_TRACK) {
        oric_dsk_scan_track(&fdc->cache);
    }
    fdc->status = fdc->end_status;
    _wd1793_set_drq(fdc, false);
    fdc->intrq = true;
    fdc->state = WD1793_IDLE;
    fdc->op = WD1793_OP_NONE;
}

CHIPS_HOT void wd1793_tick(wd1793_t* fdc) {
    switch (fdc->state) {
        case WD1793_IDLE:
        case WD1793_CPU:
            return;

        case WD1793_WAIT:
            if (fdc->wait_us > 1) {
                fdc->wait_us--;
                return;
            }
            fdc->state = fdc->next;
            return;

        case WD1793_READ_BYTE:
            fdc->data = fdc->cache.raw[fdc->pos++];
            fdc->count--;
            _wd1793_set_drq(fdc, true);
            fdc->state = WD1793_CPU;
            return;

        case WD1793_WRITE_DRQ:
            _wd1793_set_drq(fdc, true);
            fdc->state = WD1793_CPU;
            return;

        case WD1793_SECTOR_DONE:
            if (fdc->command & 0x10) {
                // Multiple sectors: carry on with the next one, stop quietly at the end of the track
                fdc->sector++;
                int s = _wd1793_find_sector(fdc);
                if (s >= 0) {
                    _wd1793_start_sector(fdc, s);
                    _wd1793_wait(fdc, WD1793_NEXT_SECTOR_US,
                                 fdc->op == WD1793_OP_READ_SECTOR ? WD1793_READ_BYTE : WD1793_WRITE_DRQ);
                    return;
                }
            }
            _wd1793_end(fdc);
            return;

        case WD1793_END:
            _wd1793_end(fdc);
            return;

        default:
            fdc->state = WD1793_IDLE;
            return;
    }
}

// CPU read of the data register during a transfer
static void _wd1793_data_read(wd1793_t* fdc) {
    _wd1793_set_drq(fdc, false);
    if (fdc->count > 0) {
        _wd1793_wait(fdc, WD1793_BYTE_US, WD1793_READ_BYTE);
        return;
    }
    switch (fdc->op) {
        case WD1793_OP_READ_SECTOR:
            _wd1793_wait(fdc, WD1793_BYTE_US * 2, WD1793_SECTOR_DONE);   // CRC bytes
            break;
        case WD1793_OP_READ_ADDRESS:
            fdc->sector = fdc->cache.raw[fdc->cache.sectors[fdc->sec_index].id + 1];   // Track address -> sector register
            _wd1793_wait(fdc, WD1793_BYTE_US, WD1793_END);
            break;
        default:
            _wd1793_wait(fdc, WD1793_BYTE_US, WD1793_END);
            break;
    }
}

static void _wd1793_put(wd1793_t* fdc, uint8_t v) {
    if (fdc->pos < ORIC_DSK_TRACK_SIZE) {
        fdc->cache.raw[fdc->pos++] = v;
    }
}

// CPU write of the data register during a transfer
static void _wd1793_data_write(wd1793_t* fdc, uint8_t v) {
    _wd1793_set_drq(fdc, false);
    oric_dsk_track_t* t = &fdc->cache;
    if (fdc->op == WD1793_OP_WRITE_SECTOR) {
        _wd1793_put(fdc, v);
        fdc->crc = oric_dsk_crc(fdc->crc, v);
        fdc->count--;
        if (fdc->count > 0) {
            _wd1793_wait(fdc, WD1793_BYTE_US, WD1793_WRITE_DRQ);
            return;
        }
        _wd1793_put(fdc, (uint8_t)(fdc->crc >> 8));
        _wd1793_put(fdc, (uint8_t)fdc->crc);
        oric_dsk_track_modified(t);
        _wd1793_wait(fdc, WD1793_BYTE_US * 2, WD1793_SECTOR_DONE);
        return;
    }
    // Write track: F5 = A1 with CRC preset, F6 = C2, F7 = the 2 CRC bytes
    int written = 1;
    if (v == 0xF5) {
        if (!fdc->crc_preset) fdc->crc = 0xFFFF;
        fdc->crc_preset = true;
        _wd1793_put(fdc, 0xA1);
        fdc->crc = oric_dsk_crc(fdc->crc, 0xA1);
    } else {
        fdc->crc_preset = false;
        if (v == 0xF6) {
            _wd1793_put(fdc, 0xC2);
        } else if (v == 0xF7) {
            uint16_t crc = fdc->crc;
            _wd1793_put(fdc, (uint8_t)(crc >> 8));
            _wd1793_put(fdc, (uint8_t)crc);
            written = 2;
        } else {
            _wd1793_put(fdc, v);
            fdc->crc = oric_dsk_crc(fdc->crc, v);
        }
    }
    oric_dsk_track_modified(t);
    fdc->count -= written;
    if (fdc->count > 0) {
        _wd1793_wait(fdc, WD1793_BYTE_US * written, WD1793_WRITE_DRQ);
    } else {
        _wd1793_wait(fdc, WD1793_BYTE_US, WD1793_END);
    }
}

uint8_t wd1793_read(wd1793_t* fdc, uint8_t reg) {
    switch (reg & 3) {
        case 0:
            fdc->intrq = false;
            return fdc->status;
        case 1:
            return fdc->track;
        case 2:
            return fdc->sector;
        default:
            if (fdc->state == WD1793_CPU && fdc->drq &&
                (fdc->op == WD1793_OP_READ_SECTOR || fdc->op == WD1793_OP_READ_ADDRESS || fdc->op == WD1793_OP_READ_TRACK)) {
                uint8_t v = fdc->data;
                _wd1793_data_read(fdc);
                return v;
            }
            return fdc->data;
    }
}

void wd1793_write(wd1793_t* fdc, uint8_t reg, uint8_t value) {
    switch (reg & 3) {
        case 0:
            _wd1793_command(fdc, value);
            break;
        case 1:
            fdc->track = value;
            break;
        case 2:
            fdc->sector = value;
            break;
        default:
            fdc->data = value;
            if (fdc->state == WD1793_CPU && fdc->drq &&
                (fdc->op == WD1793_OP_WRITE_SECTOR || fdc->op == WD1793_OP_WRITE_TRACK)) {
                _wd1793_data_write(fdc, value);
            }
            break;
    }
}

#endif  // CHIPS_IMPL
