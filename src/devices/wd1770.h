#pragma once

// wd1770.h
//
// Western Digital WD1770 floppy disc controller, as fitted to the BBC Micro
// "Acorn 1770" interface, with Acorn DFS disc images (.ssd single sided,
// .dsd double sided with sides interleaved per track): 10 sectors of 256
// bytes per track, up to 80 tracks.
//
// Written from the WD1770 data sheet (command types I-IV, status bits) and
// the Acorn 1770 interface wiring (control latch at $FE80: bit 0 drive 0,
// bit 1 drive 1, bit 2 side, bit 3 = 0 double density, bit 5 = 0 reset;
// NMI = DRQ or INTRQ). Timings: 64 us per byte (as in the reference
// emulator b2), step rates 6/12/20/30 ms, spin-up shortened to
// WD1770_SPINUP_US.
//
// Not implemented: write track (formatting), read track, precompensation,
// deleted data marks, write to a read-only image (write protect is reported).
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

#define WD1770_SECTOR_SIZE   256
#define WD1770_SECTORS       10
#define WD1770_MAX_TRACKS    80
#define WD1770_US_PER_BYTE   64
#ifndef WD1770_SPINUP_US
#define WD1770_SPINUP_US     20000
#endif
#define WD1770_SETTLE_US     30000

// Status register bits
#define WD1770_ST_BUSY       (1 << 0)
#define WD1770_ST_DRQ_INDEX  (1 << 1)
#define WD1770_ST_LOST_TRK0  (1 << 2)
#define WD1770_ST_CRC        (1 << 3)
#define WD1770_ST_RNF        (1 << 4)
#define WD1770_ST_SPINUP_DEL (1 << 5)
#define WD1770_ST_WPROT      (1 << 6)
#define WD1770_ST_MOTOR      (1 << 7)

typedef enum {
    WD1770_IDLE,
    WD1770_WAIT,             // Waiting `wait_us` then go to `next`
    WD1770_TYPE1_STEP,       // Stepping to the target track
    WD1770_TYPE1_DONE,
    WD1770_TYPE2_START_READ,  // Motor up: locate the sector and start reading
    WD1770_TYPE2_START_WRITE,
    WD1770_READ_BYTE,        // Present the next sector byte
    WD1770_WRITE_FIRST_DRQ,
    WD1770_WRITE_BYTE,       // Wait for the CPU to supply the next byte
    WD1770_ADDR_BYTE,        // Read address: next of the 6 ID bytes
    WD1770_SECTOR_DONE,      // End of a sector, multiple-sector check
    WD1770_END,              // Command complete: INTRQ
} wd1770_state_t;

typedef struct {
    uint8_t* data;           // Image bytes (writable copy) or 0 when no disc
    size_t size;
    int sides;               // 1 (.ssd) or 2 (.dsd)
    bool write_protected;
} wd1770_disc_t;

typedef struct {
    bool valid;
    // Registers
    uint8_t status;
    uint8_t track;           // Track register
    uint8_t sector;          // Sector register
    uint8_t data;            // Data register
    uint8_t command;
    // Pins
    bool drq;
    bool intrq;
    // Acorn control latch ($FE80)
    uint8_t control;
    int drive;               // 0, 1 or -1
    int side;
    // Head
    uint8_t phys_track[2];   // Physical head position per drive
    bool step_in;            // Last step direction (true = towards higher tracks)
    bool motor;
    // Command execution
    wd1770_state_t state;
    wd1770_state_t next;
    uint32_t wait_us;
    uint32_t offset;         // Byte index inside the sector / ID field
    uint8_t id[6];
    bool multiple;
    bool type2_write;
    uint32_t sector_offset;  // Byte offset of the current sector in the image
    // Discs
    wd1770_disc_t disc[2];
} wd1770_t;

void wd1770_init(wd1770_t* fdc);
void wd1770_reset(wd1770_t* fdc);
// Insert an image; data must stay valid; sides = 1 (.ssd) or 2 (.dsd)
void wd1770_insert(wd1770_t* fdc, int drive, uint8_t* data, size_t size, int sides, bool write_protected);
void wd1770_eject(wd1770_t* fdc, int drive);
// Registers at $FE84-$FE87 (addr & 3), control at $FE80
uint8_t wd1770_read(wd1770_t* fdc, uint8_t reg);
void wd1770_write(wd1770_t* fdc, uint8_t reg, uint8_t value);
uint8_t wd1770_read_control(wd1770_t* fdc);
void wd1770_write_control(wd1770_t* fdc, uint8_t value);
// Advance one microsecond (call at 1 MHz)
void wd1770_tick(wd1770_t* fdc);
// NMI level = DRQ or INTRQ
static inline bool wd1770_nmi(const wd1770_t* fdc) { return fdc->drq || fdc->intrq; }

#ifdef __cplusplus
}
#endif

/*-- IMPLEMENTATION ----------------------------------------------------------*/
#ifdef CHIPS_IMPL
#include <string.h>

static const uint16_t _wd1770_step_ms[4] = {6, 12, 20, 30};


void wd1770_init(wd1770_t* fdc) {
    memset(fdc, 0, sizeof(*fdc));
    fdc->valid = true;
    fdc->drive = -1;
    wd1770_reset(fdc);
}

void wd1770_reset(wd1770_t* fdc) {
    fdc->status = 0;
    fdc->track = 0;
    fdc->sector = 1;
    fdc->data = 0;
    fdc->command = 0;
    fdc->drq = false;
    fdc->intrq = false;
    fdc->motor = false;
    fdc->state = WD1770_IDLE;
}

void wd1770_insert(wd1770_t* fdc, int drive, uint8_t* data, size_t size, int sides, bool write_protected) {
    if (drive < 0 || drive > 1) return;
    fdc->disc[drive].data = data;
    fdc->disc[drive].size = size;
    fdc->disc[drive].sides = sides ? sides : 1;
    fdc->disc[drive].write_protected = write_protected;
}

void wd1770_eject(wd1770_t* fdc, int drive) {
    if (drive < 0 || drive > 1) return;
    memset(&fdc->disc[drive], 0, sizeof(wd1770_disc_t));
}

static void _wd1770_set_drq(wd1770_t* fdc, bool on) {
    fdc->drq = on;
    if (on) {
        fdc->status |= WD1770_ST_DRQ_INDEX;
    } else {
        fdc->status &= (uint8_t)~WD1770_ST_DRQ_INDEX;
    }
}

static void _wd1770_wait(wd1770_t* fdc, uint32_t us, wd1770_state_t next) {
    fdc->wait_us = us;
    fdc->next = next;
    fdc->state = WD1770_WAIT;
}

static void _wd1770_end(wd1770_t* fdc) {
#ifdef WD1770_TRACE
    fprintf(stderr, "wd1770: end status=%02X offset=%u\n", fdc->status, (unsigned)fdc->offset);
#endif
    fdc->status &= (uint8_t)~WD1770_ST_BUSY;
    _wd1770_set_drq(fdc, false);
    fdc->intrq = true;
    fdc->state = WD1770_IDLE;
}

// Locate the current sector (track register vs physical track, sector register, side)
static bool _wd1770_find_sector(wd1770_t* fdc) {
    if (fdc->drive < 0) return false;
    wd1770_disc_t* d = &fdc->disc[fdc->drive];
    if (!d->data) return false;
    uint8_t phys = fdc->phys_track[fdc->drive];
    if (phys != fdc->track) return false;                  // ID field track mismatch
    if (fdc->sector >= WD1770_SECTORS) return false;
    int side = fdc->side;
    if (side >= d->sides) return false;
    uint32_t track_index = (uint32_t)phys * (uint32_t)d->sides + (uint32_t)side;
    uint32_t off = (track_index * WD1770_SECTORS + fdc->sector) * WD1770_SECTOR_SIZE;
    if (off + WD1770_SECTOR_SIZE > d->size) return false;
    fdc->sector_offset = off;
    return true;
}

static void _wd1770_start_type2(wd1770_t* fdc, bool write) {
    fdc->type2_write = write;
    fdc->multiple = fdc->command & 0x10;
    fdc->offset = 0;
    if (!_wd1770_find_sector(fdc)) {
        fdc->status |= WD1770_ST_RNF;
        _wd1770_wait(fdc, 2000, WD1770_END);
        return;
    }
    if (write) {
        if (fdc->disc[fdc->drive].write_protected) {
            fdc->status |= WD1770_ST_WPROT;
            _wd1770_wait(fdc, 100, WD1770_END);
            return;
        }
        _wd1770_wait(fdc, WD1770_US_PER_BYTE * 2, WD1770_WRITE_FIRST_DRQ);
    } else {
        _wd1770_wait(fdc, WD1770_US_PER_BYTE * 2, WD1770_READ_BYTE);
    }
}

static void _wd1770_command(wd1770_t* fdc, uint8_t cmd) {
    uint8_t type = cmd >> 4;
#ifdef WD1770_TRACE
    fprintf(stderr, "wd1770: cmd %02X track=%d sector=%d data=%d drive=%d side=%d phys=%d status=%02X\n", cmd, fdc->track,
            fdc->sector, fdc->data, fdc->drive, fdc->side, fdc->drive >= 0 ? fdc->phys_track[fdc->drive] : -1, fdc->status);
#endif

    if (type == 0xD) {
        // Type IV: force interrupt
        bool was_busy = fdc->status & WD1770_ST_BUSY;
        fdc->state = WD1770_IDLE;
        fdc->status &= (uint8_t)~WD1770_ST_BUSY;
        _wd1770_set_drq(fdc, false);
        if (cmd & 0x08) {
            fdc->intrq = true;              // Immediate interrupt
        } else if (!was_busy) {
            fdc->intrq = false;
        }
        if (fdc->drive >= 0 && fdc->phys_track[fdc->drive] == 0) {
            fdc->status |= WD1770_ST_LOST_TRK0;
        }
        return;
    }

    if (fdc->status & WD1770_ST_BUSY) {
        return;                              // Ignored while busy (except force interrupt)
    }

    fdc->command = cmd;
    fdc->intrq = false;
    fdc->status = WD1770_ST_BUSY;
    _wd1770_set_drq(fdc, false);
    bool spin_wait = !fdc->motor;
    fdc->motor = true;
    fdc->status |= WD1770_ST_MOTOR;
    uint32_t delay = spin_wait ? WD1770_SPINUP_US : 0;

    if (type < 0x8) {
        // Type I
        uint16_t rate = _wd1770_step_ms[cmd & 3];
        (void)rate;
        fdc->status |= WD1770_ST_SPINUP_DEL;   // Spin-up completed
        if (fdc->drive >= 0 && fdc->disc[fdc->drive].write_protected) {
            fdc->status |= WD1770_ST_WPROT;
        }
        _wd1770_wait(fdc, delay + 10, WD1770_TYPE1_STEP);
        return;
    }
    if (type < 0xC) {
        // Type II: read sector (8x/9x) or write sector (Ax/Bx)
        if (cmd & 0x04) delay += WD1770_SETTLE_US;
        _wd1770_wait(fdc, delay + 10, type < 0xA ? WD1770_TYPE2_START_READ : WD1770_TYPE2_START_WRITE);
        return;
    }
    // Type III
    if (cmd & 0x04) delay += WD1770_SETTLE_US;
    if (type == 0xC) {
        // Read address: 6 ID bytes of the next sector on the track
        fdc->offset = 0;
        if (fdc->drive < 0 || !fdc->disc[fdc->drive].data) {
            fdc->status |= WD1770_ST_RNF;
            _wd1770_wait(fdc, delay + 2000, WD1770_END);
            return;
        }
        uint8_t phys = fdc->phys_track[fdc->drive];
        fdc->id[0] = phys;
        fdc->id[1] = (uint8_t)fdc->side;
        fdc->id[2] = fdc->sector < WD1770_SECTORS ? fdc->sector : 0;
        fdc->id[3] = 1;                      // 256 bytes
        fdc->id[4] = 0;
        fdc->id[5] = 0;
        _wd1770_wait(fdc, delay + WD1770_US_PER_BYTE, WD1770_ADDR_BYTE);
        return;
    }
    // Read track / write track: not supported, report record not found
    fdc->status |= WD1770_ST_RNF;
    _wd1770_wait(fdc, delay + 2000, WD1770_END);
}

void wd1770_tick(wd1770_t* fdc) {
    switch (fdc->state) {
        case WD1770_IDLE:
            break;

        case WD1770_WAIT:
            if (fdc->wait_us > 0) {
                fdc->wait_us--;
                break;
            }
            fdc->state = fdc->next;
            break;

        case WD1770_TYPE2_START_READ:
            _wd1770_start_type2(fdc, false);
            break;

        case WD1770_TYPE2_START_WRITE:
            _wd1770_start_type2(fdc, true);
            break;

        case WD1770_TYPE1_STEP: {
            uint8_t cmd = fdc->command;
            uint8_t type = cmd >> 4;
            uint16_t rate_us = (uint16_t)(_wd1770_step_ms[cmd & 3] * 1000);
            uint8_t* phys = fdc->drive >= 0 ? &fdc->phys_track[fdc->drive] : &fdc->phys_track[0];
            if (type == 0x0) {
                // Restore: step out until track 0
                if (*phys > 0) {
                    (*phys)--;
                    fdc->step_in = false;
                    _wd1770_wait(fdc, rate_us, WD1770_TYPE1_STEP);
                    break;
                }
                fdc->track = 0;
                fdc->state = WD1770_TYPE1_DONE;
                break;
            }
            if (type == 0x1) {
                // Seek: track register -> data register
                if (fdc->track < fdc->data) {
                    fdc->track++;
                    if (*phys < WD1770_MAX_TRACKS + 3) (*phys)++;
                    fdc->step_in = true;
                    _wd1770_wait(fdc, rate_us, WD1770_TYPE1_STEP);
                    break;
                }
                if (fdc->track > fdc->data) {
                    fdc->track--;
                    if (*phys > 0) (*phys)--;
                    fdc->step_in = false;
                    _wd1770_wait(fdc, rate_us, WD1770_TYPE1_STEP);
                    break;
                }
                fdc->state = WD1770_TYPE1_DONE;
                break;
            }
            // Step (2x/3x), step in (4x/5x), step out (6x/7x); u = bit 4 updates the track register
            bool in = (type >= 0x4 && type <= 0x5) ? true : (type >= 0x6 ? false : fdc->step_in);
            fdc->step_in = in;
            if (in) {
                if (*phys < WD1770_MAX_TRACKS + 3) (*phys)++;
                if (cmd & 0x10) fdc->track++;
            } else {
                if (*phys > 0) (*phys)--;
                if (cmd & 0x10) fdc->track--;
            }
            _wd1770_wait(fdc, rate_us, WD1770_TYPE1_DONE);
            break;
        }

        case WD1770_TYPE1_DONE: {
            uint8_t phys = fdc->drive >= 0 ? fdc->phys_track[fdc->drive] : fdc->phys_track[0];
            if (phys == 0) {
                fdc->status |= WD1770_ST_LOST_TRK0;
            }
            if (fdc->command & 0x04) {
                // Verify: the track must exist on the disc
                if (fdc->drive < 0 || !fdc->disc[fdc->drive].data || phys != fdc->track) {
                    fdc->status |= WD1770_ST_RNF;
                }
            }
            _wd1770_end(fdc);
            break;
        }

        case WD1770_READ_BYTE: {
            const uint8_t* d = fdc->disc[fdc->drive].data;
            if (fdc->drq) {
                fdc->status |= WD1770_ST_LOST_TRK0;    // Lost data: CPU too slow
#ifdef WD1770_TRACE
                fprintf(stderr, "wd1770: lost data at offset %u\n", (unsigned)fdc->offset);
#endif
            }
            fdc->data = d[fdc->sector_offset + fdc->offset];
            fdc->offset++;
            _wd1770_set_drq(fdc, true);
            _wd1770_wait(fdc, WD1770_US_PER_BYTE, fdc->offset < WD1770_SECTOR_SIZE ? WD1770_READ_BYTE : WD1770_SECTOR_DONE);
            break;
        }

        case WD1770_WRITE_FIRST_DRQ:
            _wd1770_set_drq(fdc, true);
            _wd1770_wait(fdc, WD1770_US_PER_BYTE * 9, WD1770_WRITE_BYTE);
            break;

        case WD1770_WRITE_BYTE: {
            uint8_t* d = fdc->disc[fdc->drive].data;
            if (fdc->drq) {
                // The CPU did not supply the byte in time
                fdc->status |= WD1770_ST_LOST_TRK0;
                fdc->data = 0;
            }
            d[fdc->sector_offset + fdc->offset] = fdc->data;
            fdc->offset++;
            if (fdc->offset < WD1770_SECTOR_SIZE) {
                _wd1770_set_drq(fdc, true);
                _wd1770_wait(fdc, WD1770_US_PER_BYTE, WD1770_WRITE_BYTE);
            } else {
                _wd1770_wait(fdc, WD1770_US_PER_BYTE, WD1770_SECTOR_DONE);
            }
            break;
        }

        case WD1770_ADDR_BYTE:
            fdc->data = fdc->id[fdc->offset++];
            _wd1770_set_drq(fdc, true);
            if (fdc->offset < 6) {
                _wd1770_wait(fdc, WD1770_US_PER_BYTE, WD1770_ADDR_BYTE);
            } else {
                fdc->sector = fdc->id[0];        // The 1770 copies the track ID into the sector register
                _wd1770_wait(fdc, WD1770_US_PER_BYTE, WD1770_END);
            }
            break;

        case WD1770_SECTOR_DONE:
            if (fdc->multiple) {
                fdc->sector++;
                fdc->offset = 0;
                if (_wd1770_find_sector(fdc)) {
                    _wd1770_wait(fdc, WD1770_US_PER_BYTE * 2, fdc->type2_write ? WD1770_WRITE_FIRST_DRQ : WD1770_READ_BYTE);
                    break;
                }
                fdc->status |= WD1770_ST_RNF;
            }
            fdc->state = WD1770_END;
            break;

        case WD1770_END:
            _wd1770_end(fdc);
            break;

        default:
            fdc->state = WD1770_IDLE;
            break;
    }
}

uint8_t wd1770_read(wd1770_t* fdc, uint8_t reg) {
    switch (reg & 3) {
        case 0:
            fdc->intrq = false;
            return fdc->status;
        case 1:
            return fdc->track;
        case 2:
            return fdc->sector;
        default:
            _wd1770_set_drq(fdc, false);
            return fdc->data;
    }
}

void wd1770_write(wd1770_t* fdc, uint8_t reg, uint8_t value) {
    switch (reg & 3) {
        case 0:
            _wd1770_command(fdc, value);
            break;
        case 1:
            fdc->track = value;
            break;
        case 2:
            fdc->sector = value;
            break;
        default:
            fdc->data = value;
            _wd1770_set_drq(fdc, false);
            break;
    }
}

uint8_t wd1770_read_control(wd1770_t* fdc) {
    // DFS 2.26 loops at startup until bit 0 or 1 reads back set (drive select),
    // so a drive is always reported selected (drive 0 when none was chosen).
    return (uint8_t)((fdc->control & ~3u) | (fdc->drive == 1 ? 2 : 1));
}

void wd1770_write_control(wd1770_t* fdc, uint8_t value) {
#ifdef WD1770_TRACE
    fprintf(stderr, "wd1770: control %02X\n", value);
#endif
    fdc->control = value;
    if (value & 1) {
        fdc->drive = 0;
    } else if (value & 2) {
        fdc->drive = 1;
    } else {
        fdc->drive = -1;
    }
    fdc->side = (value & 4) ? 1 : 0;
    if (!(value & 0x20)) {
        wd1770_reset(fdc);
    }
}

#endif  // CHIPS_IMPL
