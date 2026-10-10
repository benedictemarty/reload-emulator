#pragma once

// bbc_menu.h — panneau de contrôle (OSD) du BBC Micro, sur le modèle du menu du
// Telestrat (projet Neo6502TeleStrat) et du panneau de l'Oric (oric_menu.h)
//
// Page principale : lecteurs 0 et 1 (image, origine clé USB ou flash, écriture),
// clé USB, images en flash, démarrage à l'insertion (SHIFT+BREAK), volume,
// boutons BREAK / Démarrer le disque / Enregistrer la configuration / Reprendre.
// Entrée sur un lecteur : sélecteur des images (clé USB puis flash) avec
// défilement et saut à l'initiale tapée ; Suppr éjecte. La plate-forme remplit
// l'état (lecteurs, fichiers, clé, message) et exécute les actions rendues par
// bbc_menu_key().
//
// Mise en page pour une grille de 100 x 30 cellules (osd.h). Indépendant de la
// plate-forme (testé par tests/bbc/test_bbc_menu.c).
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
#include "osd/osd_panel.h"     // Style commun des panneaux (socle)
#include "osd/osd_browser.h"   // Sélecteur commun (socle)
#include "devices/hid_media.h" // HID_VOLUME_MAX

#ifndef BBC_MENU_FILES
#define BBC_MENU_FILES 64   // Images listées (clé USB puis flash)
#endif
#define BBC_MENU_NAME_LEN 40
#define BBC_MENU_DRIVES   2

// Origine d'une image
enum { BBC_SRC_USB = 0, BBC_SRC_FLASH = 1 };

enum { BBC_PAGE_MAIN = 0, BBC_PAGE_BROWSE };

enum {
    BBC_ACT_NONE = 0,
    BBC_ACT_INSERT,     // target = lecteur 0-1, file = index dans files[]
    BBC_ACT_EJECT,      // target = lecteur
    BBC_ACT_BREAK,      // BREAK (le disque reste en place)
    BBC_ACT_BOOT,       // SHIFT+BREAK : démarre le disque du lecteur 0
    BBC_ACT_SAVE,       // BBC.CFG
    BBC_ACT_RESUME,
    BBC_ACT_VOLUME,     // volume et muted du menu modifiés
    BBC_ACT_AUTOBOOT,   // autoboot du menu modifié
};

typedef struct {
    int type;
    int target;
    int file;
} bbc_menu_action_t;

typedef struct {
    const char* name;   // Kept by the platform (no copy: RAM of the RP2040)
    uint32_t size;
    uint8_t src;   // BBC_SRC_*
} bbc_menu_file_t;

// Éléments de la page principale
#define BBC_ITEM_DRIVE0   0   // 0-1 : lecteurs 0 et 1
#define BBC_ITEM_AUTOBOOT 2
#define BBC_ITEM_VOLUME   3
#define BBC_ITEM_BREAK    4
#define BBC_ITEM_BOOT     5
#define BBC_ITEM_SAVE     6
#define BBC_ITEM_RESUME   7
#define BBC_ITEMS         8

typedef struct {
    // --- Rempli par la plate-forme ---
    char drive[BBC_MENU_DRIVES][BBC_MENU_NAME_LEN];   // "" : vide
    uint8_t drive_src[BBC_MENU_DRIVES];
    bool drive_ro[BBC_MENU_DRIVES];
    bool usb_present;
    char usb_label[BBC_MENU_NAME_LEN];
    bbc_menu_file_t files[BBC_MENU_FILES];
    int nfiles;
    bool autoboot;   // Insertion dans le lecteur 0 : SHIFT+BREAK
    int volume;      // 0 à HID_VOLUME_MAX
    bool muted;
    char message[96];   // Dernier résultat d'action
    bool message_error;
    const char* model;     // "Model B", "Master 128"
    const char* version;
    // --- Navigation ---
    int page;
    int cursor;
    int browse_target;   // Lecteur qui a ouvert le sélecteur
    osd_browser_t browse;   // Ligne 0 = « éjecter », puis la liste
} bbc_menu_t;

static inline void bbc_menu_init(bbc_menu_t* m) {
    memset(m, 0, sizeof(*m));
    m->version = "";
    m->model = "Model B";
    m->cursor = BBC_ITEM_RESUME;
    m->autoboot = true;
    m->volume = HID_VOLUME_MAX;
}

#define BBC_BROWSE_VISIBLE 16

static inline void bbc_menu_message(bbc_menu_t* m, bool error, const char* text) {
    snprintf(m->message, sizeof(m->message), "%s", text);
    m->message_error = error;
}

static inline void _bbc_open_browser(bbc_menu_t* m, int drive) {
    int cursor = 0;
    // Curseur sur l'image en place
    for (int i = 0; i < m->nfiles; i++) {
        if (m->drive[drive][0] && m->drive_src[drive] == m->files[i].src && !strcmp(m->files[i].name, m->drive[drive])) {
            cursor = i + 1;
        }
    }
    m->browse_target = drive;
    osd_browser_open(&m->browse, m->nfiles, 1, cursor, BBC_BROWSE_VISIBLE);
    m->page = BBC_PAGE_BROWSE;
}

// Nom d'une entrée du sélecteur (saut à une lettre)
static inline const char* _bbc_browse_name(void* user, int k) {
    const bbc_menu_t* m = (const bbc_menu_t*)user;
    return m->files[k].name;
}

static inline bbc_menu_action_t bbc_menu_key(bbc_menu_t* m, int key) {
    bbc_menu_action_t a = {BBC_ACT_NONE, 0, -1};
    if (m->page == BBC_PAGE_BROWSE) {
        const int r = osd_browser_key(&m->browse, key, _bbc_browse_name, m);
        if (r == OSD_BROWSER_BACK) m->page = BBC_PAGE_MAIN;
        if (r != OSD_BROWSER_PICK) return a;
        const int entry = osd_browser_entry(&m->browse);
        m->page = BBC_PAGE_MAIN;
        a.target = m->browse_target;
        if (entry < 0) {
            if (m->drive[a.target][0]) a.type = BBC_ACT_EJECT;
            return a;
        }
        // Une image n'est que dans un lecteur à la fois
        const int other = 1 - a.target;
        if (m->drive[other][0] && m->drive_src[other] == m->files[entry].src &&
            !strcmp(m->drive[other], m->files[entry].name)) {
            char msg[96];
            snprintf(msg, sizeof(msg), "%.40s est déjà dans le lecteur %d", m->files[entry].name, other);
            bbc_menu_message(m, true, msg);
            return a;
        }
        a.type = BBC_ACT_INSERT;
        a.file = entry;
        return a;
    }
    int c = m->cursor;
    // Volume : Gauche / Droite règlent, Entrée coupe ou rétablit le son
    if (c == BBC_ITEM_VOLUME && (key == OSD_KEY_LEFT || key == OSD_KEY_RIGHT || key == OSD_KEY_ENTER)) {
        if (key == OSD_KEY_ENTER) {
            m->muted = !m->muted;
        } else {
            m->muted = false;
            if (key == OSD_KEY_LEFT && m->volume > 0) m->volume--;
            if (key == OSD_KEY_RIGHT && m->volume < HID_VOLUME_MAX) m->volume++;
        }
        a.type = BBC_ACT_VOLUME;
        return a;
    }
    switch (key) {
        case OSD_KEY_UP: c = (c + BBC_ITEMS - 1) % BBC_ITEMS; break;
        case OSD_KEY_DOWN: c = (c + 1) % BBC_ITEMS; break;
        case OSD_KEY_LEFT:
            if (c > BBC_ITEM_BREAK) c--;
            break;
        case OSD_KEY_RIGHT:
            if (c >= BBC_ITEM_BREAK && c < BBC_ITEM_RESUME) c++;
            break;
        case OSD_KEY_HOME: c = 0; break;
        case OSD_KEY_END: c = BBC_ITEMS - 1; break;
        case OSD_KEY_ESC: a.type = BBC_ACT_RESUME; break;
        case OSD_KEY_DEL:
            if (c < BBC_MENU_DRIVES && m->drive[c][0]) {
                a.type = BBC_ACT_EJECT;
                a.target = c;
            }
            break;
        case OSD_KEY_ENTER:
            if (c < BBC_MENU_DRIVES) {
                _bbc_open_browser(m, c);
            } else if (c == BBC_ITEM_AUTOBOOT) {
                m->autoboot = !m->autoboot;
                a.type = BBC_ACT_AUTOBOOT;
            } else if (c == BBC_ITEM_BREAK) {
                a.type = BBC_ACT_BREAK;
            } else if (c == BBC_ITEM_BOOT) {
                a.type = BBC_ACT_BOOT;
            } else if (c == BBC_ITEM_SAVE) {
                a.type = BBC_ACT_SAVE;
            } else {
                a.type = BBC_ACT_RESUME;
            }
            break;
        default: break;
    }
    m->cursor = c;
    return a;
}

/*-- Dessin ------------------------------------------------------------------*/

// Origine de la mise en page de 100 x 30 dans la grille
#define BBC_OSD_R0 ((OSD_ROWS - 30) / 2)
#define BBC_OSD_C0 ((OSD_COLS - 100) / 2)

static inline const char* _bbc_src_name(uint8_t src) { return src == BBC_SRC_FLASH ? "flash" : "clé"; }

static inline void bbc_menu_draw(const bbc_menu_t* m, osd_surface_t* s) {
    char buf[112];
    const int R = BBC_OSD_R0, C = BBC_OSD_C0;
    const bool main_page = m->page == BBC_PAGE_MAIN;
    osd_clear(s, OSD_PANEL_BG);

    // Bandeau
    snprintf(buf, sizeof(buf), "Neo6502  %s", m->version ? m->version : "");
    osd_panel_header(s, R, C + 3, "BBC MICRO", C + 24, m->model ? m->model : "", C + 97, buf);

    // Disques
    osd_panel_box(s, R + 5, C + 2, 7, 47, OSD_FLOP_L, "Disques");
    for (int d = 0; d < BBC_MENU_DRIVES; d++) {
        const int row = R + 7 + 2 * d;
        const bool sel = main_page && m->cursor == BBC_ITEM_DRIVE0 + d;
        osd_panel_item_bar(s, row, C + 3, 45, sel);
        const uint8_t base = osd_panel_base(sel), dim = osd_panel_dim(sel);
        snprintf(buf, sizeof(buf), "%d", d);
        osd_puts(s, row, C + 5, buf, osd_panel_acc(sel), -1);
        if (m->drive[d][0]) {
            osd_puts(s, row, C + 8, m->drive[d], base, 22);
            osd_puts(s, row, C + 31, _bbc_src_name(m->drive_src[d]), dim, -1);
            const bool ro = m->drive_ro[d];
            osd_putc(s, row, C + 37, ro ? OSD_LOCK : OSD_DOT, ro ? osd_panel_err(sel) : osd_panel_ok(sel));
            osd_puts(s, row, C + 39, ro ? "protégé" : "écriture", dim, -1);
        } else {
            osd_puts(s, row, C + 8, "— vide —", dim, -1);
        }
    }

    // Clé USB et images en flash
    int nusb = 0, nflash = 0;
    for (int i = 0; i < m->nfiles; i++) {
        if (m->files[i].src == BBC_SRC_FLASH) nflash++;
        else nusb++;
    }
    osd_panel_box(s, R + 5, C + 51, 4, 47, OSD_USB_L, "Clé USB");
    if (m->usb_present) {
        osd_puts(s, R + 6, C + 54, m->usb_label[0] ? m->usb_label : "Clé montée", OSD_PANEL, 42);
        snprintf(buf, sizeof(buf), "%d image%s .ssd / .dsd", nusb, nusb > 1 ? "s" : "");
        osd_puts(s, R + 7, C + 54, buf, OSD_PANEL_DIM, -1);
    } else {
        osd_puts(s, R + 6, C + 54, "Aucune clé", OSD_PANEL, -1);
        osd_puts(s, R + 7, C + 54, "Clé FAT : .ssd et .dsd à la racine", OSD_PANEL_DIM, -1);
    }
    osd_panel_box(s, R + 10, C + 51, 3, 47, OSD_CART_L, "Images en flash");
    snprintf(buf, sizeof(buf), "%d image%s intégrée%s au firmware", nflash, nflash > 1 ? "s" : "", nflash > 1 ? "s" : "");
    osd_puts(s, R + 11, C + 54, nflash ? buf : "aucune", OSD_PANEL_DIM, -1);

    // Démarrage
    osd_panel_box(s, R + 13, C + 2, 3, 96, 0, "Démarrage");
    {
        const bool sel = main_page && m->cursor == BBC_ITEM_AUTOBOOT;
        osd_panel_item_bar(s, R + 14, C + 3, 94, sel);
        osd_puts(s, R + 14, C + 5, "Insertion dans le lecteur 0", osd_panel_acc(sel), -1);
        osd_putc(s, R + 14, C + 34, m->autoboot ? OSD_CHECK : OSD_CROSS,
                 m->autoboot ? osd_panel_ok(sel) : osd_panel_err(sel));
        osd_puts(s, R + 14, C + 36, m->autoboot ? "démarrer le disque (SHIFT+BREAK)" : "insérer seulement",
                 osd_panel_base(sel), -1);
        osd_puts(s, R + 14, C + 78, "Entrée : changer", osd_panel_dim(sel), -1);
    }

    // Son
    osd_panel_box(s, R + 17, C + 2, 3, 96, 0, "Son");
    {
        const bool sel = main_page && m->cursor == BBC_ITEM_VOLUME;
        osd_panel_item_bar(s, R + 18, C + 3, 94, sel);
        const uint8_t base = osd_panel_base(sel), dim = osd_panel_dim(sel), acc = osd_panel_acc(sel);
        osd_puts(s, R + 18, C + 5, "Volume", acc, -1);
        if (m->muted) {
            osd_putc(s, R + 18, C + 14, OSD_CROSS, osd_panel_err(sel));
            osd_puts(s, R + 18, C + 16, "son coupé", base, -1);
        } else {
            for (int i = 0; i < HID_VOLUME_MAX; i++) {
                osd_putc(s, R + 18, C + 14 + 2 * i, i < m->volume ? OSD_FULL : OSD_SHADE, dim);
            }
            snprintf(buf, sizeof(buf), "%d/%d", m->volume, HID_VOLUME_MAX);
            osd_puts(s, R + 18, C + 15 + 2 * HID_VOLUME_MAX, buf, acc, -1);
        }
        osd_puts(s, R + 18, C + 52, "Gauche / Droite : régler, Entrée : couper", dim, -1);
    }

    // Actions
    const char* const labels[4] = {"BREAK", "Démarrer (SHIFT+BREAK)", "Enregistrer la config.", "Reprendre"};
    const int cols[4] = {2, 22, 50, 78}, widths[4] = {18, 26, 26, 20};
    for (int i = 0; i < 4; i++) {
        osd_panel_button(s, R + 22, C + cols[i], widths[i], labels[i], main_page && m->cursor == BBC_ITEM_BREAK + i);
    }

    // Message
    if (m->message[0]) osd_panel_message(s, R + 25, C + 3, m->message_error, m->message, 93);

    // Pied : aide des touches
    const char* const help_main[4][2] = {{" Flèches ", "choisir"}, {" Entrée ", "ouvrir, valider"}, {" Suppr ", "éjecter"},
                                         {" Échap ", "reprendre"}};
    const char* const help_browse[4][2] = {{" Flèches ", "choisir"}, {" Entrée ", "insérer"}, {" Lettre ", "aller à"},
                                           {" Échap ", "retour"}};
    osd_panel_footer(s, R + 28, C + 2, main_page ? help_main : help_browse, 4, C + 85, "F11 : ce menu");

    if (main_page) return;

    // Sélecteur, par-dessus
    const int drive = m->browse_target;
    snprintf(buf, sizeof(buf), "Disque pour le lecteur %d", drive);
    const int first = m->browse.first;
    const int top = R + 5, left = C + 12, width = 76, height = BBC_BROWSE_VISIBLE + 4;
    osd_panel_popup(s, top, left, height, width, OSD_FLOP_L, buf);
    const int n = osd_browser_rows(&m->browse);
    for (int k = 0; k < BBC_BROWSE_VISIBLE && m->browse.scroll + k < n; k++) {
        const int idx = m->browse.scroll + k, row = top + 2 + k;
        const bool sel = idx == m->browse.cursor;
        uint8_t base, dim;
        osd_panel_popup_row(s, row, left, width, sel, &base, &dim);
        if (idx < first) {
            osd_puts(s, row, left + 4, "Éjecter le disque", dim, -1);
            continue;
        }
        const bbc_menu_file_t* f = &m->files[idx - first];
        osd_puts(s, row, left + 4, f->name, base, 40);
        osd_puts(s, row, left + 46, _bbc_src_name(f->src), dim, -1);
        char size[16];
        osd_panel_size(size, sizeof(size), f->size);
        osd_puts(s, row, left + width - 5 - osd_strlen(size), size, dim, -1);
        // Image déjà dans l'autre lecteur (la choisir est refusé)
        const int other = 1 - drive;
        if (m->drive[other][0] && m->drive_src[other] == f->src && !strcmp(m->drive[other], f->name)) {
            snprintf(size, sizeof(size), "en %d", other);
            osd_puts(s, row, left + width - 18, size, sel ? OSD_PANEL_POPUP_SEL_ERR : OSD_PANEL_POPUP_ACC, -1);
        }
    }
    if (m->browse.count == 0) {
        osd_puts(s, top + 4, left + 4, "Aucune image .ssd / .dsd (clé USB ou flash)", OSD_PANEL_POPUP_ERR, -1);
    }
    osd_browser_scrollbar(s, &m->browse, top + 2, left + width - 2);
}

// Ligne du menu (0 .. OSD_LINES - 1, chacune sur deux lignes de sortie) montrée par
// le tampon de la ligne BBC y (0..255) à 800x480, où la dernière ligne BBC de chaque
// rangée de 8 n'occupe qu'une ligne de sortie (7 x 2 + 1 = 15 lignes par rangée) :
// la ligne de menu qui couvre la première ligne de sortie du tampon
static inline int bbc_menu_osd_line(int y) { return (15 * (y >> 3) + 2 * (y & 7)) >> 1; }
