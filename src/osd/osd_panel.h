#pragma once

// osd_panel.h — style commun des panneaux de contrôle (menu F1) sur la surface OSD
//
// Brique du socle, sans rien de propre à une machine : le panneau de l'Oric
// (src/osd/oric_menu.h) la suit, et les autres systèmes Neo6502 peuvent s'en servir
// pour avoir la même allure :
//
//   bandeau      osd_panel_header : 3 lignes bleues, titre en grands caractères,
//                sous-titre, texte à droite, filet
//   panneaux     osd_panel_box : fond tramé, cadre, icône et titre dans le filet
//   éléments     osd_panel_item_bar : barre de sélection pleine, marqueur ▶ ;
//                attributs OSD_PANEL_* (normal) et OSD_PANEL_SEL* (sélectionné)
//   boutons      osd_panel_button
//   message      osd_panel_message : ✓ en vert ou ✗ en rouge
//   pied         osd_panel_footer : touches en pastilles et leur aide
//   sélecteur    osd_panel_popup (cadre avec ombre et titre), osd_panel_popup_row
//                (ligne, attributs rendus), osd_panel_scrollbar
//   tailles      osd_panel_size : « 12 Ko », « 1,5 Mo »
//
// La mise en page (lignes, colonnes) reste à l'appelant. À inclure après osd/osd.h.
//
// Copyright (c) 2026 bmarty — licence zlib/libpng (voir src/devices/neo_storage.h)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "osd/osd.h"

// Couleurs
#define OSD_PANEL_BG        OSD_ATTR(OSD_WHITE, OSD_BLACK)
#define OSD_PANEL           OSD_ATTR(OSD_WHITE, OSD_BLUE | OSD_DITHER)
#define OSD_PANEL_DIM       OSD_ATTR(OSD_CYAN, OSD_BLUE | OSD_DITHER)
#define OSD_PANEL_ACC       OSD_ATTR(OSD_YELLOW, OSD_BLUE | OSD_DITHER)
#define OSD_PANEL_OK        OSD_ATTR(OSD_GREEN, OSD_BLUE | OSD_DITHER)
#define OSD_PANEL_ERR       OSD_ATTR(OSD_RED, OSD_BLUE | OSD_DITHER)
#define OSD_PANEL_EDGE      OSD_ATTR(OSD_CYAN, OSD_BLUE | OSD_DITHER)
#define OSD_PANEL_SEL       OSD_ATTR(OSD_WHITE, OSD_BLUE)
#define OSD_PANEL_SEL_ACC   OSD_ATTR(OSD_YELLOW, OSD_BLUE)
#define OSD_PANEL_SEL_DIM   OSD_ATTR(OSD_CYAN, OSD_BLUE)
#define OSD_PANEL_SEL_OK    OSD_ATTR(OSD_GREEN, OSD_BLUE)
#define OSD_PANEL_SEL_ERR   OSD_ATTR(OSD_RED, OSD_BLUE)
#define OSD_PANEL_KEY       OSD_ATTR(OSD_BLACK, OSD_CYAN)    // Pastille de touche, bouton choisi
#define OSD_PANEL_HEAD      OSD_ATTR(OSD_WHITE, OSD_BLUE)    // Bandeau
#define OSD_PANEL_POPUP     OSD_ATTR(OSD_WHITE, OSD_BLACK)   // Sélecteur : fond
#define OSD_PANEL_POPUP_DIM OSD_ATTR(OSD_CYAN, OSD_BLACK)
#define OSD_PANEL_POPUP_ACC OSD_ATTR(OSD_YELLOW, OSD_BLACK)  // Sélecteur : cadre, titre
#define OSD_PANEL_POPUP_ERR OSD_ATTR(OSD_RED, OSD_BLACK)
#define OSD_PANEL_POPUP_SEL OSD_ATTR(OSD_BLACK, OSD_CYAN)    // Sélecteur : ligne choisie
#define OSD_PANEL_POPUP_SEL_DIM OSD_ATTR(OSD_BLUE, OSD_CYAN)
#define OSD_PANEL_POPUP_SEL_ERR OSD_ATTR(OSD_RED, OSD_CYAN)

// Attributs d'un élément selon qu'il est sélectionné
static inline uint8_t osd_panel_base(bool sel) { return sel ? OSD_PANEL_SEL : OSD_PANEL; }
static inline uint8_t osd_panel_dim(bool sel) { return sel ? OSD_PANEL_SEL_DIM : OSD_PANEL_DIM; }
static inline uint8_t osd_panel_acc(bool sel) { return sel ? OSD_PANEL_SEL_ACC : OSD_PANEL_ACC; }
static inline uint8_t osd_panel_ok(bool sel) { return sel ? OSD_PANEL_SEL_OK : OSD_PANEL_OK; }
static inline uint8_t osd_panel_err(bool sel) { return sel ? OSD_PANEL_SEL_ERR : OSD_PANEL_ERR; }

// Bandeau sur toute la largeur (lignes row à row + 2, filet en row + 3) : titre en grands
// caractères à la colonne col, sous-titre à sub_col, texte à droite finissant à right_end
static inline void osd_panel_header(osd_surface_t* s, int row, int col, const char* title, int sub_col,
                                    const char* subtitle, int right_end, const char* right) {
    osd_fill(s, row, 0, 3, OSD_COLS, OSD_PANEL_HEAD);
    osd_puts_big(s, row + 1, col, title, OSD_PANEL_HEAD);
    if (subtitle) osd_puts(s, row + 1, sub_col, subtitle, OSD_ATTR(OSD_YELLOW, OSD_BLUE), -1);
    if (right) osd_puts(s, row + 1, right_end - osd_strlen(right), right, OSD_ATTR(OSD_CYAN, OSD_BLUE), -1);
    for (int c = 0; c < OSD_COLS; c++) osd_putc(s, row + 3, c, OSD_HLINE, OSD_ATTR(OSD_CYAN, OSD_BLACK));
}

// Panneau : fond tramé, cadre arrondi, icône (deux caractères, 0 : aucune) et titre dans le filet du haut
static inline void osd_panel_box(osd_surface_t* s, int row, int col, int rows, int cols, uint8_t icon, const char* title) {
    osd_fill(s, row, col, rows, cols, OSD_PANEL);
    osd_frame(s, row, col, rows, cols, OSD_PANEL_EDGE, NULL, 0);
    int c = col + 2;
    osd_putc(s, row, c++, ' ', OSD_PANEL_EDGE);
    if (icon) {
        osd_putc(s, row, c++, icon, OSD_PANEL_ACC);
        osd_putc(s, row, c++, (uint8_t)(icon + 1), OSD_PANEL_ACC);
        osd_putc(s, row, c++, ' ', OSD_PANEL_EDGE);
    }
    c += osd_puts(s, row, c, title, OSD_PANEL, -1);
    osd_putc(s, row, c, ' ', OSD_PANEL_EDGE);
}

// Ligne d'élément : barre de sélection pleine, marqueur ▶
static inline void osd_panel_item_bar(osd_surface_t* s, int row, int col, int cols, bool selected) {
    osd_fill(s, row, col, 1, cols, selected ? OSD_PANEL_SEL : OSD_PANEL);
    if (selected) osd_putc(s, row, col, OSD_TRI_R, OSD_PANEL_SEL_ACC);
}

// Bouton : libellé centré, en pastille quand il est choisi
static inline void osd_panel_button(osd_surface_t* s, int row, int col, int cols, const char* label, bool selected) {
    const uint8_t attr = selected ? OSD_PANEL_KEY : OSD_PANEL;
    osd_fill(s, row, col, 1, cols, attr);
    osd_puts(s, row, col + (cols - osd_strlen(label)) / 2, label, attr, -1);
}

// Résultat de la dernière action sur fond noir : ✓ et texte jaune, ou ✗ et texte rouge
static inline void osd_panel_message(osd_surface_t* s, int row, int col, bool error, const char* text, int max) {
    osd_putc(s, row, col, error ? OSD_CROSS : OSD_CHECK, OSD_ATTR(error ? OSD_RED : OSD_GREEN, OSD_BLACK));
    osd_puts(s, row, col + 2, text, OSD_ATTR(error ? OSD_RED : OSD_YELLOW, OSD_BLACK), max);
}

// Pied sur toute la largeur (2 lignes) : n touches en pastilles suivies de leur aide
// (help[i][0] : « Entrée », espaces compris ; help[i][1] : « valider »), à partir de col ;
// right (NULL : aucun) à la colonne right_col
static inline void osd_panel_footer(osd_surface_t* s, int row, int col, const char* const (*help)[2], int n,
                                    int right_col, const char* right) {
    osd_fill(s, row, 0, 2, OSD_COLS, OSD_PANEL);
    int c = col;
    for (int i = 0; i < n; i++) {
        c += osd_puts(s, row, c, help[i][0], OSD_PANEL_KEY, -1) + 1;
        c += osd_puts(s, row, c, help[i][1], OSD_PANEL, -1) + 2;
    }
    if (right) osd_puts(s, row, right_col, right, OSD_PANEL_EDGE, -1);
}

// Sélecteur superposé : ombre, fond noir, cadre jaune, icône et titre dans le filet du haut.
// Les lignes vont de top + 2 à top + height - 3 (osd_panel_popup_row)
static inline void osd_panel_popup(osd_surface_t* s, int top, int left, int height, int width, uint8_t icon,
                                   const char* title) {
    osd_fill(s, top + 1, left + 2, height, width, OSD_PANEL);   // Ombre
    osd_fill(s, top, left, height, width, OSD_PANEL_POPUP);
    osd_frame(s, top, left, height, width, OSD_PANEL_POPUP_ACC, NULL, 0);
    osd_putc(s, top, left + 2, ' ', OSD_PANEL_POPUP_ACC);
    osd_putc(s, top, left + 3, icon, OSD_PANEL_POPUP_ACC);
    osd_putc(s, top, left + 4, (uint8_t)(icon + 1), OSD_PANEL_POPUP_ACC);
    const int tl = osd_puts(s, top, left + 6, title, OSD_PANEL_POPUP, -1);
    osd_putc(s, top, left + 6 + tl, ' ', OSD_PANEL_POPUP_ACC);
}

// Ligne du sélecteur : fond posé ; *base et *dim reçoivent les attributs du texte
static inline void osd_panel_popup_row(osd_surface_t* s, int row, int left, int width, bool sel, uint8_t* base,
                                       uint8_t* dim) {
    *base = sel ? OSD_PANEL_POPUP_SEL : OSD_PANEL_POPUP;
    *dim = sel ? OSD_PANEL_POPUP_SEL_DIM : OSD_PANEL_POPUP_DIM;
    osd_fill(s, row, left + 2, 1, width - 5, *base);
}

// Barre de défilement à la colonne col, de row à row + visible - 1 (rien si tout tient)
static inline void osd_panel_scrollbar(osd_surface_t* s, int row, int col, int visible, int scroll, int total) {
    if (total <= visible) return;
    for (int k = 0; k < visible; k++) osd_putc(s, row + k, col, OSD_SHADE, OSD_ATTR(OSD_BLUE, OSD_BLACK));
    const int thumb = scroll * (visible - 1) / (total - visible);
    osd_putc(s, row + thumb, col, OSD_FULL, OSD_PANEL_POPUP_DIM);
}

// Taille d'un fichier : « 12 Ko » (arrondi au-dessus), « 1,5 Mo »
static inline void osd_panel_size(char* buf, size_t n, uint32_t size) {
    if (size >= 1024 * 1024) {
        snprintf(buf, n, "%u,%u Mo", (unsigned)(size >> 20), (unsigned)((size % (1u << 20)) * 10 >> 20));
    } else {
        snprintf(buf, n, "%u Ko", (unsigned)((size + 1023) >> 10));
    }
}
