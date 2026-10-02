#pragma once

// oric_menu.h — panneau de contrôle (OSD) de l'Oric, du Pravetz et du Nova, sur le modèle du menu du
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
#include "osd/osd_panel.h"   // Style commun des panneaux (socle)
#include "devices/hid_media.h"   // HID_VOLUME_MAX

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
    ORIC_ACT_PROFILE,      // file = profil (index dans profile[])
    ORIC_ACT_VOLUME,       // volume et muted du menu modifiés
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
#define ORIC_ITEM_TAPE    5
#define ORIC_ITEM_PROFILE 6
#define ORIC_ITEM_VOLUME  7
#define ORIC_ITEM_RESET   8
#define ORIC_ITEM_SAVE    9
#define ORIC_ITEM_RESUME  10
#define ORIC_ITEMS        11
#define ORIC_MENU_PROFILES 8   // Profils proposés (intégrés puis de la clé)

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
    const char* profile[ORIC_MENU_PROFILES];        // Libellés des profils (NULL : fin)
    int profile_cur;                                // Profil appliqué (-1 : aucun)
    int volume;                                     // 0 à HID_VOLUME_MAX
    bool muted;
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
    m->profile_cur = -1;
    m->volume = HID_VOLUME_MAX;
}

// Page « Démarrer sur… » : le sélecteur des profils (demarrage=choix)
static inline void _oric_open_browser(oric_menu_t* m, int item);
static inline void oric_menu_open_profiles(oric_menu_t* m) { _oric_open_browser(m, ORIC_ITEM_PROFILE); }

static inline bool _oric_item_is_drive(int item) { return item >= ORIC_ITEM_DRIVE0 && item < ORIC_ITEM_DRIVE0 + 4; }

// Les lecteurs .dsk n'existent qu'avec le Microdisc et le Jasmin
static inline bool oric_menu_dsk_drives(const oric_menu_t* m) { return m->fdc == 2 || m->fdc == 3; }

#define ORIC_BROWSE_VISIBLE 16

// Les sélecteurs de l'interface et des profils n'ont pas de ligne « éjecter »
static inline int _oric_browse_first(const oric_menu_t* m) {
    return (m->browse_target == ORIC_ITEM_FDC || m->browse_target == ORIC_ITEM_PROFILE) ? 0 : 1;
}

static inline const char* _oric_entry_name(const oric_menu_t* m, int entry) {
    if (m->browse_target == ORIC_ITEM_FDC) return oric_menu_fdc_names[entry];
    if (m->browse_target == ORIC_ITEM_PROFILE) return m->profile[entry];
    return m->files[entry].name;
}

static inline void _oric_open_browser(oric_menu_t* m, int item) {
    m->browse_count = 0;
    if (item == ORIC_ITEM_FDC) {
        for (int k = 0; k < ORIC_MENU_FDCS; k++) m->browse_list[m->browse_count++] = k;
        m->browse_cursor = m->fdc;
    } else if (item == ORIC_ITEM_PROFILE) {
        for (int k = 0; k < ORIC_MENU_PROFILES && m->profile[k]; k++) m->browse_list[m->browse_count++] = k;
        m->browse_cursor = m->profile_cur >= 0 && m->profile_cur < m->browse_count ? m->profile_cur : 0;
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
                if (item == ORIC_ITEM_PROFILE) {
                    if (m->browse_count > 0) {
                        a.type = ORIC_ACT_PROFILE;
                        a.file = m->browse_list[m->browse_cursor];
                    }
                    break;
                }
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
    // Volume : Gauche / Droite règlent, Entrée coupe ou rétablit le son
    if (c == ORIC_ITEM_VOLUME && (key == OSD_KEY_LEFT || key == OSD_KEY_RIGHT || key == OSD_KEY_ENTER)) {
        if (key == OSD_KEY_ENTER) {
            m->muted = !m->muted;
        } else {
            m->muted = false;
            if (key == OSD_KEY_LEFT && m->volume > 0) m->volume--;
            if (key == OSD_KEY_RIGHT && m->volume < HID_VOLUME_MAX) m->volume++;
        }
        a.type = ORIC_ACT_VOLUME;
        return a;
    }
    switch (key) {
        case OSD_KEY_UP: c = (c + ORIC_ITEMS - 1) % ORIC_ITEMS; break;
        case OSD_KEY_DOWN: c = (c + 1) % ORIC_ITEMS; break;
        case OSD_KEY_LEFT:
            if (c == ORIC_ITEM_TAPE || c == ORIC_ITEM_PROFILE) c = ORIC_ITEM_FDC;
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
            if (c == ORIC_ITEM_FDC || c == ORIC_ITEM_TAPE || c == ORIC_ITEM_PROFILE) {
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

// Origine de la mise en page de 100 x 30 dans la grille
#define ORIC_OSD_R0 ((OSD_ROWS - 30) / 2)
#define ORIC_OSD_C0 ((OSD_COLS - 100) / 2)

static inline void oric_menu_draw(const oric_menu_t* m, osd_surface_t* s) {
    char buf[112];
    const int R = ORIC_OSD_R0, C = ORIC_OSD_C0;
    const bool main_page = m->page == ORIC_PAGE_MAIN;
    osd_clear(s, OSD_PANEL_BG);

    // Bandeau
    snprintf(buf, sizeof(buf), "Neo6502  %s", m->version ? m->version : "");
    osd_panel_header(s, R, C + 3, "ORIC / PRAVETZ / NOVA", C + 48, "panneau de contrôle", C + 97, buf);

    // Interface disque
    osd_panel_box(s, R + 5, C + 2, 3, 47, OSD_FLOP_L, "Interface disque");
    {
        const bool sel = main_page && m->cursor == ORIC_ITEM_FDC;
        osd_panel_item_bar(s, R + 6, C + 3, 45, sel);
        osd_puts(s, R + 6, C + 5, oric_menu_fdc_names[m->fdc], sel ? OSD_PANEL_SEL_ACC : OSD_PANEL_ACC, -1);
        osd_puts(s, R + 6, C + 18, oric_menu_fdc_notes[m->fdc], sel ? OSD_PANEL_SEL_DIM : OSD_PANEL_DIM, -1);
    }

    // Disquettes
    osd_panel_box(s, R + 9, C + 2, 10, 47, OSD_FLOP_L, "Disquettes");
    for (int d = 0; d < ORIC_MENU_DRIVES; d++) {
        const int row = R + 11 + 2 * d;
        const bool sel = main_page && m->cursor == ORIC_ITEM_DRIVE0 + d;
        osd_panel_item_bar(s, row, C + 3, 45, sel);
        const uint8_t base = sel ? OSD_PANEL_SEL : OSD_PANEL, dim = sel ? OSD_PANEL_SEL_DIM : OSD_PANEL_DIM;
        snprintf(buf, sizeof(buf), "%c", 'A' + d);
        osd_puts(s, row, C + 5, buf, sel ? OSD_PANEL_SEL_ACC : OSD_PANEL_ACC, -1);
        if (m->drive[d][0]) {
            osd_puts(s, row, C + 8, m->drive[d], base, 27);
            const bool ro = m->drive_ro[d];
            osd_putc(s, row, C + 36, ro ? OSD_LOCK : OSD_DOT,
                     ro ? osd_panel_err(sel) : osd_panel_ok(sel));
            osd_puts(s, row, C + 38, ro ? "protégée" : "écriture", dim, -1);
        } else if (!oric_menu_dsk_drives(m) && d > 0) {
            osd_puts(s, row, C + 8, "—", dim, -1);
        } else {
            osd_puts(s, row, C + 8, "— vide —", dim, -1);
        }
    }

    // Cassette
    osd_panel_box(s, R + 5, C + 51, 5, 47, OSD_TAPE_L, "Cassette");
    {
        const int row = R + 7;
        const bool sel = main_page && m->cursor == ORIC_ITEM_TAPE;
        osd_panel_item_bar(s, row, C + 52, 45, sel);
        const uint8_t base = sel ? OSD_PANEL_SEL : OSD_PANEL, dim = sel ? OSD_PANEL_SEL_DIM : OSD_PANEL_DIM;
        const uint8_t acc = sel ? OSD_PANEL_SEL_ACC : OSD_PANEL_ACC;
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
        osd_puts(s, R + 8, C + 54, "CLOAD\"\" : la bande défile, moteur en marche", OSD_PANEL_DIM, 43);
    }

    // Clé USB
    osd_panel_box(s, R + 11, C + 51, 4, 47, OSD_USB_L, "Clé USB");
    if (m->usb_present) {
        int ndsk = 0, ntap = 0;
        for (int i = 0; i < m->nfiles; i++) {
            if (m->files[i].kind == ORIC_FILE_DSK) ndsk++;
            else ntap++;
        }
        osd_puts(s, R + 12, C + 54, m->usb_label[0] ? m->usb_label : "Clé montée", OSD_PANEL, 42);
        snprintf(buf, sizeof(buf), "%d .dsk   %d .tap", ndsk, ntap);
        osd_puts(s, R + 13, C + 54, buf, OSD_PANEL_DIM, -1);
    } else {
        osd_puts(s, R + 12, C + 54, "Aucune clé", OSD_PANEL, -1);
        osd_puts(s, R + 13, C + 54, "Clé FAT : .dsk et .tap à la racine", OSD_PANEL_DIM, -1);
    }

    // Profil
    osd_panel_box(s, R + 16, C + 51, 3, 47, OSD_CART_L, "Profil");
    {
        const bool sel = main_page && m->cursor == ORIC_ITEM_PROFILE;
        osd_panel_item_bar(s, R + 17, C + 52, 45, sel);
        const bool known = m->profile_cur >= 0 && m->profile_cur < ORIC_MENU_PROFILES && m->profile[m->profile_cur];
        osd_puts(s, R + 17, C + 54, known ? m->profile[m->profile_cur] : "— réglages de l'utilisateur —",
                 known ? (sel ? OSD_PANEL_SEL_ACC : OSD_PANEL_ACC) : (sel ? OSD_PANEL_SEL_DIM : OSD_PANEL_DIM), 42);
    }

    // Son
    osd_panel_box(s, R + 20, C + 2, 3, 96, 0, "Son");
    {
        const bool sel = main_page && m->cursor == ORIC_ITEM_VOLUME;
        osd_panel_item_bar(s, R + 21, C + 3, 94, sel);
        const uint8_t base = sel ? OSD_PANEL_SEL : OSD_PANEL, dim = sel ? OSD_PANEL_SEL_DIM : OSD_PANEL_DIM;
        const uint8_t acc = sel ? OSD_PANEL_SEL_ACC : OSD_PANEL_ACC;
        osd_puts(s, R + 21, C + 5, "Volume", acc, -1);
        if (m->muted) {
            osd_putc(s, R + 21, C + 14, OSD_CROSS, osd_panel_err(sel));
            osd_puts(s, R + 21, C + 16, "son coupé", base, -1);
        } else {
            for (int i = 0; i < HID_VOLUME_MAX; i++) {
                osd_putc(s, R + 21, C + 14 + 2 * i, i < m->volume ? OSD_FULL : OSD_SHADE, dim);
            }
            snprintf(buf, sizeof(buf), "%d/%d", m->volume, HID_VOLUME_MAX);
            osd_puts(s, R + 21, C + 15 + 2 * HID_VOLUME_MAX, buf, acc, -1);
        }
        osd_puts(s, R + 21, C + 52, "Gauche / Droite : régler, Entrée : couper", dim, -1);
    }

    // Actions
    const char* const labels[3] = {"Redémarrer (RESET)", "Enregistrer la configuration", "Reprendre"};
    const int cols[3] = {2, 34, 70}, widths[3] = {30, 34, 28};
    for (int i = 0; i < 3; i++) {
        osd_panel_button(s, R + 24, C + cols[i], widths[i], labels[i], main_page && m->cursor == ORIC_ITEM_RESET + i);
    }

    // Message
    if (m->message[0]) osd_panel_message(s, R + 26, C + 3, m->message_error, m->message, 93);

    // Pied : aide des touches
    const char* const help_main[4][2] = {{" Flèches ", "choisir"}, {" Entrée ", "ouvrir, valider"}, {" Suppr ", "éjecter"},
                                         {" Échap ", "reprendre"}};
    const char* const help_browse[4][2] = {{" Flèches ", "choisir"}, {" Entrée ", "valider"}, {" Lettre ", "aller à"},
                                           {" Échap ", "retour"}};
    osd_panel_footer(s, R + 28, C + 2, main_page ? help_main : help_browse, 4, C + 86, "F1 : ce menu");

    if (main_page) return;

    // Sélecteur, par-dessus
    const int item = m->browse_target;
    const bool fdc = item == ORIC_ITEM_FDC, tape = item == ORIC_ITEM_TAPE, prof = item == ORIC_ITEM_PROFILE;
    if (prof) snprintf(buf, sizeof(buf), "Démarrer sur… (la machine redémarre)");
    else if (fdc) snprintf(buf, sizeof(buf), "Interface disque (la machine redémarre)");
    else if (tape) snprintf(buf, sizeof(buf), "Cassette (la même : rembobinée)");
    else snprintf(buf, sizeof(buf), "Disquette pour le lecteur %c", 'A' + item - ORIC_ITEM_DRIVE0);
    const int first = _oric_browse_first(m);
    const int top = R + 5, left = C + 12, width = 76, height = ORIC_BROWSE_VISIBLE + 4;
    osd_panel_popup(s, top, left, height, width, tape ? OSD_TAPE_L : prof ? OSD_CART_L : OSD_FLOP_L, buf);
    const int n = m->browse_count + first;
    for (int k = 0; k < ORIC_BROWSE_VISIBLE && m->browse_scroll + k < n; k++) {
        const int idx = m->browse_scroll + k, row = top + 2 + k;
        const bool sel = idx == m->browse_cursor;
        uint8_t base, dim;
        osd_panel_popup_row(s, row, left, width, sel, &base, &dim);
        if (idx < first) {
            osd_puts(s, row, left + 4, tape ? "Éjecter la cassette" : "Éjecter la disquette", dim, -1);
            continue;
        }
        const int entry = m->browse_list[idx - first];
        if (prof) {
            osd_putc(s, row, left + 4, entry == m->profile_cur ? OSD_DOT : ' ', base);
            osd_puts(s, row, left + 6, m->profile[entry], base, width - 10);
            continue;
        }
        if (fdc) {
            osd_putc(s, row, left + 4, entry == m->fdc ? OSD_DOT : ' ', base);
            osd_puts(s, row, left + 6, oric_menu_fdc_names[entry], base, -1);
            osd_puts(s, row, left + 20, oric_menu_fdc_notes[entry], dim, -1);
            if (!m->fdc_available[entry]) {
                osd_puts(s, row, left + width - 17, "ROM absente", sel ? OSD_PANEL_POPUP_SEL_ERR : OSD_PANEL_POPUP_ERR, -1);
            }
            continue;
        }
        const oric_menu_file_t* f = &m->files[entry];
        osd_puts(s, row, left + 4, f->name, base, 50);
        char size[16];
        osd_panel_size(size, sizeof(size), f->size);
        osd_puts(s, row, left + width - 5 - osd_strlen(size), size, dim, -1);
        // Image déjà dans un autre lecteur (la choisir est refusé)
        for (int d = 0; !tape && d < ORIC_MENU_DRIVES; d++) {
            if (d == item - ORIC_ITEM_DRIVE0 || strcmp(m->drive[d], f->name)) continue;
            char tag[8];
            snprintf(tag, sizeof(tag), "en %c", 'A' + d);
            osd_puts(s, row, left + width - 18, tag, sel ? OSD_PANEL_POPUP_SEL_ERR : OSD_PANEL_POPUP_ACC, -1);
        }
    }
    if (!fdc && !prof && m->browse_count == 0) {
        osd_puts(s, top + 4, left + 4, tape ? "Aucune cassette .tap sur la clé" : "Aucune image .dsk sur la clé",
                 OSD_PANEL_POPUP_ERR, -1);
    }
    osd_panel_scrollbar(s, top + 2, left + width - 2, ORIC_BROWSE_VISIBLE, m->browse_scroll, n);
}

// Bandeau du volume (touches multimédia du clavier), à la place de celui de la
// cassette pendant 2 s : jauge, ou « Son coupé » (comme au Telestrat)
static inline void oric_volume_banner(osd_row_t* r, int level, bool muted) {
    const uint8_t base = OSD_ATTR(OSD_WHITE, OSD_BLUE);
    const uint8_t acc = OSD_ATTR(OSD_YELLOW, OSD_BLUE);
    osd_row_clear(r, base);
    const int C = ORIC_OSD_C0;
    osd_row_puts(r, C + 13, "Volume", acc);
    if (muted) {
        r->ch[C + 22] = OSD_CROSS;
        r->attr[C + 22] = OSD_ATTR(OSD_RED, OSD_BLUE);
        osd_row_puts(r, C + 24, "Son coupé", base);
        return;
    }
    for (int i = 0; i < HID_VOLUME_MAX; i++) {
        r->ch[C + 22 + 2 * i] = i < level ? OSD_FULL : OSD_SHADE;
        r->attr[C + 22 + 2 * i] = OSD_ATTR(OSD_CYAN, OSD_BLUE);
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "%d/%d", level, HID_VOLUME_MAX);
    osd_row_puts(r, C + 23 + 2 * HID_VOLUME_MAX, buf, acc);
}

// Bandeau de la cassette, incrusté sous l'image de l'Oric pendant que le
// moteur tourne (comme au Telestrat) : ▶, icône, nom, barre, pour cent
static inline void oric_tape_banner(osd_row_t* r, const char* name, int percent) {
    const uint8_t base = OSD_ATTR(OSD_WHITE, OSD_BLUE);
    const uint8_t acc = OSD_ATTR(OSD_YELLOW, OSD_BLUE);
    osd_row_clear(r, base);
    const int C = ORIC_OSD_C0;
    r->ch[C + 8] = OSD_TRI_R;
    r->attr[C + 8] = acc;
    r->ch[C + 10] = OSD_TAPE_L;
    r->ch[C + 11] = OSD_TAPE_R;
    r->attr[C + 10] = r->attr[C + 11] = acc;
    osd_row_puts(r, C + 13, "Lecture", acc);
    char buf[64];
    snprintf(buf, sizeof(buf), "%.30s", name);
    osd_row_puts(r, C + 22, buf, base);
    const int bar = C + 56, cells = 30;
    for (int i = 0; i < cells; i++) {
        r->ch[bar + i] = i * 100 / cells < percent ? OSD_FULL : OSD_SHADE;
        r->attr[bar + i] = OSD_ATTR(OSD_CYAN, OSD_BLUE);
    }
    snprintf(buf, sizeof(buf), "%3d %%", percent);
    osd_row_puts(r, bar + cells + 2, buf, acc);
}
