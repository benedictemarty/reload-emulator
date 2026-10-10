#pragma once

// bbc_config.h — BBC.CFG, configuration du BBC sur la clé USB (texte « clé=valeur »)
//
//   lecteur0=ELITE.SSD     image du lecteur 0 : fichier de la clé, ou « @Nom » pour une
//   lecteur1=@Battlezone   image en flash (nom du firmware) ; absent ou vide : lecteur vide
//   insertion=demarrer     insertion dans le lecteur 0 : demarrer (SHIFT+BREAK) ou inserer
//   volume=8               0 à 8 (même clé que ORIC.CFG et TELESTRA.CFG)
//
// Au montage de la clé, le firmware applique le fichier (lecteur 0 démarré si
// insertion=demarrer) ; le bouton « Enregistrer la configuration » du panneau le réécrit
// en gardant les lignes des autres clés et les commentaires (osd_cfgfile.h du socle).
// Indépendant de la plate-forme (testé par tests/bbc/test_bbc_menu.c).
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

#include "osd/osd_cfgfile.h"

#define BBC_CONFIG_FILE     "BBC.CFG"
#define BBC_CONFIG_NAME_LEN 40
#define BBC_CONFIG_FLASH    '@'   // Préfixe d'une image en flash

typedef struct {
    char drive[2][BBC_CONFIG_NAME_LEN];   // "" : vide ou absent
    bool drive_set[2];                    // Clé présente (même vide)
    int autoboot;                         // 1 demarrer, 0 inserer, -1 absent
    int volume;                           // 0 à max, -1 absent ou invalide
} bbc_config_t;

static const char* const bbc_config_keys[4] = {"lecteur0", "lecteur1", "insertion", "volume"};
static const char* const bbc_config_insertion[2] = {"inserer", "demarrer"};

// Lit le texte du fichier (terminé par 0)
static inline void bbc_config_parse(const char* text, bbc_config_t* c, int volume_max) {
    memset(c, 0, sizeof(*c));
    c->autoboot = -1;
    c->volume = -1;
    for (const char* line = text; line && *line; line = osd_cfg_next(line)) {
        const char* v;
        for (int d = 0; d < 2; d++) {
            if ((v = osd_cfg_value(line, bbc_config_keys[d]))) {
                osd_cfg_copy(c->drive[d], sizeof(c->drive[d]), v);
                c->drive_set[d] = true;
            }
        }
        if ((v = osd_cfg_value(line, "insertion"))) c->autoboot = osd_cfg_choice(v, bbc_config_insertion, 2);
        if ((v = osd_cfg_value(line, "volume"))) c->volume = osd_cfg_int(v, volume_max);
    }
}

// Réécrit le fichier : lignes de `old` (NULL : aucun) des autres clés, puis les nôtres.
// drive[d] : nom de l'image (« @Nom » pour la flash, "" : vide). Retourne la longueur.
static inline size_t bbc_config_merge(const char* old, const char* const drive[2], bool autoboot, int volume, char* out,
                                      size_t size) {
    osd_cfg_writer_t w;
    osd_cfg_writer_init(&w, out, size);
    if (!old || !*old) osd_cfg_put(&w, "# BBC Micro (Neo6502) : panneau F11, Enregistrer la configuration\n");
    osd_cfg_keep(&w, old, bbc_config_keys, 4);
    osd_cfg_put(&w, "lecteur0=%s\n", drive[0]);
    osd_cfg_put(&w, "lecteur1=%s\n", drive[1]);
    osd_cfg_put(&w, "insertion=%s\n", bbc_config_insertion[autoboot ? 1 : 0]);
    osd_cfg_put(&w, "volume=%d\n", volume);
    return w.len;
}
