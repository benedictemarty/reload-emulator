#pragma once

// oric_td.h
//
// Oric tape drive: WAVE images (tools/tap2wave) or .tap images played
// directly (the signal is built on the fly, byte by byte, so that a .tap file
// of any size can be read from memory or streamed from a file).

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// Tape drive port bits
#define ORIC_TD_PORT_MOTOR  (1 << 0)
#define ORIC_TD_PORT_READ   (1 << 1)
#define ORIC_TD_PORT_WRITE  (1 << 2)
#define ORIC_TD_PORT_PLAY   (1 << 3)
#define ORIC_TD_PORT_RECORD (1 << 4)

// Streamed .tap access: read up to `len` bytes at `offset` of the file, returns the count read
typedef uint32_t (*oric_td_read_t)(void* ctx, uint32_t offset, uint8_t* buf, uint32_t len);

#define ORIC_TD_TAP_BUF_SIZE 256

// Oric tape drive state
typedef struct {
    uint8_t port;
    bool valid;
    uint32_t pos;
    uint32_t bit_pos;
    uint32_t size;
    uint8_t* wave_image;
    // .tap image played directly (signal built on the fly, see the implementation)
    bool tap_mode;
    const uint8_t* tap;          // In memory, or 0 when streamed
    oric_td_read_t tap_read;
    void* tap_ctx;
    uint32_t tap_size;
    uint32_t tap_pos;            // Offset of the next byte to send
    uint32_t tap_buf_off;        // File offset of tap_buf (streamed)
    uint32_t tap_buf_len;
    uint8_t tap_buf[ORIC_TD_TAP_BUF_SIZE];
    uint8_t tap_byte;            // Byte being sent
    uint8_t tap_bit;             // Bit of the byte being sent (0-13), 14 = next byte to fetch
    uint8_t tap_parity;
    uint8_t tap_level;           // Output level
    uint8_t tap_len;             // Units per level of the current bit
    uint8_t tap_units;           // Units left at this level
    uint8_t tap_delay;           // Units of 1 pulses left in the pause after a header
    uint16_t tap_dup;            // Leader bytes (0x16) still to repeat
    uint32_t tap_hdr_end;        // Offset of the first byte after a header + name (0 = none)
    bool tap_motor;              // Motor state seen by the last tick
    bool tap_end;
    uint32_t tap_motor_pos;      // Byte offset when the motor started
    uint32_t tap_file_end;       // End of the file being read (offset after its data)
    uint16_t tap_pad;            // Missing data bytes of a truncated last file, sent as 0x00
    uint8_t tap_trailer;         // 1 bits sent after the last byte
} oric_td_t;

// Oric tape drive interface

// Initialize a new tape drive
void oric_td_init(oric_td_t* sys);

// Discard the tape drive
void oric_td_discard(oric_td_t* sys);

// Reset the tape drive (the tape stays in the drive and is rewound)
void oric_td_reset(oric_td_t* sys);

// Tick the tape drive (every 208 us)
void oric_td_tick(oric_td_t* sys);

// Insert a new tape file (WAVE image: 32-bit size, then 1 bit per 208 us, MSB first)
bool oric_td_insert_tape(oric_td_t* sys, uint8_t* wave_image);

// Insert a .tap image held in memory (the buffer must stay valid)
bool oric_td_insert_tap(oric_td_t* sys, const uint8_t* tap, uint32_t size);

// Insert a .tap image read through `read` (e.g. a file on a USB drive)
bool oric_td_insert_tap_streamed(oric_td_t* sys, uint32_t size, oric_td_read_t read, void* ctx);

// Rewind the tape
void oric_td_rewind(oric_td_t* sys);

// True once a .tap image has been played to its end
bool oric_td_tap_ended(oric_td_t* sys);

// Remove the tape file
void oric_td_remove_tape(oric_td_t* sys);

// Return true if the tape is currently inserted
bool oric_td_is_tape_inserted(oric_td_t* sys);

// Start playing the tape (press the Play button)
void oric_td_play(oric_td_t* sys);

// Start recording the tape (press the Record button)
void oric_td_record(oric_td_t* sys);

// Stop the tape (press the Stop button)
void oric_td_stop(oric_td_t* sys);

// Return true if the tape drive motor is on
bool oric_td_is_motor_on(oric_td_t* sys);

// Prepare a new tape drive snapshot for saving
void oric_td_snapshot_onsave(oric_td_t* snapshot);

// Fix up the tape drive snapshot after loading
void oric_td_snapshot_onload(oric_td_t* snapshot, oric_td_t* sys);

#ifdef __cplusplus
}  // extern "C"
#endif

/*-- IMPLEMENTATION ----------------------------------------------------------*/
#ifdef CHIPS_IMPL
#include <string.h>  // memcpy, memset
#ifndef CHIPS_ASSERT
#include <assert.h>
#define CHIPS_ASSERT(c) assert(c)
#endif

void oric_td_init(oric_td_t* sys) {
    CHIPS_ASSERT(sys && !sys->valid);
    memset(sys, 0, sizeof(oric_td_t));
    sys->valid = true;
    sys->bit_pos = 7;
    oric_td_rewind(sys);
}

void oric_td_discard(oric_td_t* sys) {
    CHIPS_ASSERT(sys && sys->valid);
    sys->valid = false;
}

void oric_td_reset(oric_td_t* sys) {
    CHIPS_ASSERT(sys && sys->valid);
    sys->port = 0;
    oric_td_rewind(sys);
}

/*-- .tap signal generator ---------------------------------------------------*/
// Same signal as Oricutron (tape.c), in units of 208 us (one oric_td_tick):
// each bit is a high then a low level of equal length, 1 unit for a 1,
// 2 units for a 0; a byte is 14 bits: 1, start bit 0, 8 data bits (LSB
// first), parity (1 when the number of 1 data bits is even), 1, 1, 1.
// When the motor starts on a leader (0x16 bytes, then 0x24), its first byte
// is repeated 80 more times and a pause of 6 units of 1 pulses (Oricutron:
// 1281 cycles) follows the header and the file name, so that the ROM is
// ready to read the data. When the motor stops, the byte being sent is dropped.
//
// The motor also runs while the ROM starts (a real user presses PLAY after
// typing CLOAD): when the motor stops before the end of the file being read,
// the tape goes back to where the motor started; once a whole file has been
// read, the tape stays after it. A last file shorter than its header says
// (end address counted exclusive by some tools) is completed with 0x00 bytes,
// and 4 bits 1 follow the last byte.

#define _ORIC_TD_TAP_BITS  14
#define _ORIC_TD_TAP_DUP   80
#define _ORIC_TD_TAP_PAUSE 6
#define _ORIC_TD_TAP_TRAILER 4

static uint8_t _oric_td_tap_peek(oric_td_t* sys, uint32_t p) {
    if (p >= sys->tap_size) {
        return 0;
    }
    if (sys->tap) {
        return sys->tap[p];
    }
    if (p < sys->tap_buf_off || p >= sys->tap_buf_off + sys->tap_buf_len) {
        sys->tap_buf_off = p;
        sys->tap_buf_len = sys->tap_read(sys->tap_ctx, p, sys->tap_buf, ORIC_TD_TAP_BUF_SIZE);
        if (p >= sys->tap_buf_off + sys->tap_buf_len) {
            return 0;   // Read error
        }
    }
    return sys->tap_buf[p - sys->tap_buf_off];
}

// Parse the file whose leader starts at or after `p`: offset after its header + name
// (0 if none), and after its data (clamped to the tape size, `pad` = missing bytes)
static uint32_t _oric_td_tap_parse(oric_td_t* sys, uint32_t p, uint32_t* file_end, uint32_t* pad) {
    *file_end = sys->tap_size;
    *pad = 0;
    int sync = 0;
    for (; p < sys->tap_size; p++) {
        uint8_t b = _oric_td_tap_peek(sys, p);
        if (b == 0x16) {
            sync++;
        } else if (b == 0x24 && sync >= 3) {
            break;
        } else {
            sync = 0;
        }
    }
    p++;   // After 0x24
    if (p + 9 > sys->tap_size) {
        return 0;
    }
    uint32_t end = _oric_td_tap_peek(sys, p + 4) * 256u + _oric_td_tap_peek(sys, p + 5);
    uint32_t start = _oric_td_tap_peek(sys, p + 6) * 256u + _oric_td_tap_peek(sys, p + 7);
    p += 9;
    while (p < sys->tap_size && _oric_td_tap_peek(sys, p) != 0) {   // File name
        p++;
    }
    if (p >= sys->tap_size) {
        return 0;
    }
    p++;
    uint32_t hdr_end = p;
    uint32_t size = (end >= start) ? end - start + 1 : 0;
    if (hdr_end + size > sys->tap_size) {
        uint32_t missing = hdr_end + size - sys->tap_size;
        *pad = missing <= 256 ? missing : 0;
    } else {
        *file_end = hdr_end + size;
    }
    return hdr_end;
}

// At a leader: repeat its first byte and locate the end of the header + name
static void _oric_td_tap_setup_header(oric_td_t* sys) {
    sys->tap_dup = 0;
    sys->tap_hdr_end = 0;
    uint32_t p = sys->tap_pos;
    if (_oric_td_tap_peek(sys, p) != 0x16) {
        return;
    }
    uint32_t i = 0;
    while (p + i < sys->tap_size && _oric_td_tap_peek(sys, p + i) == 0x16) {
        i++;
    }
    if (i < 3 || p + i >= sys->tap_size || _oric_td_tap_peek(sys, p + i) != 0x24) {
        return;
    }
    i += 1 + 9;   // 0x24, header
    if (p + i >= sys->tap_size) {
        return;
    }
    while (_oric_td_tap_peek(sys, p + i) != 0) {   // File name
        i++;
        if (p + i >= sys->tap_size) {
            return;
        }
    }
    i++;
    sys->tap_dup = _ORIC_TD_TAP_DUP;
    sys->tap_hdr_end = p + i;
}

// Length in units of the bit starting now; 0 at the end of the tape
static uint8_t _oric_td_tap_next_bit(oric_td_t* sys) {
    if (sys->tap_bit >= _ORIC_TD_TAP_BITS) {
        if (sys->tap_hdr_end != 0 && sys->tap_pos == sys->tap_hdr_end && sys->tap_dup == 0) {
            sys->tap_hdr_end = 0;
            sys->tap_delay = _ORIC_TD_TAP_PAUSE;
        }
        if (sys->tap_delay > 0) {
            return 1;
        }
        if (sys->tap_pos >= sys->tap_size) {
            if (sys->tap_pad == 0) {
                // A few 1 bits after the last byte (its last bit ends with a rising edge)
                if (sys->tap_trailer < _ORIC_TD_TAP_TRAILER) {
                    sys->tap_trailer++;
                    return 1;
                }
                return 0;
            }
            sys->tap_pad--;
            sys->tap_byte = 0x00;
            sys->tap_bit = 0;
            sys->tap_parity = 1;
            sys->tap_bit = 1;
            return 1;   // Lead bit (1) of the padding byte
        }
        sys->tap_byte = _oric_td_tap_peek(sys, sys->tap_pos);
        if (sys->tap_dup > 0) {
            sys->tap_dup--;
        } else {
            sys->tap_pos++;
        }
        sys->tap_bit = 0;
        sys->tap_parity = 1;
    }
    uint8_t bit;
    switch (sys->tap_bit) {
        case 0: bit = 1; break;                           // Lead 1
        case 1: bit = 0; break;                           // Start bit
        case 10: bit = sys->tap_parity & 1; break;        // Parity
        case 11: case 12: case 13: bit = 1; break;        // Stop bits
        default:                                          // Data bits 0-7
            bit = (sys->tap_byte >> (sys->tap_bit - 2)) & 1;
            sys->tap_parity ^= bit;
            break;
    }
    sys->tap_bit++;
    return bit ? 1 : 2;
}

// Advance the .tap signal by one unit (motor on)
static void _oric_td_tap_unit(oric_td_t* sys) {
    if (sys->tap_end) {
        return;
    }
    if (sys->tap_units > 0) {
        sys->tap_units--;
        return;
    }
    sys->tap_level ^= 1;
    if (sys->tap_level) {
        // High level: a new bit, or a pulse of the pause after a header
        uint8_t len = _oric_td_tap_next_bit(sys);
        if (len == 0) {
            sys->tap_level = 0;
            sys->tap_end = true;
            return;
        }
        sys->tap_len = len;
    } else if (sys->tap_delay > 0) {
        sys->tap_delay = (uint8_t)(sys->tap_delay > 2 ? sys->tap_delay - 2 : 0);
    }
    sys->tap_units = (uint8_t)(sys->tap_len - 1);   // The low level lasts as long as the high level
}

void oric_td_tick(oric_td_t* sys) {
    CHIPS_ASSERT(sys && sys->valid);
    if (sys->tap_mode) {
        bool motor = oric_td_is_motor_on(sys);
        if (motor != sys->tap_motor) {
            sys->tap_motor = motor;
            if (!motor) {
                // Motor off: the byte being sent is dropped; back to where the motor
                // started unless the file being read was read to its end
                sys->tap_bit = _ORIC_TD_TAP_BITS;
                sys->tap_units = 0;
                sys->tap_delay = 0;
                sys->tap_dup = 0;
                sys->tap_hdr_end = 0;
                if (sys->tap_pos < sys->tap_file_end || (sys->tap_pos >= sys->tap_size && sys->tap_pad > 0)) {
                    sys->tap_pos = sys->tap_motor_pos;
                    sys->tap_end = false;
                }
            } else {
                if (sys->tap_bit < _ORIC_TD_TAP_BITS) {
                    sys->tap_bit = _ORIC_TD_TAP_BITS;
                    sys->tap_pos++;
                }
                sys->tap_motor_pos = sys->tap_pos;
                sys->tap_trailer = 0;
                uint32_t pad = 0;
                _oric_td_tap_parse(sys, sys->tap_pos, &sys->tap_file_end, &pad);
                sys->tap_pad = (uint16_t)pad;
                _oric_td_tap_setup_header(sys);
            }
        }
        if (motor) {
            _oric_td_tap_unit(sys);
            if (sys->tap_level) {
                sys->port |= ORIC_TD_PORT_READ;
            } else {
                sys->port &= ~ORIC_TD_PORT_READ;
            }
        }
        return;
    }
    if (oric_td_is_motor_on(sys) && (sys->size > 0) && (sys->pos < sys->size)) {
        uint8_t b = sys->wave_image[sys->pos];
        b >>= sys->bit_pos;
        if (b & 1) {
            sys->port |= ORIC_TD_PORT_READ;
        } else {
            sys->port &= ~ORIC_TD_PORT_READ;
        }
        if (sys->bit_pos == 0) {
            sys->bit_pos = 7;
            sys->pos++;
        } else {
            sys->bit_pos--;
        }
    }
}

void oric_td_rewind(oric_td_t* sys) {
    CHIPS_ASSERT(sys && sys->valid);
    sys->pos = 0;
    sys->bit_pos = 7;
    sys->tap_pos = 0;
    sys->tap_buf_off = 0;
    sys->tap_buf_len = 0;
    sys->tap_bit = _ORIC_TD_TAP_BITS;
    sys->tap_parity = 1;
    sys->tap_level = 0;
    sys->tap_len = 1;
    sys->tap_units = 0;
    sys->tap_delay = 0;
    sys->tap_dup = 0;
    sys->tap_hdr_end = 0;
    sys->tap_motor = false;
    sys->tap_end = false;
    sys->tap_motor_pos = 0;
    sys->tap_file_end = 0;
    sys->tap_pad = 0;
    sys->tap_trailer = 0;
}

bool oric_td_insert_tap(oric_td_t* sys, const uint8_t* tap, uint32_t size) {
    CHIPS_ASSERT(sys && sys->valid);
    if (!tap || size == 0) return false;
    sys->tap_mode = true;
    sys->tap = tap;
    sys->tap_read = 0;
    sys->tap_ctx = 0;
    sys->tap_size = size;
    sys->size = 0;
    oric_td_rewind(sys);
    return true;
}

bool oric_td_insert_tap_streamed(oric_td_t* sys, uint32_t size, oric_td_read_t read, void* ctx) {
    CHIPS_ASSERT(sys && sys->valid);
    if (!read || size == 0) return false;
    sys->tap_mode = true;
    sys->tap = 0;
    sys->tap_read = read;
    sys->tap_ctx = ctx;
    sys->tap_size = size;
    sys->size = 0;
    oric_td_rewind(sys);
    return true;
}

bool oric_td_tap_ended(oric_td_t* sys) {
    CHIPS_ASSERT(sys && sys->valid);
    return sys->tap_mode && sys->tap_end;
}

bool oric_td_insert_tape(oric_td_t* sys, uint8_t* wave_image) {
    CHIPS_ASSERT(sys && sys->valid);
    sys->tap_mode = false;
    sys->tap_size = 0;
    sys->size = wave_image[0] | (wave_image[1] << 8) | (wave_image[2] << 16) | (wave_image[3] << 24);
    sys->pos = 0;
    sys->wave_image = &wave_image[4];
    return true;
}

void oric_td_remove_tape(oric_td_t* sys) {
    CHIPS_ASSERT(sys && sys->valid);
    oric_td_stop(sys);
    sys->size = 0;
    sys->pos = 0;
    sys->tap_mode = false;
    sys->tap = 0;
    sys->tap_read = 0;
    sys->tap_size = 0;
}

bool oric_td_is_tape_inserted(oric_td_t* sys) {
    CHIPS_ASSERT(sys && sys->valid);
    return sys->size > 0 || (sys->tap_mode && sys->tap_size > 0);
}

void oric_td_play(oric_td_t* sys) {
    CHIPS_ASSERT(sys && sys->valid);
    // motor on, play button down
    sys->port &= (ORIC_TD_PORT_MOTOR | ORIC_TD_PORT_PLAY);
}

void oric_td_record(oric_td_t* sys) {
    CHIPS_ASSERT(sys && sys->valid);
    // motor on, record button down
    sys->port &= (ORIC_TD_PORT_MOTOR | ORIC_TD_PORT_RECORD);
}

void oric_td_stop(oric_td_t* sys) {
    CHIPS_ASSERT(sys && sys->valid);
    // Set motor off, play and record buttons up
    sys->port &= ~(ORIC_TD_PORT_MOTOR | ORIC_TD_PORT_PLAY | ORIC_TD_PORT_RECORD);
}

bool oric_td_is_motor_on(oric_td_t* sys) {
    CHIPS_ASSERT(sys && sys->valid);
    return 0 != (sys->port & ORIC_TD_PORT_MOTOR);
}

void oric_td_snapshot_onsave(oric_td_t* snapshot) {
    CHIPS_ASSERT(snapshot);
    snapshot->port = 0;
}

void oric_td_snapshot_onload(oric_td_t* snapshot, oric_td_t* sys) {
    CHIPS_ASSERT(snapshot && sys);
    snapshot->port = sys->port;
}

#endif  // CHIPS_IMPL
