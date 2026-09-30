#pragma once

// oric_tape_rec.h — enregistreur de cassette : CSAVE vers un fichier .tap
//
// La ROM BASIC 1.1 écrit la bande sur PB7 du VIA 1 (routine $E65E, sortie du
// timer 1). Mesuré au banc : une période (entre deux fronts montants) de 432
// cycles pour un bit 1, de 640 pour un bit 0 ; trame de 13 bits : 0 (départ),
// 8 bits de données (bit 0 d'abord), parité, 1, 1, 1. D'abord ~260 octets de
// synchro ($16), puis $24, 9 octets d'en-tête, le nom terminé par 0, les
// données (de l'adresse de début à celle de fin, incluse).
//
// Sortie : un fichier par enregistrement, NOM.TAP (nom de l'en-tête, lettres,
// chiffres, - et _ gardés), écrit au format .tap usuel : 3 octets de synchro,
// $24, en-tête, nom, 0, données. Fermé après le dernier octet ou à l'arrêt du
// moteur.
//
// Indépendant de la plate-forme (testé dans tests/test_telestrat.c).
//
// Composant commun à reload-emulator (Oric) et Neo6502TeleStrat, repris du Telestrat
// (fusion : docs/FUSION-ORIC-TELESTRAT.md de reload).
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

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define ORIC_TAPE_REC_THRESHOLD 536   // cycles : en dessous, bit 1 (432) ; au-dessus, bit 0 (640)
#define ORIC_TAPE_REC_MAXPERIOD 2000  // au-delà : silence, trame abandonnée

typedef struct {
    bool (*open)(void* ctx, const char* name);  // nouveau fichier NOM.TAP
    void (*write)(void* ctx, const uint8_t* data, uint32_t len);
    void (*close)(void* ctx);
    void* ctx;
} oric_tape_rec_out_t;

typedef enum { ORIC_REC_SYNC = 0, ORIC_REC_HEADER, ORIC_REC_NAME, ORIC_REC_DATA } oric_tape_rec_phase_t;

typedef struct {
    oric_tape_rec_out_t out;
    bool enabled;
    // Bits
    uint8_t level;
    bool have_rise;
    uint32_t last_rise;
    uint16_t hist;    // 13 derniers bits (le plus récent en bit 0)
    bool locked;      // calé sur les trames (après une synchro $16 reconnue)
    int nbits;        // bits de la trame en cours (0 : entre deux trames)
    uint16_t shift;
    uint8_t parity;
    uint32_t bad;     // trames rejetées (parité, arrêt)
    // Octets
    oric_tape_rec_phase_t phase;
    uint8_t header[9];
    int header_n;
    char name[17];
    int name_n;
    uint32_t remaining;  // octets de données restants
    bool file_open;
    uint32_t written;    // octets de données écrits
    char file[24];       // nom du fichier en cours ou dernier écrit
} oric_tape_rec_t;

static inline void oric_tape_rec_init(oric_tape_rec_t* r, const oric_tape_rec_out_t* out) {
    memset(r, 0, sizeof(*r));
    if (out) {
        r->out = *out;
        r->enabled = out->open && out->write && out->close;
    }
}

static inline void _oric_tape_rec_close(oric_tape_rec_t* r) {
    if (r->file_open) r->out.close(r->out.ctx);
    r->file_open = false;
    r->phase = ORIC_REC_SYNC;
    r->header_n = r->name_n = 0;
}

// Octet reçu
static inline void _oric_tape_rec_byte(oric_tape_rec_t* r, uint8_t b) {
    switch (r->phase) {
        case ORIC_REC_SYNC:
            if (b == 0x24) r->phase = ORIC_REC_HEADER, r->header_n = 0;
            break;
        case ORIC_REC_HEADER:
            r->header[r->header_n++] = b;
            if (r->header_n == 9) r->phase = ORIC_REC_NAME, r->name_n = 0;
            break;
        case ORIC_REC_NAME:
            if (b != 0 && r->name_n < 16) {
                r->name[r->name_n++] = (char)b;
                break;
            }
            if (b != 0) break;  // nom tronqué à 16
            r->name[r->name_n] = 0;
            {
                // Fichier : lettres, chiffres, - et _ du nom ; « SANSNOM » sinon
                char base[17];
                int n = 0;
                for (int i = 0; i < r->name_n; i++) {
                    char c = r->name[i];
                    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
                    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') base[n++] = c;
                }
                base[n] = 0;
                memcpy(r->file, n ? base : "SANSNOM", (size_t)(n ? n : 7) + 1);
                strcat(r->file, ".TAP");
                const uint32_t end = (uint32_t)r->header[4] << 8 | r->header[5];
                const uint32_t start = (uint32_t)r->header[6] << 8 | r->header[7];
                r->remaining = end >= start ? end - start + 1 : 0;
                r->written = 0;
                r->file_open = r->out.open(r->out.ctx, r->file);
                if (r->file_open) {
                    static const uint8_t sync[4] = {0x16, 0x16, 0x16, 0x24};
                    r->out.write(r->out.ctx, sync, 4);
                    r->out.write(r->out.ctx, r->header, 9);
                    r->out.write(r->out.ctx, (const uint8_t*)r->name, (uint32_t)r->name_n + 1);
                }
                r->phase = ORIC_REC_DATA;
                if (!r->remaining) _oric_tape_rec_close(r);
            }
            break;
        case ORIC_REC_DATA:
            if (r->file_open) r->out.write(r->out.ctx, &b, 1);
            r->written++;
            if (--r->remaining == 0) _oric_tape_rec_close(r);
            break;
    }
}

// Trame de 13 bits d'un octet, dans l'ordre d'émission (premier bit en haut)
static inline uint16_t _oric_tape_rec_frame(uint8_t b) {
    int ones = 0;
    uint16_t f = 0;  // départ 0
    for (int i = 0; i < 8; i++) {
        f = (uint16_t)(f << 1 | ((b >> i) & 1));
        ones += (b >> i) & 1;
    }
    f = (uint16_t)(f << 1 | ((ones & 1) ^ 1));  // parité
    return (uint16_t)(f << 3 | 7);              // arrêt
}

// Bit reçu. Calage : les 13 derniers bits forment la trame d'une synchro
// ($16) ; ensuite les trames se comptent (0 après un 1, 8 bits, parité, 1 ;
// les 1 de plus sont des arrêts). Trame fausse (parité, arrêt) : calage perdu
// pendant la synchro, octet perdu après.
static inline void _oric_tape_rec_bit(oric_tape_rec_t* r, int bit) {
    r->hist = (uint16_t)(((r->hist << 1) | (unsigned)bit) & 0x1FFF);
    if (!r->locked) {
        if (r->hist == _oric_tape_rec_frame(0x16)) {
            r->locked = true;
            r->nbits = 0;
            _oric_tape_rec_byte(r, 0x16);
        }
        return;
    }
    if (r->nbits == 0) {
        // Départ : un 0 après un 1 (arrêt de la trame précédente)
        if (bit == 0 && (r->hist & 2)) {
            r->nbits = 1;
            r->shift = 0;
        }
        return;
    }
    // nbits 1 à 8 : données ; 9 : parité ; 10 : premier arrêt
    if (r->nbits <= 8) r->shift |= (uint16_t)(bit << (r->nbits - 1));
    else if (r->nbits == 9) r->parity = (uint8_t)bit;
    r->nbits++;
    if (r->nbits == 11) {
        int ones = 0;
        for (int i = 0; i < 8; i++) ones += (r->shift >> i) & 1;
        // Parité ($E65E : 1 si le nombre de 1 est pair) et arrêt
        if (bit == 1 && r->parity == ((ones & 1) ^ 1)) {
            _oric_tape_rec_byte(r, (uint8_t)r->shift);
        } else {
            // Trame fausse : avant l'en-tête, calage perdu ; ensuite l'octet
            // est perdu mais la suite est gardée
            r->bad++;
            if (r->phase == ORIC_REC_SYNC) r->locked = false;
        }
        r->nbits = 0;
    }
}

// Niveau de PB7 au cycle t (appelé à chaque pas, moteur en marche)
static inline void oric_tape_rec_level(oric_tape_rec_t* r, uint8_t level, uint32_t t) {
    if (!r->enabled || level == r->level) return;
    r->level = level;
    if (!level) return;
    if (r->have_rise) {
        const uint32_t period = t - r->last_rise;
        if (period > ORIC_TAPE_REC_MAXPERIOD) {
            r->nbits = 0;
            r->locked = false;
        } else {
            _oric_tape_rec_bit(r, period < ORIC_TAPE_REC_THRESHOLD);
        }
    }
    r->have_rise = true;
    r->last_rise = t;
}

// Moteur arrêté : fichier fermé (enregistrement interrompu : tel quel)
static inline void oric_tape_rec_motor_off(oric_tape_rec_t* r) {
    _oric_tape_rec_close(r);
    r->have_rise = false;
    r->nbits = 0;
    r->locked = false;
}

static inline bool oric_tape_rec_active(const oric_tape_rec_t* r) { return r->file_open; }
