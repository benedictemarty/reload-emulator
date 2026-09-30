#pragma once

// oric_tape.h — lecteur de cassette (.tap) de l'Oric / du Telestrat
//
// Signal de la bande sur CB1 du VIA 1, moteur commandé par PB6 (câblage de
// l'Atmos, repris par le Telestrat). Événements : la plate-forme appelle
// oric_tape_toggle() quand oric_tape_next() cycles se sont écoulés depuis la
// bascule précédente ; le signal change alors de niveau.
//
// Format du signal (faits relevés dans la documentation et les constantes
// d'Oricutron, tape.h : TAPE_1_PULSE = 208, TAPE_0_PULSE = 416) :
//   - un bit = une alternance haute puis une basse (il commence sur un front
//     montant), de 208 cycles chacune pour 1, de 416 pour 0 ;
//   - un octet = 14 bits : 1, 0, les 8 bits de données (bit 0 d'abord), la
//     parité (1 si le nombre de 1 est pair), puis 1, 1, 1 ;
//   - moteur démarré sur un octet de synchro ($16) : 80 octets de synchro de
//     plus, pour laisser la ROM se caler ;
//   - après un en-tête ($16…, $24, 9 octets, nom terminé par 0) : un silence
//     d'environ 1281 cycles d'alternances courtes, le temps que la ROM traite
//     l'en-tête ;
//   - fin de bande : deux alternances, puis le signal ne bouge plus.
// Implémentation propre au projet (pas de code repris d'Oricutron, GPL).
//
// Octets lus par un rappel (image en mémoire, fichier de la clé) dans une
// fenêtre de 256 octets ; oric_tape_insert_mem pour une image en mémoire.
//
// Dernier fichier tronqué (au plus 256 octets manquent d'après son en-tête) :
// complété par des octets $00, pour que la ROM finisse de le charger.
//
// Composant commun à reload-emulator (Oric) et Neo6502TeleStrat (fusion :
// docs/FUSION-ORIC-TELESTRAT.md de reload), repris du Telestrat.
//
// Lecture accélérée (oric_tape_turbo.h) : la ROM patchée demande la synchro
// (oric_tape_turbo_sync) puis les octets un à un (oric_tape_turbo_byte) ; le
// signal s'arrête alors (turbo_hold) jusqu'à l'arrêt du moteur, pour que les
// deux lectures ne se disputent pas la bande.
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

#define ORIC_TAPE_SHORT     208   // alternance d'un bit 1 (cycles)
#define ORIC_TAPE_LONG      416   // alternance d'un bit 0
#define ORIC_TAPE_SYNC_MORE 80    // octets de synchro ajoutés
#define ORIC_TAPE_GAP       1281  // silence après un en-tête (cycles)
#define ORIC_TAPE_WINDOW    256

typedef bool (*oric_tape_read_t)(void* ctx, uint32_t offset, uint8_t* buf, uint32_t len);

typedef enum {
    ORIC_TAPE_DATA = 0,  // octets de l'image
    ORIC_TAPE_GAP_RUN,   // silence après un en-tête
    ORIC_TAPE_TAIL,      // deux alternances de fin
    ORIC_TAPE_STOPPED,   // au bout : plus de bascule
} oric_tape_phase_t;

typedef struct {
    bool inserted;
    bool motor;
    uint8_t level;        // niveau du signal (CB1)
    uint32_t len;         // taille de l'image
    uint32_t pad;         // octets $00 ajoutés après l'image (dernier fichier tronqué)
    uint32_t pos;         // octet en cours
    uint16_t frame;       // 14 bits de l'octet en cours (bit 0 émis d'abord)
    uint8_t frame_bit;    // bit en cours dans la trame (0-13)
    bool frame_ready;
    bool started;         // premier front montant passé
    int extra_sync;       // octets de synchro encore à ajouter
    uint32_t header_end;  // position qui suit l'en-tête (0 : aucune)
    int gap_left;         // cycles de silence restants
    int tail_left;        // alternances de fin restantes
    oric_tape_phase_t phase;
    int next;             // cycles avant la prochaine bascule
    bool turbo_hold;      // lecture accélérée en cours : signal arrêté
    oric_tape_read_t read;
    void* ctx;
    uint8_t win[ORIC_TAPE_WINDOW];
    uint32_t win_start, win_len;
} oric_tape_t;

static inline int _oric_tape_byte(oric_tape_t* t, uint32_t off) {
    if (off >= t->len) return -1;
    if (off < t->win_start || off >= t->win_start + t->win_len) {
        const uint32_t n = t->len - off < ORIC_TAPE_WINDOW ? t->len - off : ORIC_TAPE_WINDOW;
        if (!t->read || !t->read(t->ctx, off, t->win, n)) return -1;
        t->win_start = off;
        t->win_len = n;
    }
    return t->win[off - t->win_start];
}

// Trame de 14 bits d'un octet
static inline uint16_t _oric_tape_frame(uint8_t b) {
    int ones = 0;
    for (int i = 0; i < 8; i++) ones += (b >> i) & 1;
    const uint16_t parity = (ones & 1) ? 0 : 1;
    // bits : 0 = 1 (début), 1 = 0 (synchro), 2-9 données, 10 parité, 11-13 = 1
    return (uint16_t)(1u | ((uint16_t)b << 2) | (parity << 10) | (7u << 11));
}

// Position qui suit l'en-tête commençant en `at` (synchro $16…, $24, 9
// octets, nom terminé par 0), 0 si ce n'en est pas un
static inline uint32_t _oric_tape_header_end(oric_tape_t* t, uint32_t at) {
    uint32_t i = at;
    while (_oric_tape_byte(t, i) == 0x16) i++;
    if (i - at < 3 || _oric_tape_byte(t, i) != 0x24) return 0;
    // En-tête : 2 octets, type, lancement, fin (poids fort, faible), début, 0
    const int eh = _oric_tape_byte(t, i + 5), el = _oric_tape_byte(t, i + 6);
    const int sh = _oric_tape_byte(t, i + 7), sl = _oric_tape_byte(t, i + 8);
    i += 1 + 9;
    int b;
    while ((b = _oric_tape_byte(t, i)) > 0) i++;
    if (b != 0) return 0;
    i++;
    // Données tronquées de 256 octets au plus : complétées par des $00
    if (eh >= 0 && el >= 0 && sh >= 0 && sl >= 0) {
        const uint32_t end = (uint32_t)(eh << 8 | el), start = (uint32_t)(sh << 8 | sl);
        const uint32_t size = end >= start ? end - start + 1 : 0;
        if (i + size > t->len && i + size - t->len <= 256) t->pad = i + size - t->len;
    }
    return i;
}

// Fin de la bande, octets de remplissage compris
static inline uint32_t _oric_tape_end(const oric_tape_t* t) { return t->len + t->pad; }

// Durée de l'alternance en cours
static inline int _oric_tape_half(oric_tape_t* t) {
    if (!t->frame_ready) {
        const int b = t->extra_sync > 0 ? 0x16 : _oric_tape_byte(t, t->pos);
        t->frame = _oric_tape_frame((uint8_t)(b < 0 ? 0 : b));
        t->frame_bit = 0;
        t->frame_ready = true;
    }
    return ((t->frame >> t->frame_bit) & 1) ? ORIC_TAPE_SHORT : ORIC_TAPE_LONG;
}

// Bit suivant (au front montant) : fin d'octet, d'en-tête, de bande
static inline void _oric_tape_advance(oric_tape_t* t) {
    if (++t->frame_bit < 14) return;
    t->frame_ready = false;
    if (t->extra_sync > 0) {
        t->extra_sync--;
        return;
    }
    t->pos++;
    if (t->header_end && t->pos == t->header_end) {
        t->header_end = 0;
        t->phase = ORIC_TAPE_GAP_RUN;
        t->gap_left = ORIC_TAPE_GAP;
    } else if (t->pos >= _oric_tape_end(t)) {
        t->phase = ORIC_TAPE_TAIL;
        t->tail_left = 2;
    }
}

static inline void _oric_tape_schedule(oric_tape_t* t) {
    switch (t->phase) {
        case ORIC_TAPE_DATA: t->next = _oric_tape_half(t); break;
        case ORIC_TAPE_GAP_RUN:
        case ORIC_TAPE_TAIL: t->next = ORIC_TAPE_SHORT; break;
        default: t->next = 0; break;
    }
}

static inline void oric_tape_init(oric_tape_t* t) { memset(t, 0, sizeof(*t)); }

static inline void oric_tape_rewind(oric_tape_t* t) {
    t->pos = 0;
    t->pad = 0;
    t->started = false;
    t->frame_ready = false;
    t->extra_sync = 0;
    t->header_end = 0;
    t->level = 0;
    t->phase = t->len ? ORIC_TAPE_DATA : ORIC_TAPE_STOPPED;
    _oric_tape_schedule(t);
}

// Image de len octets lue par read(ctx, …), rembobinée
static inline void oric_tape_insert(oric_tape_t* t, uint32_t len, oric_tape_read_t read, void* ctx) {
    const bool motor = t->motor;
    oric_tape_init(t);
    t->motor = motor;
    t->inserted = len > 0 && read != NULL;
    t->len = t->inserted ? len : 0;
    t->read = read;
    t->ctx = ctx;
    oric_tape_rewind(t);
}

static inline bool _oric_tape_mem_read(void* ctx, uint32_t offset, uint8_t* buf, uint32_t len) {
    memcpy(buf, (const uint8_t*)ctx + offset, len);
    return true;
}

// Image en mémoire (ou en flash), rembobinée
static inline void oric_tape_insert_mem(oric_tape_t* t, const uint8_t* data, uint32_t len) {
    oric_tape_insert(t, data ? len : 0, _oric_tape_mem_read, (void*)(uintptr_t)data);
}

static inline void oric_tape_eject(oric_tape_t* t) {
    const bool motor = t->motor;
    oric_tape_init(t);
    t->motor = motor;
    t->phase = ORIC_TAPE_STOPPED;
}

static inline bool oric_tape_running(const oric_tape_t* t) {
    return t->inserted && t->motor && !t->turbo_hold && t->phase != ORIC_TAPE_STOPPED;
}

// Moteur (PB6 du VIA 1). Arrêt au milieu d'un octet : on reprendra à l'octet
// suivant ; démarrage sur une synchro : synchro prolongée, fin d'en-tête notée
static inline void oric_tape_set_motor(oric_tape_t* t, bool on) {
    if (on == t->motor) return;
    t->motor = on;
    if (!on && t->turbo_hold) {
        // Fin d'une lecture accélérée : la bande reste où la ROM s'est arrêtée
        t->turbo_hold = false;
        t->level = 0;
        _oric_tape_schedule(t);
        return;
    }
    if (!t->inserted || t->phase != ORIC_TAPE_DATA) return;
    if (!on) {
        if (t->frame_ready && t->started && t->extra_sync == 0) t->pos++;
        t->frame_ready = false;
        t->started = false;
        t->extra_sync = 0;
        if (t->pos >= _oric_tape_end(t)) {
            t->phase = ORIC_TAPE_TAIL;
            t->tail_left = 2;
        }
    } else if (_oric_tape_byte(t, t->pos) == 0x16) {
        t->header_end = _oric_tape_header_end(t, t->pos);
        if (t->header_end) t->extra_sync = ORIC_TAPE_SYNC_MORE;
    }
    _oric_tape_schedule(t);
    if (on && t->level == 0) t->next = 4;  // premier front montant : aussitôt
}

// Cycles avant la prochaine bascule (si oric_tape_running)
static inline int oric_tape_next(const oric_tape_t* t) { return t->next; }

// Bascule : nouveau niveau, puis l'alternance suivante est préparée
static inline uint8_t oric_tape_toggle(oric_tape_t* t) {
    t->level ^= 1;
    switch (t->phase) {
        case ORIC_TAPE_DATA:
            // Front montant : début du bit suivant (le premier : bit 0 de la trame)
            if (t->level) {
                if (t->started && t->frame_ready) _oric_tape_advance(t);
                t->started = true;
            }
            break;
        case ORIC_TAPE_GAP_RUN:
            t->gap_left -= t->next;
            if (t->gap_left <= 0) t->phase = t->pos >= _oric_tape_end(t) ? ORIC_TAPE_TAIL : ORIC_TAPE_DATA;
            break;
        case ORIC_TAPE_TAIL:
            if (--t->tail_left <= 0) t->phase = ORIC_TAPE_STOPPED;
            break;
        default: break;
    }
    _oric_tape_schedule(t);
    return t->level;
}

static inline int oric_tape_percent(const oric_tape_t* t) {
    return t->len ? (int)((uint64_t)(t->pos < t->len ? t->pos : t->len) * 100 / t->len) : 0;
}

/*-- Lecture accélérée ---------------------------------------------------------*/

// Signal arrêté ; la lecture reprend à l'octet pos
static inline void _oric_tape_turbo_hold(oric_tape_t* t) {
    t->turbo_hold = true;
    t->frame_ready = false;
    t->started = false;
    t->extra_sync = 0;
    t->header_end = 0;
    t->phase = t->pos < t->len ? ORIC_TAPE_DATA : ORIC_TAPE_STOPPED;
}

// Synchro : bande placée après la prochaine suite d'au moins 3 octets $16.
// false : pas de cassette, moteur arrêté, ou plus de synchro jusqu'au bout.
static inline bool oric_tape_turbo_sync(oric_tape_t* t) {
    if (!t->inserted || !t->motor) return false;
    // Octet en cours de lecture par le signal : on repart du suivant
    if (!t->turbo_hold && t->frame_ready && t->started && t->extra_sync == 0) t->pos++;
    uint32_t i = t->pos, run = 0;
    int b;
    while ((b = _oric_tape_byte(t, i)) >= 0) {
        i++;
        if (b == 0x16) {
            run++;
        } else if (run >= 3) {
            t->pos = i - 1;  // premier octet après la synchro
            _oric_tape_turbo_hold(t);
            return true;
        } else {
            run = 0;
        }
    }
    t->pos = t->len;
    _oric_tape_turbo_hold(t);
    return false;
}

// Octet suivant (0 au bout de la bande)
static inline uint8_t oric_tape_turbo_byte(oric_tape_t* t) {
    if (!t->inserted) return 0;
    if (!t->turbo_hold) {
        if (t->frame_ready && t->started && t->extra_sync == 0) t->pos++;
        _oric_tape_turbo_hold(t);
    }
    const int b = _oric_tape_byte(t, t->pos);
    if (b < 0) return 0;
    t->pos++;
    if (t->pos >= t->len) t->phase = ORIC_TAPE_STOPPED;
    return (uint8_t)b;
}
