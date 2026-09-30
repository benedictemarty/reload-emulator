#pragma once

// oric_menu.h — panneau de contrôle (OSD) de l'Oric, sur le modèle du menu du
// Telestrat (projet Neo6502TeleStrat)
//
// Page principale : interface disque (aucune, Pravetz 8D, Microdisc, Jasmin),
// lecteurs A à D, cassette, clé USB, boutons Redémarrer / Enregistrer la
// configuration / Reprendre. Entrée sur l'interface, un lecteur ou la cassette :
// sélecteur (interfaces, fichiers .dsk ou .tap de la clé) avec défilement et
// saut à l'initiale tapée. La plate-forme remplit l'état (lecteurs, cassette,
// clé, fichiers, message) et exécute les actions rendues par oric_menu_key().
//
// Mise en page pour une grille d'au moins 100 x 30 cellules (osd.h), centrée
// dans une grille plus grande. Indépendant de la plate-forme (testé par
// tests/oric/test_oric_menu.c).
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

#include <stdio.h>
#include <string.h>

#include "osd/osd.h"

#ifndef ORIC_MENU_FILES
#define ORIC_MENU_FILES 64   // Fichiers de la clé listés
#endif
#define ORIC_MENU_NAME_LEN 48
#define ORIC_MENU_DRIVES   4
#define ORIC_MENU_FDCS     4   // Aucune, Pravetz 8D, Microdisc, Jasmin (ordre de oric_fdc_type_t)

enum { ORIC_FILE_DSK = 0, ORIC_FILE_TAP = 1 };

enum { ORIC_PAGE_MAIN = 0, ORIC_PAGE_BROWSE };

// Touches du menu (en plus des caractères imprimables, pour le saut)
enum {
    OSD_KEY_UP = 0x100, OSD_KEY_DOWN, OSD_KEY_LEFT, OSD_KEY_RIGHT, OSD_KEY_ENTER, OSD_KEY_ESC, OSD_KEY_DEL,
    OSD_KEY_PGUP, OSD_KEY_PGDN, OSD_KEY_HOME, OSD_KEY_END,
};

enum {
    ORIC_ACT_NONE = 0,
    ORIC_ACT_SET_FDC,      // file = interface (0-3)
    ORIC_ACT_INSERT,       // target = lecteur 0-3, file = index
    ORIC_ACT_EJECT,        // target = lecteur
    ORIC_ACT_TAPE_INSERT,  // file = index (la cassette en place : rembobinée)
    ORIC_ACT_TAPE_EJECT,
    ORIC_ACT_RESET,
    ORIC_ACT_SAVE,         // ORIC.CFG
    ORIC_ACT_RESUME,
};

typedef struct {
    int type;
    int target;
    int file;
} oric_menu_action_t;

typedef struct {
    char name[ORIC_MENU_NAME_LEN];
    uint32_t size;
    uint8_t kind;
} oric_menu_file_t;

// Éléments de la page principale
#define ORIC_ITEM_FDC    0
#define ORIC_ITEM_DRIVE0 1   // 1-4 : lecteurs A-D
#define ORIC_ITEM_TAPE   5
#define ORIC_ITEM_RESET  6
#define ORIC_ITEM_SAVE   7
#define ORIC_ITEM_RESUME 8
#define ORIC_ITEMS       9

static const char* const oric_menu_fdc_names[ORIC_MENU_FDCS] = {"Aucune", "Pravetz 8D", "Microdisc", "Jasmin"};
static const char* const oric_menu_fdc_notes[ORIC_MENU_FDCS] = {"BASIC seul", "images NIB (Disk II)", "Sedoric, .dsk",
                                                               "FT-DOS, .dsk"};

typedef struct {
    // --- Rempli par la plate-forme ---
    int fdc;                                        // Interface en place (0-3)
    bool fdc_available[ORIC_MENU_FDCS];             // ROM présente
    char drive[ORIC_MENU_DRIVES][ORIC_MENU_NAME_LEN];   // "" : vide
    bool drive_ro[ORIC_MENU_DRIVES];
    char tape[ORIC_MENU_NAME_LEN];                  // "" : aucune
    int tape_percent;
    bool tape_motor;
    bool usb_present;
    char usb_label[ORIC_MENU_NAME_LEN];
    oric_menu_file_t files[ORIC_MENU_FILES];
    int nfiles;
    char message[96];                               // Dernier résultat d'action
    bool message_error;
    const char* version;
    // --- Navigation ---
    int page;
    int cursor;
    int browse_target;   // Élément qui a ouvert le sélecteur
    int browse_cursor;   // Lecteurs et cassette : 0 = « éjecter », puis la liste
    int browse_scroll;
    int browse_list[ORIC_MENU_FILES];
    int browse_count;
} oric_menu_t;

static inline void oric_menu_init(oric_menu_t* m) {
    memset(m, 0, sizeof(*m));
    m->version = "";
    m->cursor = ORIC_ITEM_RESUME;
    m->fdc_available[0] = true;
}

static inline bool _oric_item_is_drive(int item) { return item >= ORIC_ITEM_DRIVE0 && item < ORIC_ITEM_DRIVE0 + 4; }

// Les lecteurs .dsk n'existent qu'avec le Microdisc et le Jasmin
static inline bool oric_menu_dsk_drives(const oric_menu_t* m) { return m->fdc == 2 || m->fdc == 3; }

#define ORIC_BROWSE_VISIBLE 16

// Le sélecteur de l'interface n'a pas de ligne « éjecter »
static inline int _oric_browse_first(const oric_menu_t* m) { return m->browse_target == ORIC_ITEM_FDC ? 0 : 1; }

static inline const char* _oric_entry_name(const oric_menu_t* m, int entry) {
    return m->browse_target == ORIC_ITEM_FDC ? oric_menu_fdc_names[entry] : m->files[entry].name;
}

static inline void _oric_open_browser(oric_menu_t* m, int item) {
    m->browse_count = 0;
    if (item == ORIC_ITEM_FDC) {
        for (int k = 0; k < ORIC_MENU_FDCS; k++) m->browse_list[m->browse_count++] = k;
        m->browse_cursor = m->fdc;
    } else {
        const uint8_t kind = item == ORIC_ITEM_TAPE ? ORIC_FILE_TAP : ORIC_FILE_DSK;
        for (int i = 0; i < m->nfiles && m->browse_count < ORIC_MENU_FILES; i++) {
            if (m->files[i].kind == kind) m->browse_list[m->browse_count++] = i;
        }
        m->browse_cursor = 0;
        // Curseur sur l'image ou la cassette en place
        const char* cur = item == ORIC_ITEM_TAPE ? m->tape : m->drive[item - ORIC_ITEM_DRIVE0];
        for (int k = 0; k < m->browse_count; k++) {
            if (cur[0] && !strcmp(m->files[m->browse_list[k]].name, cur)) m->browse_cursor = k + 1;
        }
    }
    m->browse_target = item;
    m->browse_scroll = 0;
    m->page = ORIC_PAGE_BROWSE;
}

static inline void _oric_browse_clamp(oric_menu_t* m) {
    const int n = m->browse_count + _oric_browse_first(m);
    if (m->browse_cursor < 0) m->browse_cursor = 0;
    if (m->browse_cursor >= n) m->browse_cursor = n - 1;
    if (m->browse_cursor < m->browse_scroll) m->browse_scroll = m->browse_cursor;
    if (m->browse_cursor >= m->browse_scroll + ORIC_BROWSE_VISIBLE) m->browse_scroll = m->browse_cursor - ORIC_BROWSE_VISIBLE + 1;
}

static inline int _oric_upper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

static inline void oric_menu_message(oric_menu_t* m, bool error, const char* text) {
    snprintf(m->message, sizeof(m->message), "%s", text);
    m->message_error = error;
}

static inline oric_menu_action_t oric_menu_key(oric_menu_t* m, int key) {
    oric_menu_action_t a = {ORIC_ACT_NONE, 0, -1};
    if (m->page == ORIC_PAGE_BROWSE) {
        const int first = _oric_browse_first(m);
        const int n = m->browse_count + first;
        switch (key) {
            case OSD_KEY_UP: m->browse_cursor = n ? (m->browse_cursor + n - 1) % n : 0; break;
            case OSD_KEY_DOWN: m->browse_cursor = n ? (m->browse_cursor + 1) % n : 0; break;
            case OSD_KEY_PGUP: m->browse_cursor -= ORIC_BROWSE_VISIBLE; break;
            case OSD_KEY_PGDN: m->browse_cursor += ORIC_BROWSE_VISIBLE; break;
            case OSD_KEY_HOME: m->browse_cursor = 0; break;
            case OSD_KEY_END: m->browse_cursor = n - 1; break;
            case OSD_KEY_ESC:
            case OSD_KEY_LEFT: m->page = ORIC_PAGE_MAIN; break;
            case OSD_KEY_ENTER: {
                const int item = m->browse_target;
                m->page = ORIC_PAGE_MAIN;
                if (item == ORIC_ITEM_FDC) {
                    const int t = m->browse_list[m->browse_cursor];
                    if (!m->fdc_available[t]) {
                        char msg[96];
                        snprintf(msg, sizeof(msg), "%s : ROM absente", oric_menu_fdc_names[t]);
                        oric_menu_message(m, true, msg);
                    } else {
                        a.type = ORIC_ACT_SET_FDC;
                        a.file = t;
                    }
                    break;
                }
                const bool none = m->browse_cursor == 0;
                const int file = none ? -1 : m->browse_list[m->browse_cursor - 1];
                if (_oric_item_is_drive(item)) {
                    a.target = item - ORIC_ITEM_DRIVE0;
                    if (!none) {
                        // Une image n'est que dans un lecteur à la fois
                        for (int d = 0; d < ORIC_MENU_DRIVES; d++) {
                            if (d != a.target && !strcmp(m->drive[d], m->files[file].name)) {
                                char msg[96];
                                snprintf(msg, sizeof(msg), "%.48s est déjà dans le lecteur %c", m->files[file].name, 'A' + d);
                                oric_menu_message(m, true, msg);
                                return a;
                            }
                        }
                    }
                    a.type = none ? ORIC_ACT_EJECT : ORIC_ACT_INSERT;
                } else {
                    a.type = none ? ORIC_ACT_TAPE_EJECT : ORIC_ACT_TAPE_INSERT;
                }
                a.file = file;
                break;
            }
            default:
                // Saut à l'élément suivant qui commence par la lettre tapée
                if (key > ' ' && key < 0x100 && m->browse_count > 0) {
                    for (int k = 1; k <= m->browse_count; k++) {
                        const int idx = (m->browse_cursor - first + k) % m->browse_count;
                        if (_oric_upper((uint8_t)_oric_entry_name(m, m->browse_list[idx])[0]) == _oric_upper(key)) {
                            m->browse_cursor = idx + first;
                            break;
                        }
                    }
                }
                break;
        }
        _oric_browse_clamp(m);
        return a;
    }
    int c = m->cursor;
    switch (key) {
        case OSD_KEY_UP: c = (c + ORIC_ITEMS - 1) % ORIC_ITEMS; break;
        case OSD_KEY_DOWN: c = (c + 1) % ORIC_ITEMS; break;
        case OSD_KEY_LEFT:
            if (c == ORIC_ITEM_TAPE) c = ORIC_ITEM_FDC;
            else if (c > ORIC_ITEM_RESET) c--;
            break;
        case OSD_KEY_RIGHT:
            if (c < ORIC_ITEM_TAPE) c = ORIC_ITEM_TAPE;
            else if (c >= ORIC_ITEM_RESET && c < ORIC_ITEM_RESUME) c++;
            break;
        case OSD_KEY_HOME: c = 0; break;
        case OSD_KEY_END: c = ORIC_ITEMS - 1; break;
        case OSD_KEY_ESC: a.type = ORIC_ACT_RESUME; break;
        case OSD_KEY_DEL:
            if (_oric_item_is_drive(c) && m->drive[c - ORIC_ITEM_DRIVE0][0]) {
                a.type = ORIC_ACT_EJECT;
                a.target = c - ORIC_ITEM_DRIVE0;
            } else if (c == ORIC_ITEM_TAPE && m->tape[0]) {
                a.type = ORIC_ACT_TAPE_EJECT;
            }
            break;
        case OSD_KEY_ENTER:
            if (c == ORIC_ITEM_FDC || c == ORIC_ITEM_TAPE) {
                _oric_open_browser(m, c);
            } else if (_oric_item_is_drive(c)) {
                if (oric_menu_dsk_drives(m)) {
                    _oric_open_browser(m, c);
                } else {
                    oric_menu_message(m, true, "Lecteurs .dsk : choisir l'interface Microdisc ou Jasmin");
                }
            } else if (c == ORIC_ITEM_RESET) {
                a.type = ORIC_ACT_RESET;
            } else if (c == ORIC_ITEM_SAVE) {
                a.type = ORIC_ACT_SAVE;
            } else {
                a.type = ORIC_ACT_RESUME;
            }
            break;
        default: break;
    }
    m->cursor = c;
    return a;
}

/*-- Dessin ------------------------------------------------------------------*/

#define ORIC_OSD_BG        OSD_ATTR(OSD_WHITE, OSD_BLACK)
#define ORIC_OSD_PANEL     OSD_ATTR(OSD_WHITE, OSD_BLUE | OSD_DITHER)
#define ORIC_OSD_PANEL_DIM OSD_ATTR(OSD_CYAN, OSD_BLUE | OSD_DITHER)
#define ORIC_OSD_PANEL_ACC OSD_ATTR(OSD_YELLOW, OSD_BLUE | OSD_DITHER)
#define ORIC_OSD_PANEL_OK  OSD_ATTR(OSD_GREEN, OSD_BLUE | OSD_DITHER)
#define ORIC_OSD_PANEL_ERR OSD_ATTR(OSD_RED, OSD_BLUE | OSD_DITHER)
#define ORIC_OSD_EDGE      OSD_ATTR(OSD_CYAN, OSD_BLUE | OSD_DITHER)
#define ORIC_OSD_SEL       OSD_ATTR(OSD_WHITE, OSD_BLUE)
#define ORIC_OSD_SEL_ACC   OSD_ATTR(OSD_YELLOW, OSD_BLUE)
#define ORIC_OSD_SEL_DIM   OSD_ATTR(OSD_CYAN, OSD_BLUE)

// Origine de la mise en page de 100 x 30 dans la grille
#define ORIC_OSD_R0 ((OSD_ROWS - 30) / 2)
#define ORIC_OSD_C0 ((OSD_COLS - 100) / 2)

// Panneau : fond tramé, cadre arrondi, icône et titre dans le filet du haut
static inline void _oric_panel(osd_surface_t* s, int row, int col, int rows, int cols, uint8_t icon, const char* title) {
    osd_fill(s, row, col, rows, cols, ORIC_OSD_PANEL);
    osd_frame(s, row, col, rows, cols, ORIC_OSD_EDGE);
    int c = col + 2;
    osd_putc(s, row, c++, ' ', ORIC_OSD_EDGE);
    if (icon) {
        osd_putc(s, row, c++, icon, ORIC_OSD_PANEL_ACC);
        osd_putc(s, row, c++, (uint8_t)(icon + 1), ORIC_OSD_PANEL_ACC);
        osd_putc(s, row, c++, ' ', ORIC_OSD_EDGE);
    }
    c += osd_puts(s, row, c, title, ORIC_OSD_PANEL, -1);
    osd_putc(s, row, c, ' ', ORIC_OSD_EDGE);
}

// Ligne d'élément : barre de sélection pleine, marqueur ▶
static inline void _oric_item_bar(osd_surface_t* s, int row, int col, int cols, bool selected) {
    osd_fill(s, row, col, 1, cols, selected ? ORIC_OSD_SEL : ORIC_OSD_PANEL);
    if (selected) osd_putc(s, row, col, OSD_TRI_R, ORIC_OSD_SEL_ACC);
}

static inline void _oric_size(char* buf, size_t n, uint32_t size) {
    if (size >= 1024 * 1024) {
        snprintf(buf, n, "%u,%u Mo", (unsigned)(size >> 20), (unsigned)((size % (1u << 20)) * 10 >> 20));
    } else {
        snprintf(buf, n, "%u Ko", (unsigned)((size + 1023) >> 10));
    }
}

static inline void _oric_button(osd_surface_t* s, int row, int col, int cols, const char* label, bool selected) {
    const uint8_t attr = selected ? OSD_ATTR(OSD_BLACK, OSD_CYAN) : ORIC_OSD_PANEL;
    osd_fill(s, row, col, 1, cols, attr);
    osd_puts(s, row, col + (cols - osd_strlen(label)) / 2, label, attr, -1);
}

static inline void oric_menu_draw(const oric_menu_t* m, osd_surface_t* s) {
    char buf[112];
    const int R = ORIC_OSD_R0, C = ORIC_OSD_C0;
    const bool main_page = m->page == ORIC_PAGE_MAIN;
    osd_clear(s, ORIC_OSD_BG);

    // Bandeau
    osd_fill(s, R, 0, 3, OSD_COLS, OSD_ATTR(OSD_WHITE, OSD_BLUE));
    osd_puts_big(s, R + 1, C + 3, "ORIC ATMOS", OSD_ATTR(OSD_WHITE, OSD_BLUE));
    osd_puts(s, R + 1, C + 26, "panneau de contrôle", OSD_ATTR(OSD_YELLOW, OSD_BLUE), -1);
    snprintf(buf, sizeof(buf), "Neo6502  %s", m->version ? m->version : "");
    osd_puts(s, R + 1, C + 97 - osd_strlen(buf), buf, OSD_ATTR(OSD_CYAN, OSD_BLUE), -1);
    for (int c = 0; c < OSD_COLS; c++) osd_putc(s, R + 3, c, OSD_HLINE, OSD_ATTR(OSD_CYAN, OSD_BLACK));

    // Interface disque
    _oric_panel(s, R + 5, C + 2, 3, 47, OSD_FLOP_L, "Interface disque");
    {
        const bool sel = main_page && m->cursor == ORIC_ITEM_FDC;
        _oric_item_bar(s, R + 6, C + 3, 45, sel);
        osd_puts(s, R + 6, C + 5, oric_menu_fdc_names[m->fdc], sel ? ORIC_OSD_SEL_ACC : ORIC_OSD_PANEL_ACC, -1);
        osd_puts(s, R + 6, C + 18, oric_menu_fdc_notes[m->fdc], sel ? ORIC_OSD_SEL_DIM : ORIC_OSD_PANEL_DIM, -1);
    }

    // Disquettes
    _oric_panel(s, R + 9, C + 2, 10, 47, OSD_FLOP_L, "Disquettes");
    for (int d = 0; d < ORIC_MENU_DRIVES; d++) {
        const int row = R + 11 + 2 * d;
        const bool sel = main_page && m->cursor == ORIC_ITEM_DRIVE0 + d;
        _oric_item_bar(s, row, C + 3, 45, sel);
        const uint8_t base = sel ? ORIC_OSD_SEL : ORIC_OSD_PANEL, dim = sel ? ORIC_OSD_SEL_DIM : ORIC_OSD_PANEL_DIM;
        snprintf(buf, sizeof(buf), "%c", 'A' + d);
        osd_puts(s, row, C + 5, buf, sel ? ORIC_OSD_SEL_ACC : ORIC_OSD_PANEL_ACC, -1);
        if (m->drive[d][0]) {
            osd_puts(s, row, C + 8, m->drive[d], base, 27);
            const bool ro = m->drive_ro[d];
            osd_putc(s, row, C + 36, ro ? OSD_LOCK : OSD_DOT,
                     ro ? (sel ? OSD_ATTR(OSD_RED, OSD_BLUE) : ORIC_OSD_PANEL_ERR)
                        : (sel ? OSD_ATTR(OSD_GREEN, OSD_BLUE) : ORIC_OSD_PANEL_OK));
            osd_puts(s, row, C + 38, ro ? "protégée" : "écriture", dim, -1);
        } else if (!oric_menu_dsk_drives(m) && d > 0) {
            osd_puts(s, row, C + 8, "—", dim, -1);
        } else {
            osd_puts(s, row, C + 8, "— vide —", dim, -1);
        }
    }

    // Cassette
    _oric_panel(s, R + 5, C + 51, 5, 47, OSD_TAPE_L, "Cassette");
    {
        const int row = R + 7;
        const bool sel = main_page && m->cursor == ORIC_ITEM_TAPE;
        _oric_item_bar(s, row, C + 52, 45, sel);
        const uint8_t base = sel ? ORIC_OSD_SEL : ORIC_OSD_PANEL, dim = sel ? ORIC_OSD_SEL_DIM : ORIC_OSD_PANEL_DIM;
        const uint8_t acc = sel ? ORIC_OSD_SEL_ACC : ORIC_OSD_PANEL_ACC;
        osd_putc(s, row, C + 54, OSD_TAPE_L, acc);
        osd_putc(s, row, C + 55, OSD_TAPE_R, acc);
        if (m->tape[0]) {
            osd_puts(s, row, C + 57, m->tape, base, 22);
            osd_putc(s, row, C + 80, m->tape_motor ? OSD_TRI_R : OSD_FULL, m->tape_motor ? acc : dim);
            snprintf(buf, sizeof(buf), "%3d %%", m->tape_percent);
            osd_puts(s, row, C + 82, buf, dim, -1);
            for (int i = 0; i < 8; i++) {
                osd_putc(s, row, C + 88 + i, i * 100 / 8 < m->tape_percent ? OSD_FULL : OSD_SHADE, dim);
            }
        } else {
            osd_puts(s, row, C + 57, "— pas de cassette —", dim, -1);
        }
        osd_puts(s, R + 8, C + 54, "CLOAD\"\" : la bande défile, moteur en marche", ORIC_OSD_PANEL_DIM, 43);
    }

    // Clé USB
    _oric_panel(s, R + 11, C + 51, 4, 47, OSD_USB_L, "Clé USB");
    if (m->usb_present) {
        int ndsk = 0, ntap = 0;
        for (int i = 0; i < m->nfiles; i++) {
            if (m->files[i].kind == ORIC_FILE_DSK) ndsk++;
            else ntap++;
        }
        osd_puts(s, R + 12, C + 54, m->usb_label[0] ? m->usb_label : "Clé montée", ORIC_OSD_PANEL, 42);
        snprintf(buf, sizeof(buf), "%d .dsk   %d .tap", ndsk, ntap);
        osd_puts(s, R + 13, C + 54, buf, ORIC_OSD_PANEL_DIM, -1);
    } else {
        osd_puts(s, R + 12, C + 54, "Aucune clé", ORIC_OSD_PANEL, -1);
        osd_puts(s, R + 13, C + 54, "Clé FAT : .dsk et .tap à la racine", ORIC_OSD_PANEL_DIM, -1);
    }

    // Aide
    osd_puts(s, R + 16, C + 52, "Suppr : éjecter   Échap : reprendre", ORIC_OSD_BG, -1);
    osd_puts(s, R + 17, C + 52, "ORIC.CFG (clé) : fdc=, a= … d=, tape=", OSD_ATTR(OSD_CYAN, OSD_BLACK), -1);

    // Actions
    const char* const labels[3] = {"Redémarrer (RESET)", "Enregistrer la configuration", "Reprendre"};
    const int cols[3] = {2, 34, 70}, widths[3] = {30, 34, 28};
    for (int i = 0; i < 3; i++) {
        _oric_button(s, R + 21, C + cols[i], widths[i], labels[i], main_page && m->cursor == ORIC_ITEM_RESET + i);
    }

    // Message
    if (m->message[0]) {
        osd_putc(s, R + 24, C + 3, m->message_error ? OSD_CROSS : OSD_CHECK,
                 OSD_ATTR(m->message_error ? OSD_RED : OSD_GREEN, OSD_BLACK));
        osd_puts(s, R + 24, C + 5, m->message, OSD_ATTR(m->message_error ? OSD_RED : OSD_YELLOW, OSD_BLACK), 93);
    }

    // Pied : aide des touches
    osd_fill(s, R + 28, 0, 2, OSD_COLS, ORIC_OSD_PANEL);
    const uint8_t key = OSD_ATTR(OSD_BLACK, OSD_CYAN);
    const char* const help_main[4][2] = {{" Flèches ", "choisir"}, {" Entrée ", "ouvrir, valider"}, {" Suppr ", "éjecter"},
                                         {" Échap ", "reprendre"}};
    const char* const help_browse[4][2] = {{" Flèches ", "choisir"}, {" Entrée ", "valider"}, {" Lettre ", "aller à"},
                                           {" Échap ", "retour"}};
    const char* const(*help)[2] = main_page ? help_main : help_browse;
    int c = C + 2;
    for (int i = 0; i < 4; i++) {
        c += osd_puts(s, R + 28, c, help[i][0], key, -1) + 1;
        c += osd_puts(s, R + 28, c, help[i][1], ORIC_OSD_PANEL, -1) + 2;
    }
    osd_puts(s, R + 28, C + 86, "F1 : ce menu", OSD_ATTR(OSD_CYAN, OSD_BLUE | OSD_DITHER), -1);

    if (main_page) return;

    // Sélecteur, par-dessus
    const int item = m->browse_target;
    const bool fdc = item == ORIC_ITEM_FDC, tape = item == ORIC_ITEM_TAPE;
    if (fdc) snprintf(buf, sizeof(buf), "Interface disque (la machine redémarre)");
    else if (tape) snprintf(buf, sizeof(buf), "Cassette (la même : rembobinée)");
    else snprintf(buf, sizeof(buf), "Disquette pour le lecteur %c", 'A' + item - ORIC_ITEM_DRIVE0);
    const int first = _oric_browse_first(m);
    const int top = R + 5, left = C + 12, width = 76, height = ORIC_BROWSE_VISIBLE + 4;
    osd_fill(s, top + 1, left + 2, height, width, ORIC_OSD_PANEL);   // Ombre
    osd_fill(s, top, left, height, width, OSD_ATTR(OSD_WHITE, OSD_BLACK));
    osd_frame(s, top, left, height, width, OSD_ATTR(OSD_YELLOW, OSD_BLACK));
    osd_putc(s, top, left + 2, ' ', OSD_ATTR(OSD_YELLOW, OSD_BLACK));
    const uint8_t icon = tape ? OSD_TAPE_L : OSD_FLOP_L;
    osd_putc(s, top, left + 3, icon, OSD_ATTR(OSD_YELLOW, OSD_BLACK));
    osd_putc(s, top, left + 4, (uint8_t)(icon + 1), OSD_ATTR(OSD_YELLOW, OSD_BLACK));
    const int tl = osd_puts(s, top, left + 6, buf, OSD_ATTR(OSD_WHITE, OSD_BLACK), -1);
    osd_putc(s, top, left + 6 + tl, ' ', OSD_ATTR(OSD_YELLOW, OSD_BLACK));
    const int n = m->browse_count + first;
    for (int k = 0; k < ORIC_BROWSE_VISIBLE && m->browse_scroll + k < n; k++) {
        const int idx = m->browse_scroll + k, row = top + 2 + k;
        const bool sel = idx == m->browse_cursor;
        const uint8_t base = sel ? OSD_ATTR(OSD_BLACK, OSD_CYAN) : OSD_ATTR(OSD_WHITE, OSD_BLACK);
        const uint8_t dim = sel ? OSD_ATTR(OSD_BLUE, OSD_CYAN) : OSD_ATTR(OSD_CYAN, OSD_BLACK);
        osd_fill(s, row, left + 2, 1, width - 5, base);
        if (idx < first) {
            osd_puts(s, row, left + 4, tape ? "Éjecter la cassette" : "Éjecter la disquette", dim, -1);
            continue;
        }
        const int entry = m->browse_list[idx - first];
        if (fdc) {
            osd_putc(s, row, left + 4, entry == m->fdc ? OSD_DOT : ' ', base);
            osd_puts(s, row, left + 6, oric_menu_fdc_names[entry], base, -1);
            osd_puts(s, row, left + 20, oric_menu_fdc_notes[entry], dim, -1);
            if (!m->fdc_available[entry]) {
                osd_puts(s, row, left + width - 17, "ROM absente", sel ? OSD_ATTR(OSD_RED, OSD_CYAN) : OSD_ATTR(OSD_RED, OSD_BLACK), -1);
            }
            continue;
        }
        const oric_menu_file_t* f = &m->files[entry];
        osd_puts(s, row, left + 4, f->name, base, 50);
        char size[16];
        _oric_size(size, sizeof(size), f->size);
        osd_puts(s, row, left + width - 5 - osd_strlen(size), size, dim, -1);
        // Image déjà dans un autre lecteur (la choisir est refusé)
        for (int d = 0; !tape && d < ORIC_MENU_DRIVES; d++) {
            if (d == item - ORIC_ITEM_DRIVE0 || strcmp(m->drive[d], f->name)) continue;
            char tag[8];
            snprintf(tag, sizeof(tag), "en %c", 'A' + d);
            osd_puts(s, row, left + width - 18, tag, sel ? OSD_ATTR(OSD_RED, OSD_CYAN) : OSD_ATTR(OSD_YELLOW, OSD_BLACK), -1);
        }
    }
    if (!fdc && m->browse_count == 0) {
        osd_puts(s, top + 4, left + 4, tape ? "Aucune cassette .tap sur la clé" : "Aucune image .dsk sur la clé",
                 OSD_ATTR(OSD_RED, OSD_BLACK), -1);
    }
    // Barre de défilement
    if (n > ORIC_BROWSE_VISIBLE) {
        const int bar = left + width - 2;
        for (int k = 0; k < ORIC_BROWSE_VISIBLE; k++) osd_putc(s, top + 2 + k, bar, OSD_SHADE, OSD_ATTR(OSD_BLUE, OSD_BLACK));
        const int thumb = m->browse_scroll * (ORIC_BROWSE_VISIBLE - 1) / (n - ORIC_BROWSE_VISIBLE);
        osd_putc(s, top + 2 + thumb, bar, OSD_FULL, OSD_ATTR(OSD_CYAN, OSD_BLACK));
    }
}
