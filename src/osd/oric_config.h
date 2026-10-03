#pragma once

// oric_config.h — configuration de l'Oric sur la clé USB (ORIC.CFG)
//
// Une clé par ligne : « fdc=aucune|pravetz|microdisc|jasmin » (interface
// disque), « a=NOM.DSK » … « d=NOM.DSK » (lecteurs), « tape=NOM.TAP »
// (cassette), « volume=0 » … « volume=8 » (8 : maximum, 0 : silence). Les
// lignes vides, les commentaires (#) et les autres clés sont gardés quand le
// menu réécrit le fichier.
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

#include "osd/osd_cfgfile.h"   // Fichier de configuration commun (socle)

#define ORIC_CONFIG_FILE "ORIC.CFG"

static const char* const oric_config_fdc_keys[4] = {"aucune", "pravetz", "microdisc", "jasmin"};

// Valeur de la clé `key` sur la ligne (« a », « fdc »…), ou NULL
static inline const char* oric_config_value(const char* line, const char* key) { return osd_cfg_value(line, key); }

// Interface disque d'une valeur « fdc= » (0-3), -1 si inconnue
static inline int oric_config_fdc(const char* v) { return osd_cfg_choice(v, oric_config_fdc_keys, 4); }

// Copie la valeur (jusqu'à la fin de ligne, espaces de fin retirés)
static inline void oric_config_copy(char* dst, size_t size, const char* v) { osd_cfg_copy(dst, size, v); }

// Volume d'une valeur « volume= » (0 à max), -1 si invalide
static inline int oric_config_volume(const char* v, int max) { return osd_cfg_int(v, max); }

// Nouveau contenu de ORIC.CFG : les lignes de `old` (peut être NULL) qui ne
// sont pas à nous, puis nos réglages (lecteurs et cassette vides : omis ;
// volume < 0 : omis). Retourne la longueur écrite (sans le 0 final), tronquée
// à out_size - 1.
static inline size_t oric_config_merge(const char* old, int fdc, const char* const drives[4], const char* tape, int volume,
                                       char* out, size_t out_size) {
    static const char* const ours[7] = {"fdc", "a", "b", "c", "d", "tape", "volume"};
    osd_cfg_writer_t w;
    osd_cfg_writer_init(&w, out, out_size);
    osd_cfg_keep(&w, old, ours, 7);
    if (fdc >= 0 && fdc < 4) osd_cfg_put(&w, "fdc=%s\n", oric_config_fdc_keys[fdc]);
    for (int d = 0; d < 4; d++) {
        if (drives[d] && drives[d][0]) osd_cfg_put(&w, "%c=%s\n", 'a' + d, drives[d]);
    }
    if (tape && tape[0]) osd_cfg_put(&w, "tape=%s\n", tape);
    if (volume >= 0) osd_cfg_put(&w, "volume=%d\n", volume);
    return w.len;
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
static inline const char* _oric_config_field(const char* v, char* dst, size_t size) { return osd_cfg_field(v, dst, size); }
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
