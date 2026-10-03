#pragma once

// osd_browser.h — sélecteur commun des menus F1 : liste à défilement, touches, lettre
//
// Brique du socle, sans rien de propre à une machine (l'Oric s'en sert dans oric_menu.h). La
// liste a `count` entrées, précédées de `first` lignes fixes (0, ou 1 pour « Éjecter ») ;
// `cursor` va de 0 à first + count - 1, `scroll` est la première ligne affichée.
//
//   OSD_KEY_*, osd_key_from_hid   touches des menus, depuis les codes de hid_app.c
//   osd_browser_open    ouvre la liste (curseur donné, rendu visible)
//   osd_browser_key     Haut / Bas (en boucle), Page préc. / suiv., Début / Fin, Échap ou Gauche
//                       (retour), Entrée (choix), une lettre : entrée suivante qui commence
//                       par elle (nom donné par l'appelant, sans tenir compte de la casse)
//   osd_browser_entry   entrée choisie (-1 : une ligne fixe, par exemple « Éjecter »)
//   osd_browser_scrollbar  barre de défilement (osd_panel_scrollbar)
//
// Le dessin des lignes reste à l'appelant (osd_panel_popup, osd_panel_popup_row). À inclure
// après osd/osd.h.
//
// Copyright (c) 2026 bmarty — licence zlib/libpng (voir src/devices/neo_storage.h)

#include <stdbool.h>
#include <stdint.h>

#include "osd/osd.h"
#include "osd/osd_panel.h"

// Touches des menus (en plus des caractères imprimables, pour le saut à une lettre)
enum {
    OSD_KEY_UP = 0x100, OSD_KEY_DOWN, OSD_KEY_LEFT, OSD_KEY_RIGHT, OSD_KEY_ENTER, OSD_KEY_ESC, OSD_KEY_DEL,
    OSD_KEY_PGUP, OSD_KEY_PGDN, OSD_KEY_HOME, OSD_KEY_END,
};

// Code d'une touche du clavier USB (hid_app.c du socle : ASCII, ou usage HID | 0x100) en
// touche de menu ; -1 : touche sans rôle dans les menus
static inline int osd_key_from_hid(int code) {
    switch (code) {
        case 0x152: return OSD_KEY_UP;
        case 0x151: return OSD_KEY_DOWN;
        case 0x150: return OSD_KEY_LEFT;
        case 0x14F: return OSD_KEY_RIGHT;
        case 0x0D: return OSD_KEY_ENTER;
        case 0x1B: return OSD_KEY_ESC;
        case 0x7F:
        case 0x08: return OSD_KEY_DEL;
        case 0x14B: return OSD_KEY_PGUP;
        case 0x14E: return OSD_KEY_PGDN;
        case 0x14A: return OSD_KEY_HOME;
        case 0x14D: return OSD_KEY_END;
        default: return (code > ' ' && code < 0x7F) ? code : -1;
    }
}

typedef struct {
    int count;     // Entrées de la liste
    int first;     // Lignes fixes avant la liste (0 ou 1)
    int cursor;    // Ligne choisie, de 0 à first + count - 1
    int scroll;    // Première ligne affichée
    int visible;   // Lignes affichées à la fois
} osd_browser_t;

enum { OSD_BROWSER_NONE = 0, OSD_BROWSER_PICK, OSD_BROWSER_BACK };

// Nom de l'entrée `entry` (0 à count - 1), pour le saut à une lettre
typedef const char* (*osd_browser_name_fn)(void* user, int entry);

static inline int osd_browser_rows(const osd_browser_t* b) { return b->count + b->first; }

static inline void osd_browser_clamp(osd_browser_t* b) {
    const int n = osd_browser_rows(b);
    if (b->cursor >= n) b->cursor = n - 1;
    if (b->cursor < 0) b->cursor = 0;
    if (b->cursor < b->scroll) b->scroll = b->cursor;
    if (b->cursor >= b->scroll + b->visible) b->scroll = b->cursor - b->visible + 1;
}

// Liste ouverte sur la ligne `cursor`, rendue visible
static inline void osd_browser_open(osd_browser_t* b, int count, int first, int cursor, int visible) {
    b->count = count;
    b->first = first;
    b->cursor = cursor;
    b->scroll = 0;
    b->visible = visible;
    osd_browser_clamp(b);
}

// Entrée sous le curseur (0 à count - 1), ou -1 sur une ligne fixe
static inline int osd_browser_entry(const osd_browser_t* b) { return b->cursor - b->first; }

static inline int _osd_browser_upper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

// Touche dans la liste : OSD_BROWSER_PICK (Entrée), OSD_BROWSER_BACK (Échap, Gauche), sinon
// OSD_BROWSER_NONE (déplacement)
static inline int osd_browser_key(osd_browser_t* b, int key, osd_browser_name_fn name, void* user) {
    const int n = osd_browser_rows(b);
    int r = OSD_BROWSER_NONE;
    switch (key) {
        case OSD_KEY_UP: b->cursor = n ? (b->cursor + n - 1) % n : 0; break;
        case OSD_KEY_DOWN: b->cursor = n ? (b->cursor + 1) % n : 0; break;
        case OSD_KEY_PGUP: b->cursor -= b->visible; break;
        case OSD_KEY_PGDN: b->cursor += b->visible; break;
        case OSD_KEY_HOME: b->cursor = 0; break;
        case OSD_KEY_END: b->cursor = n - 1; break;
        case OSD_KEY_ESC:
        case OSD_KEY_LEFT: r = OSD_BROWSER_BACK; break;
        case OSD_KEY_ENTER: r = OSD_BROWSER_PICK; break;
        default:
            // Saut à l'entrée suivante qui commence par la lettre tapée
            if (key > ' ' && key < 0x100 && b->count > 0 && name) {
                for (int k = 1; k <= b->count; k++) {
                    const int idx = (b->cursor - b->first + k) % b->count;
                    const char* s = name(user, idx);
                    if (s && _osd_browser_upper((uint8_t)s[0]) == _osd_browser_upper(key)) {
                        b->cursor = idx + b->first;
                        break;
                    }
                }
            }
            break;
    }
    osd_browser_clamp(b);
    return r;
}

// Barre de défilement à la colonne col, à partir de la ligne row (rien si tout tient)
static inline void osd_browser_scrollbar(osd_surface_t* s, const osd_browser_t* b, int row, int col) {
    osd_panel_scrollbar(s, row, col, b->visible, b->scroll, osd_browser_rows(b));
}
