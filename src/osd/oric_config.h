#pragma once

// oric_config.h — configuration de l'Oric sur la clé USB (ORIC.CFG)
//
// Une clé par ligne : « fdc=aucune|pravetz|microdisc|jasmin » (interface
// disque), « a=NOM.DSK » … « d=NOM.DSK » (lecteurs), « tape=NOM.TAP »
// (cassette). Les lignes vides, les commentaires (#) et les autres clés sont
// gardés quand le menu réécrit le fichier.
//
// Profils (au plus ORIC_CONFIG_PROFILES) : « profil=Libellé;clé=valeur;… »
// avec fdc=, rom= (ROM BASIC de 16 Ko sur la clé, ex. BASIC 1.0 de l'Oric-1
// ou ROM d'origine du Nova 64), a= … d=, tape= ; appliquer un profil vide
// les lecteurs et la cassette qu'il ne cite pas.
// « demarrage=choix » ouvre au montage de la clé la page « Démarrer sur… »,
// « demarrage=Libellé » applique directement un profil (intégré ou de la clé).
//
// Indépendant de la plate-forme (testé par tests/oric/test_oric_menu.c).
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
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define ORIC_CONFIG_FILE "ORIC.CFG"

static const char* const oric_config_fdc_keys[4] = {"aucune", "pravetz", "microdisc", "jasmin"};

// Valeur de la clé `key` sur la ligne (« a », « fdc »…), ou NULL
static inline const char* oric_config_value(const char* line, const char* key) {
    size_t n = strlen(key);
    if (strncmp(line, key, n) != 0 || line[n] != '=') return NULL;
    return line + n + 1;
}

// Interface disque d'une valeur « fdc= » (0-3), -1 si inconnue
static inline int oric_config_fdc(const char* v) {
    for (int i = 0; i < 4; i++) {
        size_t n = strlen(oric_config_fdc_keys[i]);
        if (strncmp(v, oric_config_fdc_keys[i], n) == 0 && (v[n] == 0 || v[n] == '\r' || v[n] == '\n' || v[n] == ' ')) {
            return i;
        }
    }
    return -1;
}

// Copie la valeur (jusqu'à la fin de ligne, espaces de fin retirés)
static inline void oric_config_copy(char* dst, size_t size, const char* v) {
    size_t n = 0;
    while (v[n] && v[n] != '\r' && v[n] != '\n') n++;
    while (n > 0 && v[n - 1] == ' ') n--;
    if (n >= size) n = size - 1;
    memcpy(dst, v, n);
    dst[n] = 0;
}

static inline bool _oric_config_ours(const char* line) {
    static const char* const keys[6] = {"fdc", "a", "b", "c", "d", "tape"};
    for (int i = 0; i < 6; i++) {
        if (oric_config_value(line, keys[i])) return true;
    }
    return false;
}

// Nouveau contenu de ORIC.CFG : les lignes de `old` (peut être NULL) qui ne
// sont pas à nous, puis nos réglages (lecteurs et cassette vides : omis).
// Retourne la longueur écrite (sans le 0 final), tronquée à out_size - 1.
static inline size_t oric_config_merge(const char* old, int fdc, const char* const drives[4], const char* tape, char* out,
                                       size_t out_size) {
    size_t len = 0;
    out[0] = 0;
#define _ORIC_CFG_PUT(...)                                                                  \
    do {                                                                                    \
        if (len + 1 < out_size) {                                                           \
            int w = snprintf(out + len, out_size - len, __VA_ARGS__);                       \
            if (w > 0) len += (size_t)w < out_size - len ? (size_t)w : out_size - len - 1; \
        }                                                                                   \
    } while (0)
    for (const char* p = old; p && *p;) {
        const char* e = p;
        while (*e && *e != '\n') e++;
        if (!_oric_config_ours(p)) {
            _ORIC_CFG_PUT("%.*s\n", (int)(e - p > 0 && e[-1] == '\r' ? e - p - 1 : e - p), p);
        }
        p = *e ? e + 1 : e;
    }
    if (fdc >= 0 && fdc < 4) _ORIC_CFG_PUT("fdc=%s\n", oric_config_fdc_keys[fdc]);
    for (int d = 0; d < 4; d++) {
        if (drives[d] && drives[d][0]) _ORIC_CFG_PUT("%c=%s\n", 'a' + d, drives[d]);
    }
    if (tape && tape[0]) _ORIC_CFG_PUT("tape=%s\n", tape);
#undef _ORIC_CFG_PUT
    return len;
}

/*-- Profils ------------------------------------------------------------------*/

#define ORIC_CONFIG_PROFILES  3
#define ORIC_CONFIG_LABEL_LEN 28
#define ORIC_CONFIG_NAME_LEN  48

typedef struct {
    char label[ORIC_CONFIG_LABEL_LEN];
    int fdc;                                  // -1 : inchangée
    char rom[ORIC_CONFIG_NAME_LEN];           // "" : ROM du firmware
    char drive[4][ORIC_CONFIG_NAME_LEN];
    char tape[ORIC_CONFIG_NAME_LEN];
} oric_profile_t;

// Copie jusqu'au séparateur `;` ou à la fin de ligne, espaces de bord retirés
static inline const char* _oric_config_field(const char* v, char* dst, size_t size) {
    while (*v == ' ') v++;
    size_t n = 0;
    while (v[n] && v[n] != ';' && v[n] != '\r' && v[n] != '\n') n++;
    size_t m = n;
    while (m > 0 && v[m - 1] == ' ') m--;
    if (m >= size) m = size - 1;
    memcpy(dst, v, m);
    dst[m] = 0;
    return v[n] == ';' ? v + n + 1 : NULL;
}

// Valeur d'une ligne « profil= » -> profil ; false sans libellé
static inline bool oric_config_profile(const char* v, oric_profile_t* p) {
    memset(p, 0, sizeof(*p));
    p->fdc = -1;
    const char* next = _oric_config_field(v, p->label, sizeof(p->label));
    if (!p->label[0]) return false;
    while (next) {
        char field[ORIC_CONFIG_NAME_LEN + 8];
        next = _oric_config_field(next, field, sizeof(field));
        const char* val;
        if ((val = oric_config_value(field, "fdc"))) {
            p->fdc = oric_config_fdc(val);
        } else if ((val = oric_config_value(field, "rom"))) {
            oric_config_copy(p->rom, sizeof(p->rom), val);
        } else if ((val = oric_config_value(field, "tape"))) {
            oric_config_copy(p->tape, sizeof(p->tape), val);
        } else {
            for (int d = 0; d < 4; d++) {
                const char key[2] = {(char)('a' + d), 0};
                if ((val = oric_config_value(field, key))) oric_config_copy(p->drive[d], sizeof(p->drive[d]), val);
            }
        }
    }
    return true;
}
