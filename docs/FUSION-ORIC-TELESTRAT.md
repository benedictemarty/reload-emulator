# Fusion Oric / Telestrat (étape 4 de STRATEGIE-NEO6502.md)

Plan du 2026-09-30, établi après comparaison fine des deux projets (lecture seule chez
le Telestrat) et avec les contraintes de la session Telestrat. Principe : **une seule
version de chaque composant, dans le socle de reload** ; le Telestrat la reprend en
changeant d'étiquette de socle quand il le décide, et retire alors sa copie.

## Critères d'acceptation (contraintes du Telestrat)

1. Rejeu cycle à cycle identique (`telestrat_ref.h` = `telestrat.h`, `tests/test_replay.sh`).
2. Charge du RP2040 (`make charge`) : ≤ 58 % en moyenne, ≤ 65 % au pire ; mesurée avant
   et après tout composant du chemin par cycle.
3. Variante RAM 64 Ko : 376 octets de marge ; un composant commun ne doit pas être plus gros.
4. Tests des deux côtés : reload (CTest : appareils, intégration, menu) ; Telestrat
   (`make test` : 24 tests FDC, 18 de cassette, 14 de menu, bout en bout).
5. Instantanés : toute structure modifiée → version de format + 1 (prévenir).
6. Côté reload : tests PC, firmwares, `tools/socle_consommateurs.sh`, mesure sur carte
   des composants du chemin par cycle (`oric_carte.py mesure`, 3D Fongus).

## Composants et décisions

| # | Composant | Base | Travail | Risque |
|---|---|---|---|---|
| 1 | `kbd.h`, `clk.h`, `chips_common.h`, `w65c02cpu.h` | identiques | le Telestrat supprime ses copies | nul |
| 2 | `hid_app.c`, `hid_media.h`, `osd_font.h` | reload | le Telestrat supprime ses copies | faible |
| 3 | `ay38910psg.h` | reload (`CHIPS_HOT`) | mesurer la RAM des deux variantes du Telestrat | faible |
| 4 | `tusb_config.h` — **fait** | reload | CDC et `ENUMERATION_BUFSIZE` réglables (`#ifndef`) | faible |
| 5 | **WD1793** — côté reload **fait** (socle-2026-09-30-2) | **reload** (`wd1793.h` + `oric_dsk.h`) : validé Sedoric et FT-DOS (NOT_READY pendant l'occupation, INTRQ sur `$D0`, comme Oricutron), drapeau C, marque effacée en écriture, images tronquées, 128 o de RAM de moins | ajouter : `wd1793_tick_n(cycles)` et « µs avant le prochain événement » (pour les pas au repos du Telestrat) ; réécriture sur la clé **à la fin de chaque commande d'écriture** (option, par défaut : plus sûr pour l'Oric aussi) ; lecture de l'en-tête dans `insert_streamed` en option ; rappel d'écriture `const` ; délais 500/32 µs d'Oricutron en option | moyen (STRATSED) |
| 6 | **Cassette** — côté reload **fait** (socle-2026-09-30-3 : l'Oric utilise `oric_tape*.h` ; remplissage et `oric_tape_insert_mem` ajoutés ; le retour au démarrage du moteur n'a pas été nécessaire, `CLOAD` passe ; WAVE retiré) | **Telestrat** (`oric_tape.h`, `oric_tape_rec.h` CSAVE, `oric_tape_turbo.h`) : exacte au cycle, sans état global | ajouter de reload : retour au démarrage du moteur si arrêt en cours de fichier, remplissage d'un dernier fichier tronqué, entrée mémoire, lectures courtes, WAVE (tap2wave réécrit) ; l'Oric remplace `oric_td.h` ; registres du turbo non décodés quand Jasmin ou Pravetz occupent `$03FE-$03FF` | moyen |
| 7 | **Menu à l'écran** — `osd.h` **fait** (socle-2026-09-30-4 : un seul rendu en plans, celui de reload, 3 à 5 % plus rapide sur M0+ simulé et sans table en RAM ; noms du Telestrat `osd_render_line` / `osd_render_cells` ; `osd_frame` à titre facultatif ; `OSD_RAM` accepté ; grille par défaut 100 × 30, le Telestrat définit 120 × 34 ; aperçus de l'Oric identiques au bit près ; suite du Telestrat 458/458 avec ce fichier) | `osd.h` unifié ; reste le moteur commun `osd_ui.h` (styles, panneaux, bandeaux paramétrés), `osd_browser.h` (sélecteur), `osd_cfgfile.h` (fichier de configuration, profils) | chaque machine garde son `<machine>_menu.h` et sa configuration ; `rom_pool.h`, `rom_builtin.h` restent au Telestrat ; côté Telestrat : définir `OSD_COLS=120`, `OSD_ROWS=34`, retirer l'appel à `osd_init_luts` (banc) | moyen |
| 8 | Clé USB — `MSC_VOLUMES` **fait** (essai sur carte à faire) | `msc_app.c` de reload | option « un seul volume » (RAM), diagnostics SWD en option ; essai sur carte | moyen |
| 9 | VIA 6522 | reload | le Telestrat reprend d'abord le correctif IER seul ; le mode paresseux ensuite, avec `sync` dans ses fonctions de saut (sinon IRQ perdue) ; arbitrage avec la VIA du Vic20 (étape 4b) | élevé |
| 10 | Pilote de bus | `wdc65C02bus.h` (SIO puis PIO) | étape 5 ; carte (TELEMON, banques, disquettes) | élevé |

Choix encore ouverts, à trancher par les deux suites d'intégration : pause après
l'en-tête de cassette (1248 ou 1281 cycles), fin de bande (4 bits 1 ou 2 alternances).

## Ordre

1 → 4 (sans risque, surtout côté Telestrat), puis **5 (WD1793)** et **6 (cassette)**
côté reload avec les tests des deux projets, puis 7, 8, 9, 10. Chaque composant :
commit dans reload, nouvelle étiquette `socle-…`, message à la session Telestrat, qui
migre et retire sa copie quand elle le décide.
