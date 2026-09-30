#pragma once

// hid_media.h — touches multimédia d'un clavier USB : Volume +, Volume −, Muet
// (page HID « Consumer », 0x0C : usages 0xE9, 0xEA, 0xE2). Elles n'arrivent pas
// dans le rapport « boot » du clavier mais sur une autre interface HID, avec
// leur propre descripteur ; celui-ci est lu au branchement (hid_media_parse),
// puis chaque rapport reçu est décodé (hid_media_keys).
//
// Deux formes de champ, les plus courantes :
// - tableau (Input Data,Array) : chaque élément porte le numéro de l'usage
//   (souvent 16 bits, 0 = aucune touche) ;
// - variable (Input Data,Variable) : un bit par usage, dans l'ordre des Usage.
//
// Indépendant de la plate-forme (testé dans tests/devices/test_hid_media.c ; copie
// identique à celle de ~/Neo6502TeleStrat, à part cette ligne).
//
// Copyright (c) 2026 bmarty — licence zlib/libpng (voir src/osd/osd.h)

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define HID_MEDIA_VOL_UP   0x01
#define HID_MEDIA_VOL_DOWN 0x02
#define HID_MEDIA_MUTE     0x04

#define HID_MEDIA_MAX_FIELDS 4
#define HID_MEDIA_MAX_IDS    8   // identifiants de rapport suivis pendant la lecture
#define HID_MEDIA_MAX_USAGES 32  // usages locaux retenus avant un Input

typedef struct {
    uint8_t report_id;  // 0 : pas d'identifiant de rapport
    bool array;
    uint16_t bit_off;   // position du champ après l'identifiant
    uint8_t size, count;
    // tableau : valeur de l'élément pour chaque touche (-1 : absente) ;
    // variable : numéro du bit dans le champ (-1 : absente)
    int16_t key[3];
} hid_media_field_t;

typedef struct {
    uint8_t num_fields;
    bool ids;  // le descripteur utilise des identifiants de rapport
    hid_media_field_t field[HID_MEDIA_MAX_FIELDS];
} hid_media_t;

static inline int _hid_media_key(uint32_t usage) {
    switch (usage) {
        case 0x000C00E9: return 0;
        case 0x000C00EA: return 1;
        case 0x000C00E2: return 2;
        default: return -1;
    }
}

// Lit le descripteur de rapport ; rend true s'il contient au moins une des trois touches
static inline bool hid_media_parse(hid_media_t* m, const uint8_t* desc, uint16_t len) {
    memset(m, 0, sizeof(*m));
    struct { uint32_t page, report_size, report_count, report_id; int32_t lmin; } g = {0}, stack[4];
    int sp = 0;
    uint32_t usages[HID_MEDIA_MAX_USAGES];
    int nu = 0;
    uint32_t umin = 0, umax = 0;
    bool have_range = false;
    // bits d'entrée déjà placés, par identifiant de rapport
    uint8_t id_of[HID_MEDIA_MAX_IDS];
    uint16_t bits[HID_MEDIA_MAX_IDS];
    int nid = 0;

    for (uint16_t i = 0; i < len;) {
        const uint8_t p = desc[i++];
        if (p == 0xFE) {  // élément long : ignoré
            if (i + 1 >= len) break;
            i += 2 + desc[i];
            continue;
        }
        const uint8_t sz = (p & 3) == 3 ? 4 : (p & 3);
        if (i + sz > len) break;
        uint32_t u = 0;
        for (int k = 0; k < sz; k++) u |= (uint32_t)desc[i + k] << (8 * k);
        int32_t s = sz == 1 ? (int8_t)u : sz == 2 ? (int16_t)u : (int32_t)u;
        i += sz;
        const uint8_t type = (p >> 2) & 3, tag = p >> 4;
        // usage local : page courante si l'usage tient sur 16 bits
        const uint32_t full = sz == 4 ? u : (g.page << 16) | u;
        if (type == 1) {  // global
            switch (tag) {
                case 0x0: g.page = u; break;
                case 0x1: g.lmin = s; break;
                case 0x7: g.report_size = u; break;
                case 0x8: g.report_id = u & 0xFF; m->ids = true; break;
                case 0x9: g.report_count = u; break;
                case 0xA: if (sp < 4) stack[sp++] = g; break;
                case 0xB: if (sp > 0) g = stack[--sp]; break;
                default: break;
            }
        } else if (type == 2) {  // local
            if (tag == 0x0 && nu < HID_MEDIA_MAX_USAGES) usages[nu++] = full;
            if (tag == 0x1) { umin = full; have_range = true; }
            if (tag == 0x2) { umax = full; have_range = true; }
        } else if (type == 0) {  // principal
            if (tag == 0x8) {  // Input
                int slot = 0;
                while (slot < nid && id_of[slot] != g.report_id) slot++;
                if (slot == nid) {
                    if (nid == HID_MEDIA_MAX_IDS) break;  // trop d'identifiants : lecture arrêtée
                    id_of[nid] = (uint8_t)g.report_id;
                    bits[nid++] = 0;
                }
                const uint16_t off = bits[slot];
                const uint32_t total = g.report_size * g.report_count;
                bits[slot] = (uint16_t)(off + total);
                const bool constant = u & 1, variable = u & 2;
                if (!constant && g.report_size && g.report_size <= 16 && m->num_fields < HID_MEDIA_MAX_FIELDS) {
                    hid_media_field_t f = {(uint8_t)g.report_id, !variable, off, (uint8_t)g.report_size,
                                           (uint8_t)(g.report_count > 255 ? 255 : g.report_count), {-1, -1, -1}};
                    bool any = false;
                    if (variable) {
                        for (uint32_t b = 0; b < f.count; b++) {
                            uint32_t us = have_range ? umin + b : (b < (uint32_t)nu ? usages[b] : (nu ? usages[nu - 1] : 0));
                            if (have_range && us > umax) break;
                            int k = _hid_media_key(us);
                            if (k >= 0 && f.key[k] < 0) { f.key[k] = (int16_t)b; any = true; }
                        }
                    } else if (have_range) {
                        for (int k = 0; k < 3; k++) {
                            const uint32_t want = k == 0 ? 0x000C00E9 : k == 1 ? 0x000C00EA : 0x000C00E2;
                            if (want >= umin && want <= umax) { f.key[k] = (int16_t)(g.lmin + (int32_t)(want - umin)); any = true; }
                        }
                    } else {
                        for (int j = 0; j < nu; j++) {
                            int k = _hid_media_key(usages[j]);
                            if (k >= 0 && f.key[k] < 0) { f.key[k] = (int16_t)(g.lmin + j); any = true; }
                        }
                    }
                    if (any) m->field[m->num_fields++] = f;
                }
            }
            nu = 0;
            have_range = false;
            umin = umax = 0;
        }
    }
    return m->num_fields > 0;
}

static inline uint32_t _hid_media_bits(const uint8_t* data, uint16_t len, uint32_t off, uint8_t n) {
    uint32_t v = 0;
    for (uint8_t b = 0; b < n; b++) {
        const uint32_t pos = off + b;
        if (pos / 8 >= len) break;
        if (data[pos / 8] & (1u << (pos % 8))) v |= 1u << b;
    }
    return v;
}

// Touches enfoncées dans ce rapport (HID_MEDIA_*) ; 0 pour un rapport d'un autre identifiant
static inline uint8_t hid_media_keys(const hid_media_t* m, const uint8_t* report, uint16_t len) {
    uint8_t keys = 0;
    uint8_t id = 0;
    if (m->ids) {
        if (len == 0) return 0;
        id = report[0];
        report++;
        len--;
    }
    for (int i = 0; i < m->num_fields; i++) {
        const hid_media_field_t* f = &m->field[i];
        if (f->report_id != id) continue;
        for (uint32_t e = 0; e < f->count; e++) {
            const uint32_t v = _hid_media_bits(report, len, f->bit_off + e * f->size, f->size);
            for (int k = 0; k < 3; k++) {
                if (f->key[k] < 0) continue;
                if (f->array ? (int32_t)v == f->key[k] : (e == (uint32_t)f->key[k] && v)) keys |= 1 << k;
            }
        }
    }
    return keys;
}

// Volume de sortie : niveaux 0 à HID_VOLUME_MAX (gain en 256es, progression
// d'environ 3 dB par pas), coupure à part (Muet, puis Volume + ou − la lève)
#define HID_VOLUME_MAX 8

typedef struct {
    uint8_t level;
    bool muted;
} hid_volume_t;

static inline void hid_volume_init(hid_volume_t* v) {
    v->level = HID_VOLUME_MAX;
    v->muted = false;
}

static inline uint16_t hid_volume_gain(const hid_volume_t* v) {
    static const uint16_t gain[HID_VOLUME_MAX + 1] = {0, 23, 32, 45, 64, 91, 128, 181, 256};
    return v->muted ? 0 : gain[v->level];
}

// Applique les touches nouvellement enfoncées ; rend true si le volume a changé
static inline bool hid_volume_keys(hid_volume_t* v, uint8_t pressed) {
    const hid_volume_t old = *v;
    if (pressed & HID_MEDIA_MUTE) v->muted = !v->muted;
    if (pressed & HID_MEDIA_VOL_UP) {
        v->muted = false;
        if (v->level < HID_VOLUME_MAX) v->level++;
    }
    if (pressed & HID_MEDIA_VOL_DOWN) {
        v->muted = false;
        if (v->level > 0) v->level--;
    }
    return old.level != v->level || old.muted != v->muted;
}

static inline uint8_t hid_volume_apply(const hid_volume_t* v, uint8_t sample) {
    return (uint8_t)((sample * hid_volume_gain(v)) >> 8);
}
