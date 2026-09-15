#pragma once

// w65c02cpu.h
//
// Cycle-stepped WDC W65C02S emulator with the same interface as mos6502cpu.h
// (one call per clock cycle; the core drives `addr`, `rw` and, for writes,
// `data`; the memory system answers reads by storing into `data` before the
// next tick).
//
// Bus cycles follow the W65C02S data sheet (table 6-1): CMOS behaviour for
// the extra cycles (dummy reads instead of dummy writes, JMP (abs) in 6
// cycles without the page bug, BRK/interrupts clear D, decimal ADC/SBC take
// one more cycle and set N/Z/V from the decimal result), the 65C02 opcodes
// (BRA, PHX/PHY/PLX/PLY, STZ, TSB/TRB, BIT #/zp,X/abs,X, INC A/DEC A,
// JMP (abs,X), (zp) addressing) and the Rockwell/WDC bit instructions
// (RMB/SMB/BBR/BBS, WAI, STP). Undefined opcodes are the CMOS NOPs of the
// documented lengths.
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

typedef struct {
    int unused;
} w65c02cpu_desc_t;

typedef struct {
    // Bus
    uint16_t addr;
    uint8_t data;
    bool rw;        // true = read
    bool sync;      // true during the opcode fetch cycle
    // Control inputs
    bool irq;       // Level
    bool nmi;       // Level (edge detected internally)
    bool nmi_triggered;
    bool res;
    bool rdy;       // Unused (kept for interface compatibility)
    // Registers
    uint16_t PC;
    uint8_t A, X, Y, S;
    bool cf, zf, iflag, df, bf, vf, nf;
    // Execution state
    uint8_t op;         // Current opcode
    uint8_t step;       // Cycle within the instruction (0 = fetch)
    uint16_t ea;        // Effective address
    uint8_t tmp;        // Operand / intermediate byte
    uint8_t irq_vec;    // Vector low byte for the interrupt being taken (0xFA NMI, 0xFE IRQ/BRK)
    bool in_brk;        // Executing a software BRK (B flag pushed set)
    bool page_cross;
    bool waiting;       // WAI executed: waiting for an interrupt
    bool stopped;       // STP executed
    bool nmi_pending;   // NMI edge seen, not yet serviced
    bool take_irq;      // Interrupt sampled at the end of the previous instruction
    bool brk_nmi;       // Interface compatibility (last interrupt was NMI)
    bool brk_irq;
    uint16_t irq_pip, nmi_pip;   // Interface compatibility (unused)
} w65c02cpu_t;

#define MOS6502CPU_T                 w65c02cpu_t
#define MOS6502CPU_DESC_T            w65c02cpu_desc_t
#define MOS6502CPU_INIT(c, desc)     w65c02cpu_init(c)
#define MOS6502CPU_RESET(c)          ((c)->res = true)
#define MOS6502CPU_NMI(c)            ((c)->nmi_triggered = !(c)->nmi, (c)->nmi = true)
#define MOS6502CPU_TICK(c)           w65c02cpu_tick(c)
#define MOS6502CPU_GET_ADDR(c)       ((c)->addr)
#define MOS6502CPU_GET_DATA(c)       ((c)->data)
#define MOS6502CPU_SET_DATA(c, d)    ((c)->data = d)
#define MOS6502CPU_SET_IRQ(c, state) ((c)->irq = state)
#define MOS6502CPU_SET_NMI(c, state) ((c)->nmi_triggered = ((state) && !(c)->nmi), (c)->nmi = (state))

void w65c02cpu_init(w65c02cpu_t* c);
void w65c02cpu_tick(w65c02cpu_t* c);

#ifdef __cplusplus
}
#endif

/*-- IMPLEMENTATION ----------------------------------------------------------*/
#ifdef CHIPS_IMPL
#include <string.h>

void w65c02cpu_init(w65c02cpu_t* c) {
    memset(c, 0, sizeof(*c));
    c->rw = true;
    c->res = true;
    c->iflag = true;
    c->S = 0xFD;
}

/*-- Helpers -----------------------------------------------------------------*/

static inline void _w65_rd(w65c02cpu_t* c, uint16_t a) { c->addr = a; c->rw = true; }
static inline void _w65_wr(w65c02cpu_t* c, uint16_t a, uint8_t d) { c->addr = a; c->rw = false; c->data = d; }
static inline void _w65_nz(w65c02cpu_t* c, uint8_t v) { c->zf = (v == 0); c->nf = (v & 0x80) != 0; }

static inline uint8_t _w65_get_p(const w65c02cpu_t* c, bool b) {
    return (uint8_t)((c->nf ? 0x80 : 0) | (c->vf ? 0x40 : 0) | 0x20 | (b ? 0x10 : 0) | (c->df ? 0x08 : 0) |
                     (c->iflag ? 0x04 : 0) | (c->zf ? 0x02 : 0) | (c->cf ? 0x01 : 0));
}

static inline void _w65_set_p(w65c02cpu_t* c, uint8_t p) {
    c->nf = p & 0x80; c->vf = p & 0x40; c->df = p & 0x08; c->iflag = p & 0x04; c->zf = p & 0x02; c->cf = p & 0x01;
}

// Addressing modes
enum {
    W65_IMP, W65_ACC, W65_IMM, W65_ZP, W65_ZPX, W65_ZPY, W65_ABS, W65_ABX, W65_ABY, W65_INX, W65_INY, W65_ZPI,
    W65_REL, W65_JMP_ABS, W65_JMP_IND, W65_JMP_INX, W65_JSR, W65_RTS, W65_RTI, W65_BRK, W65_PUSH, W65_PULL,
    W65_ZPREL,  // BBR/BBS
    W65_NOP1, W65_NOP2_2, W65_NOP2_3, W65_NOP2_4, W65_NOP3_4, W65_NOP3_8, W65_WAI, W65_STP,
};

// Operation kinds
enum { W65_K_READ, W65_K_RMW, W65_K_WRITE, W65_K_OTHER };

// Decoded per opcode: mode, kind, "always extra cycle for indexed" flag
static uint8_t _w65_mode(uint8_t op) {
    // Column-based decoding of the 65C02 map
    uint8_t lo = op & 0x0F, hi = op >> 4;
    switch (lo) {
        case 0x0:
            if (op == 0x00) return W65_BRK;
            if (op == 0x20) return W65_JSR;
            if (op == 0x40) return W65_RTI;
            if (op == 0x60) return W65_RTS;
            if (op == 0x80) return W65_REL;      // BRA
            if (op == 0xA0 || op == 0xC0 || op == 0xE0) return W65_IMM;
            return W65_REL;                       // Bxx
        case 0x1: return (hi & 1) ? W65_INY : W65_INX;
        case 0x2:
            if (hi >= 0xA && hi <= 0xA) return W65_IMM;   // LDX #
            if (hi & 1) return W65_ZPI;                    // (zp)
            return W65_NOP2_2;                             // 02 22 42 62 82 C2 E2
        case 0x3: return W65_NOP1;
        case 0xB:
            if (op == 0xCB) return W65_WAI;
            if (op == 0xDB) return W65_STP;
            return W65_NOP1;
        case 0x4:
            if (op == 0x04 || op == 0x14) return (op == 0x04) ? W65_ZP : W65_ZP;   // TSB zp, TRB zp
            if (op == 0x24) return W65_ZP;                 // BIT zp
            if (op == 0x34) return W65_ZPX;                // BIT zp,X
            if (op == 0x44) return W65_NOP2_3;
            if (op == 0x54 || op == 0xD4 || op == 0xF4) return W65_NOP2_4;
            if (op == 0x64) return W65_ZP;                 // STZ zp
            if (op == 0x74) return W65_ZPX;                // STZ zp,X
            if (op == 0x84 || op == 0xA4 || op == 0xC4 || op == 0xE4) return W65_ZP;
            if (op == 0x94 || op == 0xB4) return W65_ZPX;
            return W65_NOP2_4;
        case 0x5: return (hi & 1) ? W65_ZPX : W65_ZP;
        case 0x6:
            if (hi & 1) return (hi == 0x9 || hi == 0xB) ? W65_ZPY : W65_ZPX;   // STX/LDX zp,Y
            return W65_ZP;
        case 0x7: return W65_ZP;                           // RMBx / SMBx
        case 0x8:
            if (op == 0x08 || op == 0x48) return W65_PUSH;      // PHP PHA
            if (op == 0x28 || op == 0x68) return W65_PULL;      // PLP PLA
            return W65_IMP;
        case 0x9:
            if (hi & 1) return W65_ABY;
            return (op == 0x89) ? W65_IMM : W65_IMM;       // xx9 even rows: immediate (89 = BIT #)
        case 0xA:
            if (op == 0x5A || op == 0xDA) return W65_PUSH;      // PHY PHX
            if (op == 0x7A || op == 0xFA) return W65_PULL;      // PLY PLX
            return W65_ACC;                                     // shifts A, INC A/DEC A, TXA...
        case 0xC:
            if (op == 0x0C || op == 0x1C) return (op == 0x0C) ? W65_ABS : W65_ABS;   // TSB abs, TRB abs
            if (op == 0x2C) return W65_ABS;                // BIT abs
            if (op == 0x3C) return W65_ABX;                // BIT abs,X
            if (op == 0x4C) return W65_JMP_ABS;
            if (op == 0x5C) return W65_NOP3_8;
            if (op == 0x6C) return W65_JMP_IND;
            if (op == 0x7C) return W65_JMP_INX;
            if (op == 0x8C || op == 0xAC || op == 0xCC || op == 0xEC) return W65_ABS;
            if (op == 0x9C) return W65_ABS;                // STZ abs
            if (op == 0xBC) return W65_ABX;                // LDY abs,X
            return W65_NOP3_4;                             // DC FC
        case 0xD: return (hi & 1) ? W65_ABX : W65_ABS;
        case 0xE:
            if (hi & 1) return (hi == 0xB) ? W65_ABY : W65_ABX;   // LDX abs,Y ; 9E STZ abs,X
            return W65_ABS;
        case 0xF: return W65_ZPREL;                        // BBRx / BBSx
    }
    return W65_IMP;
}

static uint8_t _w65_kind(uint8_t op) {
    uint8_t lo = op & 0x0F, hi = op >> 4;
    switch (lo) {
        case 0x1: case 0x2: case 0x5: case 0x9: case 0xD:
            if (op == 0x89) return W65_K_READ;                        // BIT #
            if (hi == 0x8 || hi == 0x9) return W65_K_WRITE;           // STA
            return W65_K_READ;
        case 0x4:
            if (op == 0x04 || op == 0x0C || op == 0x14 || op == 0x1C) return W65_K_RMW;   // TSB/TRB
            if (op == 0x64 || op == 0x74 || op == 0x84 || op == 0x94) return W65_K_WRITE; // STZ/STY
            return W65_K_READ;
        case 0x6: case 0xE:
            if (hi == 0x8 || hi == 0x9) return W65_K_WRITE;           // STX, STZ 9E
            if (hi == 0xA || hi == 0xB) return W65_K_READ;            // LDX
            return W65_K_RMW;                                          // ASL/ROL/LSR/ROR/DEC/INC
        case 0x7: return W65_K_RMW;                                    // RMB/SMB
        case 0xC:
            if (op == 0x0C || op == 0x1C) return W65_K_RMW;
            if (op == 0x8C || op == 0x9C) return W65_K_WRITE;         // STY abs, STZ abs
            return W65_K_READ;
        default: return W65_K_OTHER;
    }
}

/*-- ALU ---------------------------------------------------------------------*/

static void _w65_adc(w65c02cpu_t* c, uint8_t v) {
    if (c->df) {
        uint8_t cin = c->cf ? 1 : 0;
        int lo = (c->A & 0x0F) + (v & 0x0F) + cin;
        int hi = (c->A >> 4) + (v >> 4);
        if (lo > 9) { lo += 6; hi++; }
        int bin = c->A + v + cin;
        c->vf = (~(c->A ^ v) & (c->A ^ (uint8_t)(hi << 4)) & 0x80) != 0;
        if (hi > 9) hi += 6;
        c->cf = hi > 15;
        c->A = (uint8_t)(((hi & 0x0F) << 4) | (lo & 0x0F));
        (void)bin;
        _w65_nz(c, c->A);
    } else {
        int r = c->A + v + (c->cf ? 1 : 0);
        c->vf = (~(c->A ^ v) & (c->A ^ r) & 0x80) != 0;
        c->cf = r > 0xFF;
        c->A = (uint8_t)r;
        _w65_nz(c, c->A);
    }
}

static void _w65_sbc(w65c02cpu_t* c, uint8_t v) {
    if (c->df) {
        int cin = c->cf ? 0 : 1;
        int r = c->A - v - cin;
        int lo = (c->A & 0x0F) - (v & 0x0F) - cin;
        int hi = (c->A >> 4) - (v >> 4);
        if (lo < 0) { lo -= 6; hi--; }
        if (hi < 0) hi -= 6;
        c->vf = ((c->A ^ v) & (c->A ^ r) & 0x80) != 0;
        c->cf = r >= 0;
        c->A = (uint8_t)(((hi & 0x0F) << 4) | (lo & 0x0F));
        _w65_nz(c, c->A);
    } else {
        int r = c->A - v - (c->cf ? 0 : 1);
        c->vf = ((c->A ^ v) & (c->A ^ r) & 0x80) != 0;
        c->cf = r >= 0;
        c->A = (uint8_t)r;
        _w65_nz(c, c->A);
    }
}

static void _w65_cmp(w65c02cpu_t* c, uint8_t r, uint8_t v) {
    int d = r - v;
    c->cf = d >= 0;
    _w65_nz(c, (uint8_t)d);
}

// Read-type operation with operand v
static void _w65_op_read(w65c02cpu_t* c, uint8_t v) {
    uint8_t op = c->op;
    switch (op) {
        // ORA
        case 0x01: case 0x05: case 0x09: case 0x0D: case 0x11: case 0x12: case 0x15: case 0x19: case 0x1D:
            c->A |= v; _w65_nz(c, c->A); break;
        // AND
        case 0x21: case 0x25: case 0x29: case 0x2D: case 0x31: case 0x32: case 0x35: case 0x39: case 0x3D:
            c->A &= v; _w65_nz(c, c->A); break;
        // EOR
        case 0x41: case 0x45: case 0x49: case 0x4D: case 0x51: case 0x52: case 0x55: case 0x59: case 0x5D:
            c->A ^= v; _w65_nz(c, c->A); break;
        // ADC
        case 0x61: case 0x65: case 0x69: case 0x6D: case 0x71: case 0x72: case 0x75: case 0x79: case 0x7D:
            _w65_adc(c, v); break;
        // LDA
        case 0xA1: case 0xA5: case 0xA9: case 0xAD: case 0xB1: case 0xB2: case 0xB5: case 0xB9: case 0xBD:
            c->A = v; _w65_nz(c, v); break;
        // CMP
        case 0xC1: case 0xC5: case 0xC9: case 0xCD: case 0xD1: case 0xD2: case 0xD5: case 0xD9: case 0xDD:
            _w65_cmp(c, c->A, v); break;
        // SBC
        case 0xE1: case 0xE5: case 0xE9: case 0xED: case 0xF1: case 0xF2: case 0xF5: case 0xF9: case 0xFD:
            _w65_sbc(c, v); break;
        // LDX
        case 0xA2: case 0xA6: case 0xAE: case 0xB6: case 0xBE:
            c->X = v; _w65_nz(c, v); break;
        // LDY
        case 0xA0: case 0xA4: case 0xAC: case 0xB4: case 0xBC:
            c->Y = v; _w65_nz(c, v); break;
        // CPX
        case 0xE0: case 0xE4: case 0xEC: _w65_cmp(c, c->X, v); break;
        // CPY
        case 0xC0: case 0xC4: case 0xCC: _w65_cmp(c, c->Y, v); break;
        // BIT (immediate form only affects Z)
        case 0x89: c->zf = (c->A & v) == 0; break;
        case 0x24: case 0x2C: case 0x34: case 0x3C:
            c->zf = (c->A & v) == 0; c->nf = (v & 0x80) != 0; c->vf = (v & 0x40) != 0; break;
        default: break;
    }
}

// Read-modify-write operation on v, returns the new value
static uint8_t _w65_op_rmw(w65c02cpu_t* c, uint8_t v) {
    uint8_t op = c->op;
    if ((op & 0x0F) == 0x07) {
        // RMBx (0x07..0x77) / SMBx (0x87..0xF7)
        uint8_t bit = (uint8_t)(1 << ((op >> 4) & 7));
        return (op & 0x80) ? (uint8_t)(v | bit) : (uint8_t)(v & ~bit);
    }
    switch (op) {
        case 0x04: case 0x0C: c->zf = (c->A & v) == 0; return (uint8_t)(v | c->A);   // TSB
        case 0x14: case 0x1C: c->zf = (c->A & v) == 0; return (uint8_t)(v & ~c->A);  // TRB
        default: break;
    }
    switch (op & 0xE0) {
        case 0x00: c->cf = (v & 0x80) != 0; v <<= 1; break;                                   // ASL
        case 0x20: { bool cy = c->cf; c->cf = (v & 0x80) != 0; v = (uint8_t)((v << 1) | (cy ? 1 : 0)); break; }   // ROL
        case 0x40: c->cf = (v & 0x01) != 0; v >>= 1; break;                                   // LSR
        case 0x60: { bool cy = c->cf; c->cf = (v & 0x01) != 0; v = (uint8_t)((v >> 1) | (cy ? 0x80 : 0)); break; } // ROR
        case 0xC0: v--; break;                                                               // DEC
        case 0xE0: v++; break;                                                               // INC
        default: break;
    }
    _w65_nz(c, v);
    return v;
}

// Value written by a write-type operation
static uint8_t _w65_op_write(w65c02cpu_t* c) {
    uint8_t op = c->op;
    switch (op) {
        case 0x64: case 0x74: case 0x9C: case 0x9E: return 0;       // STZ
        case 0x84: case 0x8C: case 0x94: return c->Y;               // STY
        case 0x86: case 0x8E: case 0x96: return c->X;               // STX
        default: return c->A;                                       // STA
    }
}

// Single-byte operations (implied / accumulator), executed during the dummy cycle
static void _w65_op_imp(w65c02cpu_t* c) {
    switch (c->op) {
        case 0x0A: c->A = _w65_op_rmw(c, c->A); break;   // ASL A
        case 0x2A: c->A = _w65_op_rmw(c, c->A); break;   // ROL A
        case 0x4A: c->A = _w65_op_rmw(c, c->A); break;   // LSR A
        case 0x6A: c->A = _w65_op_rmw(c, c->A); break;   // ROR A
        case 0x1A: c->A++; _w65_nz(c, c->A); break;      // INC A
        case 0x3A: c->A--; _w65_nz(c, c->A); break;      // DEC A
        case 0x18: c->cf = false; break;   // CLC
        case 0x38: c->cf = true; break;    // SEC
        case 0x58: c->iflag = false; break;// CLI
        case 0x78: c->iflag = true; break; // SEI
        case 0xB8: c->vf = false; break;   // CLV
        case 0xD8: c->df = false; break;   // CLD
        case 0xF8: c->df = true; break;    // SED
        case 0x8A: c->A = c->X; _w65_nz(c, c->A); break;   // TXA
        case 0x98: c->A = c->Y; _w65_nz(c, c->A); break;   // TYA
        case 0xAA: c->X = c->A; _w65_nz(c, c->X); break;   // TAX
        case 0xA8: c->Y = c->A; _w65_nz(c, c->Y); break;   // TAY
        case 0xBA: c->X = c->S; _w65_nz(c, c->X); break;   // TSX
        case 0x9A: c->S = c->X; break;                     // TXS
        case 0xCA: c->X--; _w65_nz(c, c->X); break;        // DEX
        case 0x88: c->Y--; _w65_nz(c, c->Y); break;        // DEY
        case 0xE8: c->X++; _w65_nz(c, c->X); break;        // INX
        case 0xC8: c->Y++; _w65_nz(c, c->Y); break;        // INY
        case 0xEA: break;                                  // NOP
        default: break;
    }
}

static bool _w65_branch_taken(const w65c02cpu_t* c) {
    switch (c->op) {
        case 0x10: return !c->nf;   // BPL
        case 0x30: return c->nf;    // BMI
        case 0x50: return !c->vf;   // BVC
        case 0x70: return c->vf;    // BVS
        case 0x90: return !c->cf;   // BCC
        case 0xB0: return c->cf;    // BCS
        case 0xD0: return !c->zf;   // BNE
        case 0xF0: return c->zf;    // BEQ
        case 0x80: return true;     // BRA
        default: return false;
    }
}

/*-- Sequencer ---------------------------------------------------------------*/

#define _W65_FETCH() do { c->step = 0; _w65_rd(c, c->PC); c->sync = true; } while (0)
#define _W65_STACK() ((uint16_t)(0x0100 | c->S))

// Called when the effective address is known and the operand cycle starts.
// Returns true if the instruction is complete (fetch was issued).
static bool _w65_operand_cycle(w65c02cpu_t* c, uint8_t kind, uint8_t phase) {
    // phase 0: issue the access; phase 1: (RMW) dummy read; phase 2: (RMW) write
    switch (kind) {
        case W65_K_READ:
            _w65_rd(c, c->ea);
            return false;
        case W65_K_WRITE:
            _w65_wr(c, c->ea, _w65_op_write(c));
            return false;
        case W65_K_RMW:
            if (phase == 0) { _w65_rd(c, c->ea); return false; }
            if (phase == 1) { c->tmp = c->data; _w65_rd(c, c->ea); return false; }   // CMOS: dummy read
            _w65_wr(c, c->ea, _w65_op_rmw(c, c->tmp));
            return false;
        default:
            return false;
    }
}

void w65c02cpu_tick(w65c02cpu_t* c) {
    // NMI edge
    if (c->nmi_triggered) {
        c->nmi_pending = true;
        c->nmi_triggered = false;
    }

    if (c->res) {
        // Reset: 7 cycles then the vector at $FFFC
        c->res = false;
        c->stopped = false;
        c->waiting = false;
        c->in_brk = false;
        c->irq_vec = 0xFC;
        c->step = 100;   // Interrupt sequence, reset flavour
        c->sync = false;
        _w65_rd(c, c->PC);
        return;
    }

    if (c->stopped) {
        _w65_rd(c, c->PC);
        return;
    }

    if (c->waiting) {
        // WAI: resume on any interrupt line (IRQ even if masked, NMI)
        if (c->nmi_pending || c->irq) {
            c->waiting = false;
        } else {
            _w65_rd(c, c->PC);
            return;
        }
    }

    if (c->step == 0 && c->take_irq) {
        // Interrupt sampled at the end of the previous instruction: the fetch just
        // done is the dummy first cycle of the 7-cycle sequence
        c->take_irq = false;
        c->in_brk = false;
        c->sync = false;
        c->step = 100;
    }

    // Interrupt / reset sequence (steps 100..106)
    if (c->step >= 100) {
        switch (c->step) {
            case 100:   // Dummy read of PC (BRK: operand byte, PC already incremented)
                if (c->irq_vec == 0xFC) { _w65_rd(c, c->PC); c->step = 101; return; }
                _w65_rd(c, c->PC);
                c->step = 101;
                return;
            case 101:
                if (c->irq_vec == 0xFC) { _w65_rd(c, _W65_STACK()); c->S--; }
                else { _w65_wr(c, _W65_STACK(), (uint8_t)(c->PC >> 8)); c->S--; }
                c->step = 102;
                return;
            case 102:
                if (c->irq_vec == 0xFC) { _w65_rd(c, _W65_STACK()); c->S--; }
                else { _w65_wr(c, _W65_STACK(), (uint8_t)c->PC); c->S--; }
                c->step = 103;
                return;
            case 103:
                if (c->irq_vec == 0xFC) { _w65_rd(c, _W65_STACK()); c->S--; }
                else { _w65_wr(c, _W65_STACK(), _w65_get_p(c, c->in_brk)); c->S--; }
                c->step = 104;
                return;
            case 104:
                // Vector selection: an NMI arriving during BRK/IRQ hijacks the vector on the 65C02 as well
                if (c->irq_vec != 0xFC && c->nmi_pending) { c->irq_vec = 0xFA; c->nmi_pending = false; }
                _w65_rd(c, (uint16_t)(0xFF00 | c->irq_vec));
                c->iflag = true;
                c->df = false;
                c->step = 105;
                return;
            case 105:
                c->tmp = c->data;
                _w65_rd(c, (uint16_t)(0xFF00 | (c->irq_vec + 1)));
                c->step = 106;
                return;
            case 106:
                c->PC = (uint16_t)(c->tmp | (c->data << 8));
                c->brk_nmi = (c->irq_vec == 0xFA);
                c->brk_irq = (c->irq_vec == 0xFE) && !c->in_brk;
                c->in_brk = false;
                _W65_FETCH();
                return;
            default:
                _W65_FETCH();
                return;
        }
    }

    if (c->step == 0) {
        // The opcode has been fetched: decode, or take an interrupt sampled before the fetch
        c->sync = false;
        c->op = c->data;
        c->PC++;
        c->step = 1;
        c->page_cross = false;
        c->brk_nmi = c->brk_irq = false;
    }

    uint8_t op = c->op;
    uint8_t mode = _w65_mode(op);
    uint8_t kind = _w65_kind(op);
    uint8_t s = c->step;

    // Helper to finish an instruction: sample interrupts, then fetch
#define _W65_DONE() do { \
        if (c->nmi_pending) { c->nmi_pending = false; c->irq_vec = 0xFA; c->take_irq = true; } \
        else if (c->irq && !c->iflag) { c->irq_vec = 0xFE; c->take_irq = true; } \
        _W65_FETCH(); return; } while (0)
    // Read-type operand consumed at the fetch of the next opcode
#define _W65_READ_DONE() do { _w65_op_read(c, c->data); \
        if (c->df && ((op & 0xE0) == 0x60 || (op & 0xE0) == 0xE0) && ((op & 3) == 1 || (op & 0x1F) == 0x12)) { \
            /* Decimal ADC/SBC: one extra cycle (dummy read) */ c->step = 90; _w65_rd(c, c->PC); return; } \
        _W65_DONE(); } while (0)

    if (s == 90) { _W65_DONE(); }

    switch (mode) {
        case W65_IMP:
        case W65_ACC:
            // 2 cycles: dummy read of the next byte while the operation executes
            if (s == 1) { _w65_rd(c, c->PC); c->step = 2; return; }
            _w65_op_imp(c);
            _W65_DONE();

        case W65_IMM:
            if (s == 1) { _w65_rd(c, c->PC); c->PC++; c->step = 2; return; }
            _W65_READ_DONE();

        case W65_ZP:
            if (s == 1) { _w65_rd(c, c->PC); c->PC++; c->step = 2; return; }
            if (s == 2) { c->ea = c->data; c->step = 3; _w65_operand_cycle(c, kind, 0); return; }
            if (kind == W65_K_READ) _W65_READ_DONE();
            if (kind == W65_K_WRITE) _W65_DONE();
            if (s == 3) { c->step = 4; _w65_operand_cycle(c, kind, 1); return; }
            if (s == 4) { c->step = 5; _w65_operand_cycle(c, kind, 2); return; }
            _W65_DONE();

        case W65_ZPX:
        case W65_ZPY:
            if (s == 1) { _w65_rd(c, c->PC); c->PC++; c->step = 2; return; }
            if (s == 2) { c->tmp = c->data; _w65_rd(c, (uint16_t)(c->PC - 1)); c->step = 3; return; }   // CMOS: re-read operand
            if (s == 3) { c->ea = (uint8_t)(c->tmp + (mode == W65_ZPX ? c->X : c->Y)); c->step = 4; _w65_operand_cycle(c, kind, 0); return; }
            if (kind == W65_K_READ) _W65_READ_DONE();
            if (kind == W65_K_WRITE) _W65_DONE();
            if (s == 4) { c->step = 5; _w65_operand_cycle(c, kind, 1); return; }
            if (s == 5) { c->step = 6; _w65_operand_cycle(c, kind, 2); return; }
            _W65_DONE();

        case W65_ABS:
            if (s == 1) { _w65_rd(c, c->PC); c->PC++; c->step = 2; return; }
            if (s == 2) { c->tmp = c->data; _w65_rd(c, c->PC); c->PC++; c->step = 3; return; }
            if (s == 3) { c->ea = (uint16_t)(c->tmp | (c->data << 8)); c->step = 4; _w65_operand_cycle(c, kind, 0); return; }
            if (kind == W65_K_READ) _W65_READ_DONE();
            if (kind == W65_K_WRITE) _W65_DONE();
            if (s == 4) { c->step = 5; _w65_operand_cycle(c, kind, 1); return; }
            if (s == 5) { c->step = 6; _w65_operand_cycle(c, kind, 2); return; }
            _W65_DONE();

        case W65_ABX:
        case W65_ABY: {
            if (s == 1) { _w65_rd(c, c->PC); c->PC++; c->step = 2; return; }
            if (s == 2) { c->tmp = c->data; _w65_rd(c, c->PC); c->PC++; c->step = 3; return; }
            if (s == 3) {
                uint16_t base = (uint16_t)(c->tmp | (c->data << 8));
                uint8_t idx = (mode == W65_ABX) ? c->X : c->Y;
                c->ea = (uint16_t)(base + idx);
                c->page_cross = ((base ^ c->ea) & 0xFF00) != 0;
                // Extra cycle: page crossing (reads), always for writes, always for INC/DEC abs,X,
                // page crossing only for the shifts abs,X
                bool extra;
                if (kind == W65_K_WRITE) extra = true;
                else if (kind == W65_K_RMW) extra = c->page_cross || (op == 0xDE || op == 0xFE);
                else extra = c->page_cross;
                if (extra) { _w65_rd(c, (uint16_t)(c->PC - 1)); c->step = 4; return; }   // CMOS dummy read of the last operand byte
                c->step = 5; _w65_operand_cycle(c, kind, 0); return;
            }
            if (s == 4) { c->step = 5; _w65_operand_cycle(c, kind, 0); return; }
            if (kind == W65_K_READ) _W65_READ_DONE();
            if (kind == W65_K_WRITE) _W65_DONE();
            if (s == 5) { c->step = 6; _w65_operand_cycle(c, kind, 1); return; }
            if (s == 6) { c->step = 7; _w65_operand_cycle(c, kind, 2); return; }
            _W65_DONE();
        }

        case W65_INX:
            if (s == 1) { _w65_rd(c, c->PC); c->PC++; c->step = 2; return; }
            if (s == 2) { c->tmp = c->data; _w65_rd(c, (uint16_t)(c->PC - 1)); c->step = 3; return; }
            if (s == 3) { c->tmp = (uint8_t)(c->tmp + c->X); _w65_rd(c, c->tmp); c->step = 4; return; }
            if (s == 4) { c->ea = c->data; _w65_rd(c, (uint8_t)(c->tmp + 1)); c->step = 5; return; }
            if (s == 5) { c->ea |= (uint16_t)(c->data << 8); c->step = 6; _w65_operand_cycle(c, kind, 0); return; }
            if (kind == W65_K_READ) _W65_READ_DONE();
            _W65_DONE();

        case W65_INY: {
            if (s == 1) { _w65_rd(c, c->PC); c->PC++; c->step = 2; return; }
            if (s == 2) { c->tmp = c->data; _w65_rd(c, c->tmp); c->step = 3; return; }
            if (s == 3) { c->ea = c->data; _w65_rd(c, (uint8_t)(c->tmp + 1)); c->step = 4; return; }
            if (s == 4) {
                uint16_t base = (uint16_t)(c->ea | (c->data << 8));
                c->ea = (uint16_t)(base + c->Y);
                c->page_cross = ((base ^ c->ea) & 0xFF00) != 0;
                if (kind == W65_K_WRITE || c->page_cross) { _w65_rd(c, (uint8_t)(c->tmp + 1)); c->step = 5; return; }   // CMOS dummy read of the pointer high byte
                c->step = 6; _w65_operand_cycle(c, kind, 0); return;
            }
            if (s == 5) { c->step = 6; _w65_operand_cycle(c, kind, 0); return; }
            if (kind == W65_K_READ) _W65_READ_DONE();
            _W65_DONE();
        }

        case W65_ZPI:
            if (s == 1) { _w65_rd(c, c->PC); c->PC++; c->step = 2; return; }
            if (s == 2) { c->tmp = c->data; _w65_rd(c, c->tmp); c->step = 3; return; }
            if (s == 3) { c->ea = c->data; _w65_rd(c, (uint8_t)(c->tmp + 1)); c->step = 4; return; }
            if (s == 4) { c->ea |= (uint16_t)(c->data << 8); c->step = 5; _w65_operand_cycle(c, kind, 0); return; }
            if (kind == W65_K_READ) _W65_READ_DONE();
            _W65_DONE();

        case W65_REL:
            if (s == 1) { _w65_rd(c, c->PC); c->PC++; c->step = 2; return; }
            if (s == 2) {
                c->tmp = c->data;
                if (!_w65_branch_taken(c)) _W65_DONE();
                uint16_t target = (uint16_t)(c->PC + (int8_t)c->tmp);
                c->page_cross = ((target ^ c->PC) & 0xFF00) != 0;
                _w65_rd(c, c->PC);       // Dummy read
                c->ea = target;
                c->step = 3;
                return;
            }
            if (s == 3 && c->page_cross) { _w65_rd(c, c->PC); c->step = 4; c->PC = c->ea; return; }
            c->PC = c->ea;
            _W65_DONE();

        case W65_ZPREL:   // BBRx / BBSx: 5 cycles, +1 if branch taken, +1 more on page cross
            if (s == 1) { _w65_rd(c, c->PC); c->PC++; c->step = 2; return; }
            if (s == 2) { _w65_rd(c, c->data); c->step = 3; return; }
            if (s == 3) { c->tmp = c->data; _w65_rd(c, c->PC); c->PC++; c->step = 4; return; }   // Offset (data now = zp value; keep in tmp)
            if (s == 4) {
                uint8_t bit = (uint8_t)(1 << ((op >> 4) & 7));
                bool set = (c->tmp & bit) != 0;
                bool taken = (op & 0x80) ? set : !set;
                int8_t off = (int8_t)c->data;
                _w65_rd(c, c->PC);   // Dummy read
                if (!taken) { c->step = 5; c->ea = c->PC; c->page_cross = false; return; }
                uint16_t target = (uint16_t)(c->PC + off);
                c->page_cross = ((target ^ c->PC) & 0xFF00) != 0;
                c->ea = target;
                c->step = 5;
                return;
            }
            if (s == 5) {
                if (c->page_cross) { _w65_rd(c, c->PC); c->step = 6; c->PC = c->ea; return; }
                c->PC = c->ea;
                _W65_DONE();
            }
            _W65_DONE();

        case W65_JMP_ABS:
            if (s == 1) { _w65_rd(c, c->PC); c->PC++; c->step = 2; return; }
            if (s == 2) { c->tmp = c->data; _w65_rd(c, c->PC); c->step = 3; return; }
            c->PC = (uint16_t)(c->tmp | (c->data << 8));
            _W65_DONE();

        case W65_JMP_IND:   // 6 cycles on the 65C02, no page-wrap bug
            if (s == 1) { _w65_rd(c, c->PC); c->PC++; c->step = 2; return; }
            if (s == 2) { c->tmp = c->data; _w65_rd(c, c->PC); c->step = 3; return; }
            if (s == 3) { c->ea = (uint16_t)(c->tmp | (c->data << 8)); _w65_rd(c, c->PC); c->step = 4; return; }   // Extra cycle
            if (s == 4) { _w65_rd(c, c->ea); c->step = 5; return; }
            if (s == 5) { c->tmp = c->data; _w65_rd(c, (uint16_t)(c->ea + 1)); c->step = 6; return; }
            c->PC = (uint16_t)(c->tmp | (c->data << 8));
            _W65_DONE();

        case W65_JMP_INX:   // JMP (abs,X): 6 cycles
            if (s == 1) { _w65_rd(c, c->PC); c->PC++; c->step = 2; return; }
            if (s == 2) { c->tmp = c->data; _w65_rd(c, c->PC); c->step = 3; return; }
            if (s == 3) { c->ea = (uint16_t)((c->tmp | (c->data << 8)) + c->X); _w65_rd(c, c->PC); c->step = 4; return; }
            if (s == 4) { _w65_rd(c, c->ea); c->step = 5; return; }
            if (s == 5) { c->tmp = c->data; _w65_rd(c, (uint16_t)(c->ea + 1)); c->step = 6; return; }
            c->PC = (uint16_t)(c->tmp | (c->data << 8));
            _W65_DONE();

        case W65_JSR:
            if (s == 1) { _w65_rd(c, c->PC); c->PC++; c->step = 2; return; }
            if (s == 2) { c->tmp = c->data; _w65_rd(c, _W65_STACK()); c->step = 3; return; }
            if (s == 3) { _w65_wr(c, _W65_STACK(), (uint8_t)(c->PC >> 8)); c->S--; c->step = 4; return; }
            if (s == 4) { _w65_wr(c, _W65_STACK(), (uint8_t)c->PC); c->S--; c->step = 5; return; }
            if (s == 5) { _w65_rd(c, c->PC); c->step = 6; return; }
            c->PC = (uint16_t)(c->tmp | (c->data << 8));
            _W65_DONE();

        case W65_RTS:
            if (s == 1) { _w65_rd(c, c->PC); c->step = 2; return; }
            if (s == 2) { _w65_rd(c, _W65_STACK()); c->S++; c->step = 3; return; }
            if (s == 3) { _w65_rd(c, _W65_STACK()); c->S++; c->step = 4; return; }
            if (s == 4) { c->tmp = c->data; _w65_rd(c, _W65_STACK()); c->step = 5; return; }
            if (s == 5) { c->PC = (uint16_t)(c->tmp | (c->data << 8)); _w65_rd(c, c->PC); c->PC++; c->step = 6; return; }
            _W65_DONE();

        case W65_RTI:
            if (s == 1) { _w65_rd(c, c->PC); c->step = 2; return; }
            if (s == 2) { _w65_rd(c, _W65_STACK()); c->S++; c->step = 3; return; }
            if (s == 3) { _w65_rd(c, _W65_STACK()); c->S++; c->step = 4; return; }
            if (s == 4) { _w65_set_p(c, c->data); _w65_rd(c, _W65_STACK()); c->S++; c->step = 5; return; }
            if (s == 5) { c->tmp = c->data; _w65_rd(c, _W65_STACK()); c->step = 6; return; }
            c->PC = (uint16_t)(c->tmp | (c->data << 8));
            _W65_DONE();

        case W65_BRK:
            // Software interrupt: signature byte read, PC += 1, B pushed set, vector $FFFE
            c->in_brk = true;
            c->irq_vec = 0xFE;
            c->PC++;
            _w65_rd(c, c->PC);
            c->step = 101;
            return;

        case W65_PUSH:
            if (s == 1) { _w65_rd(c, c->PC); c->step = 2; return; }
            if (s == 2) {
                uint8_t v = (op == 0x08) ? _w65_get_p(c, true) : (op == 0x48) ? c->A : (op == 0x5A) ? c->Y : c->X;   // PHP PHA PHY PHX(DA)
                _w65_wr(c, _W65_STACK(), v); c->S--;
                c->step = 3;
                return;
            }
            _W65_DONE();

        case W65_PULL:
            if (s == 1) { _w65_rd(c, c->PC); c->step = 2; return; }
            if (s == 2) { _w65_rd(c, _W65_STACK()); c->S++; c->step = 3; return; }
            if (s == 3) { _w65_rd(c, _W65_STACK()); c->step = 4; return; }
            switch (op) {
                case 0x28: _w65_set_p(c, c->data); break;                     // PLP
                case 0x68: c->A = c->data; _w65_nz(c, c->A); break;           // PLA
                case 0x7A: c->Y = c->data; _w65_nz(c, c->Y); break;           // PLY
                case 0xFA: c->X = c->data; _w65_nz(c, c->X); break;           // PLX
                default: break;
            }
            _W65_DONE();

        case W65_NOP1:
            _W65_DONE();
        case W65_NOP2_2:
            if (s == 1) { _w65_rd(c, c->PC); c->PC++; c->step = 2; return; }
            _W65_DONE();
        case W65_NOP2_3:
            if (s == 1) { _w65_rd(c, c->PC); c->PC++; c->step = 2; return; }
            if (s == 2) { _w65_rd(c, c->data); c->step = 3; return; }
            _W65_DONE();
        case W65_NOP2_4:
            if (s == 1) { _w65_rd(c, c->PC); c->PC++; c->step = 2; return; }
            if (s == 2) { _w65_rd(c, c->data); c->step = 3; return; }
            if (s == 3) { _w65_rd(c, c->data); c->step = 4; return; }
            _W65_DONE();
        case W65_NOP3_4:
            if (s == 1) { _w65_rd(c, c->PC); c->PC++; c->step = 2; return; }
            if (s == 2) { _w65_rd(c, c->PC); c->PC++; c->step = 3; return; }
            if (s == 3) { _w65_rd(c, c->PC); c->step = 4; return; }
            _W65_DONE();
        case W65_NOP3_8:
            if (s == 1) { _w65_rd(c, c->PC); c->PC++; c->step = 2; return; }
            if (s == 2) { _w65_rd(c, c->PC); c->PC++; c->step = 3; return; }
            if (s < 8) { _w65_rd(c, 0xFFFF); c->step = (uint8_t)(s + 1); return; }
            _W65_DONE();

        case W65_WAI:
            if (s == 1) { _w65_rd(c, c->PC); c->step = 2; return; }
            if (s == 2) { _w65_rd(c, c->PC); c->step = 3; c->waiting = true; return; }
            _W65_DONE();
        case W65_STP:
            if (s == 1) { _w65_rd(c, c->PC); c->step = 2; return; }
            c->stopped = true;
            _w65_rd(c, c->PC);
            return;

        default:
            _W65_DONE();
    }
}

#undef _W65_DONE
#undef _W65_READ_DONE
#undef _W65_FETCH
#undef _W65_STACK

#endif  // CHIPS_IMPL
