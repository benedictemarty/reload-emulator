#pragma once

// bbc_state.h — instantanés du BBC Micro (Model B) : la machine entière, enregistrée et reprise
//
// À inclure après systems/bbc.h, dans le fichier qui en contient l'implémentation (CHIPS_IMPL) :
// le pas d'espionnage se sert de ses fonctions internes.
//
// Registres du processeur (bbc_cpu_capture / bbc_cpu_restore) : sur le Neo6502 le 65C02
// est une vraie puce, ses registres ne se lisent pas. On les lui fait écrire, ou charger,
// par un NMI détourné (méthode du Telestrat, projet Neo6502TeleStrat, validée sur le vrai
// W65C02S) : pendant quelques dizaines de cycles, un pas d'espionnage sert lui-même le bus.
//   - capture : NMI ; le processeur finit son instruction, empile PCH, PCL, P (écritures
//     relevées : PC, P, S), lit le vecteur en $FFFA : on répond $FF00, où l'on sert
//     STA $FF00, STX $FF00, STY $FF00 (écritures relevées : A, X, Y, non faites) puis RTI,
//     qui dépile P et PC de la vraie pile ; le programme reprend où il était ;
//   - restitution (machine déjà chargée) : NMI ; lectures servies sans effet, écritures
//     jetées ; au vecteur, LDX #S-3, TXS, LDA #A, LDX #X, LDY #Y, RTI ; les trois lectures
//     de pile du RTI sont servies (P, PCL, PCH de l'instantané).
// Le même code tourne au banc PC sur le 65C02 émulé, où les registres se lisent : il y est
// testé (tests/bbc/test_bbc_state.c).
//
// Fichier (bbc_state_save / bbc_state_load, par rappels d'écriture et de lecture) : en-tête
// (« BBCSTATE », version, signature des tailles), registres, état de la machine champ par
// champ (pas de pointeurs : valable d'un firmware à l'autre tant que la version ne change
// pas), RAM, RAM latérale, VIA, SN76489, registres du WD1770. Pas enregistrés : les disques
// (ce sont ceux qui sont insérés), les lignes capturées. Refusé : Master 128, Tube, commande
// du WD1770 en cours.
//
// ## Licence zlib/libpng
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
#include <string.h>

#ifndef BBC_COLD
#define BBC_COLD
#endif

typedef struct {
    uint8_t a, x, y, s, p;
    uint16_t pc;
} bbc_regs_t;

#define BBC_STUB     0xFF00
#define BBC_STUB_LEN 12

typedef struct {
    bool restore;      // Restitution (sinon capture)
    int phase;         // 0 : NMI en attente ; 1 : programme ; 2 : terminé
    bbc_regs_t r;
    uint8_t stub[BBC_STUB_LEN];
    uint16_t waddr[3];   // Trois dernières écritures (empilements du NMI)
    uint8_t wdata[3];
    int nwrites;         // Écritures du programme de capture (A, X, Y)
    bool rti;            // RTI lu
    bool ok;
} _bbc_snoop_t;

// Lecture sans effet de bord (restitution : la machine chargée ne change pas)
static inline uint8_t _bbc_quiet_read(bbc_t* sys, uint16_t addr) {
    if ((uint16_t)(addr - 0xFC00) < 0x300) return 0xFF;   // FRED, JIM, SHEILA
    return mem_rd(&sys->mem, addr);
}

// Cycle du processeur pendant l'espionnage (même étirement à 1 MHz que _bbc_tick_cpu)
BBC_COLD static void _bbc_snoop_cpu(bbc_t* sys, _bbc_snoop_t* sn) {
    if (sys->stall) {
        sys->stall--;
        return;
    }
    MOS6502CPU_TICK(&sys->cpu);
    const uint16_t addr = MOS6502CPU_GET_ADDR(&sys->cpu);
    const bool rd = sys->cpu.rw;
    bool served = false;
    if (sn->phase == 0) {
        if (rd && (addr == 0xFFFA || addr == 0xFFFB)) {
            if (addr == 0xFFFA) {
                // Empilements : PCH, PCL, P aux trois dernières écritures
                const bool pushes = sn->waddr[0] >> 8 == 1 && sn->waddr[1] == sn->waddr[0] - 1 &&
                                    sn->waddr[2] == sn->waddr[0] - 2;
                if (!sn->restore) {
                    sn->ok = pushes;
                    sn->r.s = (uint8_t)sn->waddr[0];
                    sn->r.pc = (uint16_t)(sn->wdata[0] << 8 | sn->wdata[1]);
                    sn->r.p = sn->wdata[2];
                }
            } else {
                sn->phase = 1;
                MOS6502CPU_SET_NMI(&sys->cpu, false);
            }
            MOS6502CPU_SET_DATA(&sys->cpu, addr == 0xFFFA ? (uint8_t)BBC_STUB : (uint8_t)(BBC_STUB >> 8));
            served = true;
        } else if (!rd) {
            for (int i = 0; i < 2; i++) sn->waddr[i] = sn->waddr[i + 1], sn->wdata[i] = sn->wdata[i + 1];
            sn->waddr[2] = addr;
            sn->wdata[2] = MOS6502CPU_GET_DATA(&sys->cpu);
            served = sn->restore;   // Restitution : écriture jetée
        } else if (sn->restore) {
            MOS6502CPU_SET_DATA(&sys->cpu, _bbc_quiet_read(sys, addr));
            served = true;
        }
    } else if (sn->phase == 1) {
        const uint8_t s0 = sn->r.s;   // Pile d'avant le NMI : P en S-2, PCL en S-1, PCH en S
        if (rd && addr >= BBC_STUB && addr < BBC_STUB + BBC_STUB_LEN) {
            if (addr - BBC_STUB == 9) sn->rti = true;   // RTI (même place dans les deux programmes)
            MOS6502CPU_SET_DATA(&sys->cpu, sn->stub[addr - BBC_STUB]);
            served = true;
        } else if (!rd && addr >= BBC_STUB) {
            // Capture : STA, STX, STY
            const uint8_t d = MOS6502CPU_GET_DATA(&sys->cpu);
            if (sn->nwrites == 0) sn->r.a = d;
            else if (sn->nwrites == 1) sn->r.x = d;
            else if (sn->nwrites == 2) sn->r.y = d;
            sn->nwrites++;
            served = true;
        } else if (sn->restore) {
            // Pile servie : P, PCL, PCH de l'instantané ; le reste : jeté
            uint8_t d = 0xFF;
            if (addr == 0x100 + (uint8_t)(s0 - 2)) d = sn->r.p;
            else if (addr == 0x100 + (uint8_t)(s0 - 1)) d = (uint8_t)sn->r.pc;
            else if (addr == 0x100 + s0) d = (uint8_t)(sn->r.pc >> 8);
            if (rd) MOS6502CPU_SET_DATA(&sys->cpu, d);
            served = true;
        }
        if (rd && sn->rti && addr == 0x100 + s0) {
            sn->phase = 2;   // PCH dépilé : le programme reprend au cycle suivant
            sn->ok = sn->restore ? true : (sn->ok && sn->nwrites == 3);
        }
    }
    if (!served) {
        if ((uint16_t)(addr - 0xFC00) < 0x300 && _bbc_is_1mhz(addr)) {
            sys->stall = (uint8_t)((sys->system_ticks & 1) ? 1 : 2);
        }
        _bbc_mem_rw(sys, addr, rd);
    }
}

// Pas complet pendant la capture (la machine continue : même travail que bbc_tick) ;
// restitution : seul le processeur avance, la machine chargée ne bouge pas
BBC_COLD static void _bbc_tick_snoop(bbc_t* sys, _bbc_snoop_t* sn) {
    if (sn->restore) {
        _bbc_snoop_cpu(sys, sn);
        return;
    }
    _bbc_crtc_sync(sys);
    _bbc_snoop_cpu(sys, sn);
    _bbc_tick_crtc(sys, sys->system_ticks & 1);
    if ((sys->system_ticks & 3) == 3) _bbc_tick_1mhz(sys);
    if ((sys->system_ticks & 7) == 7) _bbc_sn_tick(sys);
    sys->system_ticks++;
}

static inline bool _bbc_snoop_run(bbc_t* sys, _bbc_snoop_t* sn) {
    MOS6502CPU_SET_NMI(&sys->cpu, false);   // Un front, même après un NMI resté bas
    MOS6502CPU_SET_NMI(&sys->cpu, true);
    for (int i = 0; i < 200 && sn->phase != 2; i++) _bbc_tick_snoop(sys, sn);
    MOS6502CPU_SET_NMI(&sys->cpu, false);
    sys->nmi_pin = false;
#ifdef BBC_STATE_DEBUG
    fprintf(stderr, "snoop: restore=%d phase=%d ok=%d nwrites=%d rti=%d s=%02X pc=%04X p=%02X a=%02X x=%02X y=%02X\n", sn->restore,
            sn->phase, sn->ok, sn->nwrites, sn->rti, sn->r.s, sn->r.pc, sn->r.p, sn->r.a, sn->r.x, sn->r.y);
#endif
    return sn->phase == 2 && sn->ok;
}

// Capture des registres (le programme reprend aussitôt) ; false : NMI non pris, ou
// machine dans un état non pris en charge (WD1770 en cours : son NMI)
BBC_COLD bool bbc_cpu_capture(bbc_t* sys, bbc_regs_t* r) {
    if (sys->fdc.state != WD1770_IDLE || sys->nmi_pin || sys->tube_enabled) return false;
    static const uint8_t stub[BBC_STUB_LEN] = {0x8D, 0x00, 0xFF, 0x8E, 0x00, 0xFF, 0x8C, 0x00, 0xFF, 0x40, 0xEA, 0xEA};
    _bbc_snoop_t sn;
    memset(&sn, 0, sizeof(sn));
    memcpy(sn.stub, stub, sizeof(stub));
    const bool ok = _bbc_snoop_run(sys, &sn);
    if (r) *r = sn.r;
    return ok;
}

// Restitution des registres (machine déjà chargée : RAM, périphériques)
BBC_COLD bool bbc_cpu_restore(bbc_t* sys, const bbc_regs_t* r) {
    _bbc_snoop_t sn;
    memset(&sn, 0, sizeof(sn));
    sn.restore = true;
    sn.r = *r;
    const uint8_t stub[BBC_STUB_LEN] = {0xA2, (uint8_t)(r->s - 3), 0x9A, 0xA9, r->a, 0xA2, r->x, 0xA0, r->y, 0x40, 0xEA, 0xEA};
    memcpy(sn.stub, stub, sizeof(stub));
    sys->stall = 0;
    return _bbc_snoop_run(sys, &sn);
}

/*-- Fichier ------------------------------------------------------------------*/

#define BBC_STATE_MAGIC   "BBCSTATE"
#define BBC_STATE_VERSION 1

typedef bool (*bbc_state_write_t)(void* ctx, const void* data, uint32_t len);
typedef bool (*bbc_state_read_t)(void* ctx, void* data, uint32_t len);

// Champs de la machine (types fixes : le fichier ne dépend pas de l'agencement de bbc_t)
typedef struct {
    uint32_t system_ticks, reserved, adc_timer, field_count;
    int32_t display_y;
    uint16_t ma, ma_row_start, adc_value, adc_channel[4], last_pc;
    uint8_t stall, irq_pin, nmi_pin, any_key, kbd_ca2, hcc, tt_bottom_row, ula_ctrl, vsync, nmi, adc_status;
    uint8_t crtc_reg[18], crtc_addr, vcc, rc, adjust, in_adjust, vsync_count, field, vsync_seen, frame_done;
    uint8_t romsel, ic32, sysvia_pb_old, key_cols[16], key_scan_column, ula_pal[16], joystick_fire;
    uint8_t swr_banks;   // Banques de RAM latérale qui suivent (bit n : banque n)
} bbc_state_core_t;

// Registres du WD1770 (au repos : pas de commande en cours)
typedef struct {
    uint8_t status, track, sector, data, command, drq, intrq, control, step_in, motor;
    int8_t drive, side;
    uint8_t phys_track[2];
} bbc_state_fdc_t;

typedef struct {
    char magic[8];
    uint16_t version;
    uint16_t model;
    uint32_t sizes;   // Signature : tailles des blocs enregistrés tels quels
} bbc_state_header_t;

static inline uint32_t _bbc_state_sizes(void) {
    return (uint32_t)sizeof(bbc_state_core_t) | (uint32_t)sizeof(mos6522via_t) << 8 | (uint32_t)sizeof(bbc_sn76489_t) << 16 |
           (uint32_t)sizeof(bbc_state_fdc_t) << 24;
}

// Enregistre la machine (registres capturés : le programme continue). Refusé : Master,
// Tube, WD1770 en cours de commande, NMI non pris.
BBC_COLD bool bbc_state_save(bbc_t* sys, bbc_state_write_t wr, void* ctx) {
    if (sys->model != BBC_MODEL_B || sys->tube_enabled || sys->fdc.state != WD1770_IDLE) return false;
    bbc_regs_t r;
    if (!bbc_cpu_capture(sys, &r)) return false;
    _bbc_crtc_sync(sys);
    bbc_state_header_t h;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, BBC_STATE_MAGIC, 8);
    h.version = BBC_STATE_VERSION;
    h.model = (uint16_t)sys->model;
    h.sizes = _bbc_state_sizes();
    bbc_state_core_t c;
    memset(&c, 0, sizeof(c));
    c.system_ticks = sys->system_ticks;
    c.adc_timer = sys->adc_timer;
    c.field_count = sys->field_count;
    c.display_y = sys->display_y;
    c.ma = sys->ma;
    c.ma_row_start = sys->ma_row_start;
    c.adc_value = sys->adc_value;
    memcpy(c.adc_channel, sys->adc_channel, sizeof(c.adc_channel));
    c.last_pc = sys->last_pc;
    c.stall = sys->stall;
    c.irq_pin = sys->irq_pin;
    c.nmi_pin = sys->nmi_pin;
    c.any_key = sys->any_key;
    c.kbd_ca2 = sys->kbd_ca2;
    c.hcc = sys->hcc;
    c.tt_bottom_row = sys->tt_bottom_row;
    c.ula_ctrl = sys->ula_ctrl;
    c.vsync = sys->vsync;
    c.nmi = sys->nmi;
    c.adc_status = sys->adc_status;
    memcpy(c.crtc_reg, sys->crtc_reg, sizeof(c.crtc_reg));
    c.crtc_addr = sys->crtc_addr;
    c.vcc = sys->vcc;
    c.rc = sys->rc;
    c.adjust = sys->adjust;
    c.in_adjust = sys->in_adjust;
    c.vsync_count = sys->vsync_count;
    c.field = sys->field;
    c.vsync_seen = sys->vsync_seen;
    c.frame_done = sys->frame_done;
    c.romsel = sys->romsel;
    c.ic32 = sys->ic32;
    c.sysvia_pb_old = sys->sysvia_pb_old;
    memcpy(c.key_cols, sys->key_cols, sizeof(c.key_cols));
    c.key_scan_column = sys->key_scan_column;
    memcpy(c.ula_pal, sys->ula_pal, sizeof(c.ula_pal));
    c.joystick_fire = sys->joystick_fire;
    for (int b = 0; b < BBC_NUM_ROM_BANKS && b < 8; b++) {
        if (sys->ram_bank[b]) c.swr_banks |= (uint8_t)(1u << b);
    }
    const wd1770_t* f = &sys->fdc;
    bbc_state_fdc_t fd = {f->status, f->track, f->sector, f->data, f->command, f->drq, f->intrq, f->control,
                          f->step_in, f->motor, (int8_t)f->drive, (int8_t)f->side, {f->phys_track[0], f->phys_track[1]}};
    bool ok = wr(ctx, &h, sizeof(h)) && wr(ctx, &r, sizeof(r)) && wr(ctx, &c, sizeof(c)) && wr(ctx, sys->ram, sizeof(sys->ram));
    for (int b = 0; ok && b < 8; b++) {
        if (c.swr_banks & (1u << b)) ok = wr(ctx, sys->ram_bank[b], 0x4000);
    }
    return ok && wr(ctx, &sys->sysvia, sizeof(sys->sysvia)) && wr(ctx, &sys->uservia, sizeof(sys->uservia)) &&
           wr(ctx, &sys->sn, sizeof(sys->sn)) && wr(ctx, &fd, sizeof(fd));
}

// Vérifie l'en-tête d'un instantané (lu par rd) ; false : pas un instantané de cette version
BBC_COLD bool bbc_state_check(bbc_state_read_t rd, void* ctx) {
    bbc_state_header_t h;
    return rd(ctx, &h, sizeof(h)) && !memcmp(h.magic, BBC_STATE_MAGIC, 8) && h.version == BBC_STATE_VERSION &&
           h.model == BBC_MODEL_B && h.sizes == _bbc_state_sizes();
}

// Reprend la machine depuis un instantané (lu par rd, en-tête compris). En cas d'échec
// après la lecture de l'en-tête, la machine est dans un état mélangé : la redémarrer.
// Les disques insérés restent ceux du moment.
BBC_COLD bool bbc_state_load(bbc_t* sys, bbc_state_read_t rd, void* ctx) {
    if (sys->model != BBC_MODEL_B || sys->tube_enabled) return false;
    if (!bbc_state_check(rd, ctx)) return false;
    bbc_regs_t r;
    bbc_state_core_t c;
    if (!rd(ctx, &r, sizeof(r)) || !rd(ctx, &c, sizeof(c)) || !rd(ctx, sys->ram, sizeof(sys->ram))) return false;
    for (int b = 0; b < 8; b++) {
        if (!(c.swr_banks & (1u << b))) continue;
        if (b >= BBC_NUM_ROM_BANKS || !sys->ram_bank[b]) return false;   // Pas de RAM latérale à cette place
        if (!rd(ctx, sys->ram_bank[b], 0x4000)) return false;
    }
    bbc_state_fdc_t fd;
    // Cadence des échantillons : celle de la plate-forme qui reprend (pas celle de l'instantané)
    const int32_t sample_period = sys->sn.sample_period, sample_accum = sys->sn.sample_accum;
    if (!rd(ctx, &sys->sysvia, sizeof(sys->sysvia)) || !rd(ctx, &sys->uservia, sizeof(sys->uservia)) ||
        !rd(ctx, &sys->sn, sizeof(sys->sn)) || !rd(ctx, &fd, sizeof(fd))) {
        return false;
    }
    sys->sn.sample_period = sample_period;
    sys->sn.sample_accum = sample_accum;
    sys->system_ticks = c.system_ticks;
    sys->adc_timer = c.adc_timer;
    sys->field_count = c.field_count;
    sys->display_y = c.display_y;
    sys->ma = c.ma;
    sys->ma_row_start = c.ma_row_start;
    sys->adc_value = c.adc_value;
    memcpy(sys->adc_channel, c.adc_channel, sizeof(c.adc_channel));
    sys->last_pc = c.last_pc;
    sys->stall = 0;
    sys->irq_pin = c.irq_pin;
    sys->nmi_pin = false;
    sys->any_key = false;   // Les touches sont celles du moment : aucune n'est reprise enfoncée
    sys->kbd_ca2 = c.kbd_ca2;
    sys->hcc = c.hcc;
    sys->tt_bottom_row = c.tt_bottom_row;
    sys->ula_ctrl = c.ula_ctrl;
    sys->vsync = c.vsync;
    sys->nmi = false;
    sys->adc_status = c.adc_status;
    memcpy(sys->crtc_reg, c.crtc_reg, sizeof(c.crtc_reg));
    sys->crtc_addr = c.crtc_addr;
    sys->vcc = c.vcc;
    sys->rc = c.rc;
    sys->adjust = c.adjust;
    sys->in_adjust = c.in_adjust;
    sys->vsync_count = c.vsync_count;
    sys->field = c.field;
    sys->vsync_seen = c.vsync_seen;
    sys->frame_done = c.frame_done;
    sys->ic32 = c.ic32;
    sys->sysvia_pb_old = c.sysvia_pb_old;
    memset(sys->key_cols, 0, sizeof(sys->key_cols));
    sys->key_scan_column = c.key_scan_column;
    memcpy(sys->ula_pal, c.ula_pal, sizeof(c.ula_pal));
    sys->joystick_fire = c.joystick_fire;
    // État dérivé : CRTC exact, aucune écriture ULA en attente sur la ligne, carte mémoire
    sys->crtc_lazy = false;
    sys->ula_event_count = 0;
    sys->ula_ctrl_start = sys->ula_ctrl;
    memcpy(sys->ula_pal_start, sys->ula_pal, 16);
    _bbc_set_romsel(sys, c.romsel);
    // WD1770 au repos, disques en place gardés
    wd1770_t* f = &sys->fdc;
    f->status = fd.status;
    f->track = fd.track;
    f->sector = fd.sector;
    f->data = fd.data;
    f->command = fd.command;
    f->drq = fd.drq;
    f->intrq = fd.intrq;
    f->control = fd.control;
    f->step_in = fd.step_in;
    f->motor = fd.motor;
    f->drive = fd.drive;
    f->side = fd.side;
    f->phys_track[0] = fd.phys_track[0];
    f->phys_track[1] = fd.phys_track[1];
    f->state = WD1770_IDLE;
    MOS6502CPU_SET_IRQ(&sys->cpu, false);   // Le RTI de la restitution ne doit pas être interrompu
    const bool ok = bbc_cpu_restore(sys, &r);
    MOS6502CPU_SET_IRQ(&sys->cpu, sys->irq_pin);
    return ok;
}
