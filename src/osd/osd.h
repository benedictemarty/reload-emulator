#pragma once

// osd.h — surface du menu (OSD) : grille de cellules de 8 x 8 pixels
//
// Reprise de la surface du Telestrat (projet Neo6502TeleStrat, même auteur),
// avec une grille dimensionnée d'après la sortie vidéo (OSD_COLS x OSD_ROWS,
// 100 x 30 par défaut : 800 x 240 lignes de tampon affichées deux fois, soit
// le 800 x 480 du Neo6502) et un rendu en indices de palette (1 octet par
// pixel, couleurs 0-7 : bit 0 rouge, bit 1 vert, bit 2 bleu, comme l'Oric).
//
// Chaque cellule : un caractère (osd_font.h), une couleur d'encre, une
// couleur de fond, et un fond « tramé » (la couleur un pixel sur deux,
// décalée d'une ligne à l'autre) qui donne des demi-teintes. Grandes lettres :
// un caractère sur deux cellules, pixels doublés en largeur.
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

#include "osd/osd_font.h"

#ifndef OSD_COLS
#define OSD_COLS 100
#endif
#ifndef OSD_ROWS
#define OSD_ROWS 30
#endif
#define OSD_WIDTH (OSD_COLS * 8)
#define OSD_LINES (OSD_ROWS * 8)

// Placement des fonctions de rendu (appelées par le cœur 1 sur le RP2040)
#ifndef OSD_HOT
#define OSD_HOT
#endif

// Couleurs (bit 0 rouge, 1 vert, 2 bleu), comme l'Oric
enum {
    OSD_BLACK = 0, OSD_RED, OSD_GREEN, OSD_YELLOW, OSD_BLUE, OSD_MAGENTA, OSD_CYAN, OSD_WHITE,
};
#define OSD_DITHER 0x08  // fond tramé : la couleur de fond un pixel sur deux

// Attribut : bits 0-2 encre, bits 4-6 fond, bit 7 fond tramé
#define OSD_ATTR(ink, paper) ((uint8_t)((ink) | (((paper) & 7) << 4) | (((paper) & OSD_DITHER) ? 0x80 : 0)))

// Cellule d'une grande lettre (pixels doublés en largeur)
#define OSD_BIG_LEFT  1
#define OSD_BIG_RIGHT 2

typedef struct {
    uint8_t ch[OSD_ROWS][OSD_COLS];
    uint8_t attr[OSD_ROWS][OSD_COLS];
    uint8_t big[OSD_ROWS][OSD_COLS];
} osd_surface_t;

static inline void osd_clear(osd_surface_t* s, uint8_t attr) {
    memset(s->ch, ' ', sizeof(s->ch));
    memset(s->attr, attr, sizeof(s->attr));
    memset(s->big, 0, sizeof(s->big));
}

// Rectangle d'attribut (fond d'un panneau), caractères effacés
static inline void osd_fill(osd_surface_t* s, int row, int col, int rows, int cols, uint8_t attr) {
    for (int r = row; r < row + rows; r++) {
        if (r < 0 || r >= OSD_ROWS) continue;
        for (int c = col; c < col + cols; c++) {
            if (c < 0 || c >= OSD_COLS) continue;
            s->ch[r][c] = ' ';
            s->attr[r][c] = attr;
            s->big[r][c] = 0;
        }
    }
}

static inline void osd_putc(osd_surface_t* s, int row, int col, uint8_t ch, uint8_t attr) {
    if (row < 0 || row >= OSD_ROWS || col < 0 || col >= OSD_COLS) return;
    s->ch[row][col] = ch;
    s->attr[row][col] = attr;
    s->big[row][col] = 0;
}

// Caractère suivant d'une chaîne UTF-8, ramené au codage de la police
// (Latin-1 pour les accents ; '?' hors de portée)
static inline uint8_t osd_next_char(const char** p) {
    const uint8_t* s = (const uint8_t*)*p;
    uint32_t c = s[0];
    int n = 1;
    if (c >= 0xC0 && c < 0xE0 && (s[1] & 0xC0) == 0x80) {
        c = ((c & 0x1F) << 6) | (s[1] & 0x3F);
        n = 2;
    } else if (c >= 0xE0 && c < 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        c = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        n = 3;
    } else if (c >= 0x80 && c < 0xC0) {
        c = '?';   // Octet de continuation isolé
    } else if (c >= 0xF0) {
        c = '?';
    }
    *p += n;
    if (c == 0x2014 || c == 0x2013) return OSD_EMDASH;   // — –
    if (c == 0x2026) return OSD_ELLIPSIS;                // …
    if (c == 0x2192) return OSD_ARROW_R;                 // →
    return c < 256 ? (uint8_t)c : '?';
}

// Écrit une chaîne UTF-8 ; au plus max cellules (max < 0 : jusqu'au bord).
// Retourne le nombre de cellules écrites.
static inline int osd_puts(osd_surface_t* s, int row, int col, const char* str, uint8_t attr, int max) {
    int n = 0;
    while (*str && (max < 0 || n < max) && col + n < OSD_COLS) {
        osd_putc(s, row, col + n, osd_next_char(&str), attr);
        n++;
    }
    return n;
}

// Longueur d'une chaîne UTF-8 en cellules
static inline int osd_strlen(const char* str) {
    int n = 0;
    while (*str) {
        osd_next_char(&str);
        n++;
    }
    return n;
}

// Grandes lettres : deux cellules par caractère
static inline int osd_puts_big(osd_surface_t* s, int row, int col, const char* str, uint8_t attr) {
    int n = 0;
    while (*str && col + n + 1 < OSD_COLS) {
        uint8_t c = osd_next_char(&str);
        osd_putc(s, row, col + n, c, attr);
        osd_putc(s, row, col + n + 1, c, attr);
        if (row >= 0 && row < OSD_ROWS && col + n >= 0) {
            s->big[row][col + n] = OSD_BIG_LEFT;
            s->big[row][col + n + 1] = OSD_BIG_RIGHT;
        }
        n += 2;
    }
    return n;
}

// Cadre à coins arrondis
static inline void osd_frame(osd_surface_t* s, int row, int col, int rows, int cols, uint8_t attr) {
    for (int c = col + 1; c < col + cols - 1; c++) {
        osd_putc(s, row, c, OSD_HLINE, attr);
        osd_putc(s, row + rows - 1, c, OSD_HLINE, attr);
    }
    for (int r = row + 1; r < row + rows - 1; r++) {
        osd_putc(s, r, col, OSD_VLINE, attr);
        osd_putc(s, r, col + cols - 1, OSD_VLINE, attr);
    }
    osd_putc(s, row, col, OSD_TL, attr);
    osd_putc(s, row, col + cols - 1, OSD_TR, attr);
    osd_putc(s, row + rows - 1, col, OSD_BL, attr);
    osd_putc(s, row + rows - 1, col + cols - 1, OSD_BR, attr);
}

// Octet de glyphe -> 16 pixels (grandes lettres)
static inline uint16_t _osd_widen(uint8_t b) {
    uint16_t w = 0;
    for (int i = 0; i < 8; i++) {
        if (b >> i & 1) w |= (uint16_t)(3u << (2 * i));
    }
    return w;
}

// Une ligne de tampon (0 à OSD_LINES - 1) en indices de palette, 8 pixels par
// cellule (bit 0 du glyphe = pixel de gauche) ; out : OSD_WIDTH octets
static inline void OSD_HOT osd_render_line(const osd_surface_t* s, int line, uint8_t* out) {
    const int row = line >> 3, y = line & 7, parity = line & 1;
    const uint8_t* chs = s->ch[row];
    const uint8_t* attrs = s->attr[row];
    const uint8_t* bigs = s->big[row];
    for (int c = 0; c < OSD_COLS; c++) {
        uint32_t px = osd_font[chs[c]][y];
        if (bigs[c]) {
            const uint16_t wide = _osd_widen((uint8_t)px);
            px = bigs[c] == OSD_BIG_LEFT ? (wide & 0xFFu) : (uint32_t)(wide >> 8);
        }
        const uint8_t a = attrs[c];
        const uint8_t ink = a & 7, paper = (a >> 4) & 7;
        const bool dither = a & 0x80;
        uint8_t* o = out + c * 8;
        for (int i = 0; i < 8; i++) {
            if (px & (1u << i)) {
                o[i] = ink;
            } else {
                o[i] = (dither && ((i ^ parity) & 1)) ? OSD_BLACK : paper;
            }
        }
    }
}
