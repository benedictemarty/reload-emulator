#pragma once

// osd_cfgfile.h — fichier de configuration commun des menus F1 (texte « clé=valeur »)
//
// Brique du socle, sans rien de propre à une machine (l'Oric s'en sert dans oric_config.h pour
// ORIC.CFG). Format : une clé par ligne, « clé=valeur » ; lignes vides, commentaires (#) et
// clés inconnues gardés quand le menu réécrit le fichier. Une valeur de profil se découpe en
// champs séparés par « ; » (« Libellé;clé=valeur;… »).
//
//   osd_cfg_next        ligne suivante d'un texte (boucle sur les lignes)
//   osd_cfg_value       valeur de la clé sur une ligne, ou NULL
//   osd_cfg_copy        copie d'une valeur, jusqu'à la fin de ligne, espaces de fin retirés
//   osd_cfg_int         entier de 0 à max, -1 si invalide
//   osd_cfg_choice      rang d'une valeur parmi des mots, -1 si inconnue
//   osd_cfg_field       champ suivant d'une valeur à « ; » (espaces de bord retirés)
//   osd_cfg_writer_t    réécriture : osd_cfg_keep (lignes des autres clés), osd_cfg_put
//
// C pur, sans allocation ; à inclure tel quel.
//
// Copyright (c) 2026 bmarty — licence zlib/libpng (voir src/devices/neo_storage.h)

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

// Début de la ligne suivante (après le '\n'), ou NULL à la fin du texte
static inline const char* osd_cfg_next(const char* line) {
    const char* e = strchr(line, '\n');
    return (e && e[1]) ? e + 1 : NULL;
}

// Valeur de la clé `key` sur la ligne (« a », « fdc »…), ou NULL
static inline const char* osd_cfg_value(const char* line, const char* key) {
    const size_t n = strlen(key);
    if (strncmp(line, key, n) != 0 || line[n] != '=') return NULL;
    return line + n + 1;
}

// Fin d'une valeur : 0, '\r' ou '\n'
static inline bool _osd_cfg_end(char c) { return c == 0 || c == '\r' || c == '\n'; }

// Copie la valeur (jusqu'à la fin de ligne, espaces de fin retirés), tronquée à size - 1
static inline void osd_cfg_copy(char* dst, size_t size, const char* v) {
    size_t n = 0;
    while (!_osd_cfg_end(v[n])) n++;
    while (n > 0 && v[n - 1] == ' ') n--;
    if (n >= size) n = size - 1;
    memcpy(dst, v, n);
    dst[n] = 0;
}

// Entier de 0 à max (3 chiffres au plus, espaces de fin admis), -1 si invalide
static inline int osd_cfg_int(const char* v, int max) {
    int n = 0, digits = 0;
    while (v[digits] >= '0' && v[digits] <= '9' && digits < 3) n = n * 10 + (v[digits++] - '0');
    const char e = v[digits];
    if (!digits || (!_osd_cfg_end(e) && e != ' ') || n > max) return -1;
    return n;
}

// Rang de la valeur parmi les mots `words` (mot entier, suivi d'une fin ou d'un espace), -1 sinon
static inline int osd_cfg_choice(const char* v, const char* const* words, int count) {
    for (int i = 0; i < count; i++) {
        const size_t n = strlen(words[i]);
        if (strncmp(v, words[i], n) == 0 && (_osd_cfg_end(v[n]) || v[n] == ' ')) return i;
    }
    return -1;
}

// Champ jusqu'au « ; » ou à la fin de ligne, espaces de bord retirés ; retourne le début du
// champ suivant, ou NULL s'il n'y en a plus
static inline const char* osd_cfg_field(const char* v, char* dst, size_t size) {
    while (*v == ' ') v++;
    size_t n = 0;
    while (v[n] != ';' && !_osd_cfg_end(v[n])) n++;
    size_t m = n;
    while (m > 0 && v[m - 1] == ' ') m--;
    if (m >= size) m = size - 1;
    memcpy(dst, v, m);
    dst[m] = 0;
    return v[n] == ';' ? v + n + 1 : NULL;
}

/*-- Réécriture --------------------------------------------------------------*/

typedef struct {
    char* out;
    size_t size;   // Taille du tampon (le texte est toujours terminé par 0)
    size_t len;    // Longueur écrite
} osd_cfg_writer_t;

static inline void osd_cfg_writer_init(osd_cfg_writer_t* w, char* out, size_t size) {
    w->out = out;
    w->size = size;
    w->len = 0;
    if (size) out[0] = 0;
}

// Ajoute du texte au format printf (tronqué à la fin du tampon)
static inline void osd_cfg_put(osd_cfg_writer_t* w, const char* fmt, ...) {
    if (w->len + 1 >= w->size) return;
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(w->out + w->len, w->size - w->len, fmt, ap);
    va_end(ap);
    if (n > 0) w->len += (size_t)n < w->size - w->len ? (size_t)n : w->size - w->len - 1;
}

// Recopie les lignes de `old` (peut être NULL) dont la clé n'est pas dans `keys` (ces clés-là
// seront réécrites par l'appelant) ; les fins de ligne \r\n deviennent \n
static inline void osd_cfg_keep(osd_cfg_writer_t* w, const char* old, const char* const* keys, int count) {
    for (const char* p = old; p && *p;) {
        const char* e = p;
        while (*e && *e != '\n') e++;
        bool ours = false;
        for (int i = 0; i < count && !ours; i++) ours = osd_cfg_value(p, keys[i]) != NULL;
        if (!ours) osd_cfg_put(w, "%.*s\n", (int)(e - p > 0 && e[-1] == '\r' ? e - p - 1 : e - p), p);
        p = *e ? e + 1 : e;
    }
}
