#pragma once

// tube.h
//
// Acorn Tube ULA: four register pairs shared between the BBC host ($FEE0-$FEE7)
// and a second processor ($FEF8-$FEFF on the 6502 parasite).
//
// Register 1: host->parasite 1 byte, parasite->host 24-byte FIFO.
// Register 2: 1 byte each way. Register 3: 2-byte FIFOs (1-byte mode unless V),
// drives PNMI. Register 4: 1 byte each way, drives HIRQ (Q) and PIRQ (J).
// Status/control bits written by the host to register 0: S (bit 7: set/clear),
// T (reset FIFOs), P (reset parasite), V, M (PNMI enable), J, I, Q.
// Each FIFO status byte read from the status registers: bit 7 = data available,
// bit 6 = not full. Behaviour as documented by Acorn and checked against b2.
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

#ifdef __cplusplus
extern "C" {
#endif

#define TUBE_FIFO1_SIZE 24

#define TUBE_ST_Q 0x01
#define TUBE_ST_I 0x02
#define TUBE_ST_J 0x04
#define TUBE_ST_M 0x08
#define TUBE_ST_V 0x10
#define TUBE_ST_P 0x20
#define TUBE_ST_T 0x40
#define TUBE_ST_S 0x80

#define TUBE_AVAILABLE 0x80
#define TUBE_NOT_FULL  0x40

typedef struct {
    uint8_t status;            // Control bits Q..T (bit 7 unused)
    // Per register: host status (A/F as seen by the host), parasite status
    uint8_t hstat[4], pstat[4];
    uint8_t h2p1;
    uint8_t p2h1[TUBE_FIFO1_SIZE];
    uint8_t p2h1_r, p2h1_w, p2h1_n;
    uint8_t h2p2, p2h2;
    uint8_t h2p3[2], p2h3[2];
    uint8_t h2p3_n, p2h3_n;
    bool pnmi_flag;
    uint8_t h2p4, p2h4;
    uint8_t last_h2p, last_p2h;
    // Outputs
    bool hirq, pirq, pnmi;
} tube_t;

void tube_init(tube_t* t);
void tube_reset(tube_t* t);
// Host side, reg = address & 7
uint8_t tube_host_read(tube_t* t, uint8_t reg);
void tube_host_write(tube_t* t, uint8_t reg, uint8_t value);
// Parasite side, reg = address & 7
uint8_t tube_parasite_read(tube_t* t, uint8_t reg);
void tube_parasite_write(tube_t* t, uint8_t reg, uint8_t value);

#ifdef __cplusplus
}
#endif

/*-- IMPLEMENTATION ----------------------------------------------------------*/
#ifdef CHIPS_IMPL
#include <string.h>

static void _tube_update_irqs(tube_t* t) {
    t->hirq = (t->status & TUBE_ST_Q) && (t->hstat[3] & TUBE_AVAILABLE);
    t->pirq = ((t->status & TUBE_ST_I) && (t->pstat[0] & TUBE_AVAILABLE)) ||
              ((t->status & TUBE_ST_J) && (t->pstat[3] & TUBE_AVAILABLE));
    t->pnmi = (t->status & TUBE_ST_M) && t->pnmi_flag;
}

// 1-byte latch read by `this` side: no longer available here, not full on the other side
static void _tube_latch_read(uint8_t* this_stat, uint8_t* other_stat) {
    if (*this_stat & TUBE_AVAILABLE) {
        *this_stat &= (uint8_t)~TUBE_AVAILABLE;
        *other_stat |= TUBE_NOT_FULL;
    }
}

static void _tube_latch_write(uint8_t* this_stat, uint8_t* other_stat) {
    *this_stat &= (uint8_t)~TUBE_NOT_FULL;
    *other_stat |= TUBE_AVAILABLE;
}

void tube_init(tube_t* t) {
    memset(t, 0, sizeof(*t));
    tube_reset(t);
}

void tube_reset(tube_t* t) {
    for (int i = 0; i < 4; i++) {
        t->hstat[i] = TUBE_NOT_FULL;
        t->pstat[i] = TUBE_NOT_FULL;
    }
    t->p2h1_r = t->p2h1_w = t->p2h1_n = 0;
    // Register 3 after reset: host sees data available (one dummy byte), parasite sees full
    t->hstat[2] = TUBE_NOT_FULL | TUBE_AVAILABLE;
    t->pstat[2] = 0;
    if (t->p2h3_n == 0) t->p2h3[0] = t->last_p2h;
    t->p2h3_n = 1;
    t->h2p3_n = 0;
    t->pnmi_flag = false;
    _tube_update_irqs(t);
}

static uint8_t _tube_fifo3_read(uint8_t* this_stat, uint8_t* other_stat, uint8_t* fifo, uint8_t* n, uint8_t dummy) {
    uint8_t v = dummy;
    if (*n > 0) {
        v = fifo[0];
        fifo[0] = fifo[1];
        (*n)--;
    }
    if (*n == 0) {
        *this_stat &= (uint8_t)~TUBE_AVAILABLE;
        *other_stat |= TUBE_NOT_FULL;
    }
    return v;
}

static void _tube_fifo3_write(tube_t* t, uint8_t* this_stat, uint8_t* other_stat, uint8_t* fifo, uint8_t* n, uint8_t v) {
    if (*n < 2) {
        fifo[*n] = v;
        (*n)++;
    }
    if (t->status & TUBE_ST_V) {
        if (*n == 2) {
            *this_stat &= (uint8_t)~TUBE_NOT_FULL;
            *other_stat |= TUBE_AVAILABLE;
        }
    } else {
        _tube_latch_write(this_stat, other_stat);
    }
}

uint8_t tube_host_read(tube_t* t, uint8_t reg) {
    uint8_t v;
    switch (reg & 7) {
        case 0: return (uint8_t)(t->hstat[0] | (t->status & 0x3F));
        case 1:
            v = t->last_p2h;
            if (t->p2h1_n > 0) {
                v = t->p2h1[t->p2h1_r];
                t->p2h1_r = (uint8_t)((t->p2h1_r + 1) % TUBE_FIFO1_SIZE);
                t->p2h1_n--;
                t->pstat[0] |= TUBE_NOT_FULL;
                if (t->p2h1_n == 0) t->hstat[0] &= (uint8_t)~TUBE_AVAILABLE;
            }
            return v;
        case 2: return t->hstat[1];
        case 3:
            _tube_latch_read(&t->hstat[1], &t->pstat[1]);
            return t->p2h2;
        case 4: return t->hstat[2];
        case 5:
            v = _tube_fifo3_read(&t->hstat[2], &t->pstat[2], t->p2h3, &t->p2h3_n, t->last_p2h);
            if (t->p2h3_n == 0) t->pnmi_flag = true;
            _tube_update_irqs(t);
            return v;
        case 6: return t->hstat[3];
        default:
            _tube_latch_read(&t->hstat[3], &t->pstat[3]);
            _tube_update_irqs(t);
            return t->p2h4;
    }
}

void tube_host_write(tube_t* t, uint8_t reg, uint8_t value) {
    switch (reg & 7) {
        case 0:
            if (value & TUBE_ST_S) t->status |= value; else t->status &= (uint8_t)~value;
            break;
        case 1:
            t->h2p1 = value;
            _tube_latch_write(&t->hstat[0], &t->pstat[0]);
            break;
        case 3:
            t->h2p2 = value;
            _tube_latch_write(&t->hstat[1], &t->pstat[1]);
            break;
        case 5:
            _tube_fifo3_write(t, &t->hstat[2], &t->pstat[2], t->h2p3, &t->h2p3_n, value);
            if (t->status & TUBE_ST_V) {
                if (t->h2p3_n == 2) t->pnmi_flag = true;
            } else if (t->h2p3_n >= 1) {
                t->pnmi_flag = true;
            }
            break;
        case 7:
            t->h2p4 = value;
            _tube_latch_write(&t->hstat[3], &t->pstat[3]);
            break;
        default:
            break;
    }
    t->last_h2p = value;
    _tube_update_irqs(t);
}

uint8_t tube_parasite_read(tube_t* t, uint8_t reg) {
    uint8_t v;
    switch (reg & 7) {
        case 0: return (uint8_t)(t->pstat[0] | (t->status & 0x3F));
        case 1:
            _tube_latch_read(&t->pstat[0], &t->hstat[0]);
            _tube_update_irqs(t);
            return t->h2p1;
        case 2: return t->pstat[1];
        case 3:
            _tube_latch_read(&t->pstat[1], &t->hstat[1]);
            return t->h2p2;
        case 4: return (uint8_t)((t->pstat[2] & TUBE_NOT_FULL) | (t->pnmi_flag ? TUBE_AVAILABLE : 0));
        case 5:
            v = _tube_fifo3_read(&t->pstat[2], &t->hstat[2], t->h2p3, &t->h2p3_n, t->last_h2p);
            if (t->h2p3_n == 0) t->pnmi_flag = false;
            _tube_update_irqs(t);
            return v;
        case 6: return t->pstat[3];
        default:
            _tube_latch_read(&t->pstat[3], &t->hstat[3]);
            _tube_update_irqs(t);
            return t->h2p4;
    }
}

void tube_parasite_write(tube_t* t, uint8_t reg, uint8_t value) {
    switch (reg & 7) {
        case 1:
            if (t->p2h1_n < TUBE_FIFO1_SIZE) {
                t->p2h1[t->p2h1_w] = value;
                t->p2h1_w = (uint8_t)((t->p2h1_w + 1) % TUBE_FIFO1_SIZE);
                t->p2h1_n++;
                if (t->p2h1_n == TUBE_FIFO1_SIZE) t->pstat[0] &= (uint8_t)~TUBE_NOT_FULL;
                t->hstat[0] |= TUBE_AVAILABLE;
            }
            break;
        case 3:
            t->p2h2 = value;
            _tube_latch_write(&t->pstat[1], &t->hstat[1]);
            break;
        case 5:
            _tube_fifo3_write(t, &t->pstat[2], &t->hstat[2], t->p2h3, &t->p2h3_n, value);
            if (t->status & TUBE_ST_V) {
                if (t->p2h3_n == 2) t->pnmi_flag = false;
            } else {
                t->pnmi_flag = false;
            }
            break;
        case 7:
            t->p2h4 = value;
            _tube_latch_write(&t->pstat[3], &t->hstat[3]);
            break;
        default:
            break;
    }
    t->last_p2h = value;
    _tube_update_irqs(t);
}

#endif  // CHIPS_IMPL
