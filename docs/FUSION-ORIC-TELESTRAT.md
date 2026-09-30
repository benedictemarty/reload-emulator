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
| 7 | **Menu à l'écran** — `osd.h` **fait** (socle-2026-09-30-4 : un seul rendu en plans, celui de reload, sans table en RAM ; coût : un peu moins cher dans un banc isolé, 6 % de plus en moyenne dans le firmware du Telestrat ; option `OSD_NOINLINE` ; noms du Telestrat `osd_render_line` / `osd_render_cells` ; `osd_frame` à titre facultatif ; `OSD_RAM` accepté ; grille par défaut 100 × 30, le Telestrat définit 120 × 34 ; aperçus de l'Oric identiques au bit près ; suite du Telestrat 458/458 avec ce fichier) | `osd.h` unifié ; reste le moteur commun `osd_ui.h` (styles, panneaux, bandeaux paramétrés), `osd_browser.h` (sélecteur), `osd_cfgfile.h` (fichier de configuration, profils) | chaque machine garde son `<machine>_menu.h` et sa configuration ; `rom_pool.h`, `rom_builtin.h` restent au Telestrat ; côté Telestrat : définir `OSD_COLS=120`, `OSD_ROWS=34`, retirer l'appel à `osd_init_luts` (banc) | moyen |
| 8 | Clé USB — `MSC_VOLUMES` **fait** (essai sur carte à faire) | `msc_app.c` de reload | option « un seul volume » (RAM), diagnostics SWD en option ; essai sur carte | moyen |
| 9 | VIA 6522 | reload | le Telestrat reprend d'abord le correctif IER seul ; le mode paresseux ensuite, avec `sync` dans ses fonctions de saut (sinon IRQ perdue) ; arbitrage avec la VIA du Vic20 (étape 4b) | élevé |
| 10 | Pilote de bus | `wdc65C02bus.h` (SIO puis PIO) | étape 5 ; carte (TELEMON, banques, disquettes) | élevé |

## Le Vic20 (Neo6502Vic20) dans la fusion

Ajouté le 2026-10-01 (lecture seule chez le Vic20, v0.3.3). Consommateur exemplaire du
socle : `RELOAD_DIR` épinglé (socle-2026-10-01), `hid_app.c`, `audio.c`
(`audio_pwm_rate_q8`), `msc_app.c`, FatFs, PicoDVI, `neo_dvi_timings.h`,
`neo_multiboot.h`, bus PIO (`wdc65C02cpu.h`, `wdc65C02bus.h`, `bus6502.pio`). Vérifié à la
compilation par `tools/socle_consommateurs.sh` depuis le 2026-10-01.

| Composant | État au Vic20 | Décision |
|---|---|---|
| `w65c02cpu.h` (PC) | copie d'avant la fusion avec la NES (sans la scrutation à l'avant-dernier cycle) | reprendre celle du socle (socle-2026-09-30-5 ou plus) |
| `mos6502cpu.h` | + option `MOS6502_NO_MACROS` (6502 du 1541 à côté du 65C02) | **reprise dans le socle** (2026-10-01) ; le Vic20 peut supprimer sa copie |
| VIA 6522 `via6522.c` | 61/61 tests VIC-20 de VICE, chemin paresseux prouvé par rejeu | **référence de l'arbitrage 4b** (VIA unique, avec reload, Telestrat, Tangerine) |
| Menu (`vic20_osd.h`) | texte dans l'image du VIC-I (police de la ROM, couleurs du VIC) | reste au Vic20 ; le sélecteur commun de l'étape 7 pourra servir |
| `mos6561vic.h`, `c1541.h`, `vic20_tape.h` | propres à la machine | restent au Vic20 |

## Migrations constatées (2026-09-30)

- **Validé sur la carte le 2026-10-01** avec le socle commun : Oric (tête a3132f4 : CSAVE /
  CLOAD par la clé, Sedoric en flux sous 8 `DIR`, aucun trait vu, son propre) et
  Telestrat v0.16.17 (socle-2026-10-01-2 : WD1793 en flux, `DIR` et `SAVE` sur la clé, image
  relue ; image en flash au retrait de la clé ; démarrage sans clé ; son
  « très bien » selon bmarty). Les relevés « 0 ligne en retard » de ces deux essais ne
  prouvent rien (compteur de PicoDVI qui redescend, voir STRATEGIE section 3 bis).
- **Telestrat v0.16.16** (socle-2026-10-01) : `ay38910psg.h` et `hid_app.c` pris aussi dans le
  socle ; seule copie restante : `mos6522via.h`. AY produit à la cadence réelle du PWM
  (arrondie à l'Hz) ; AY gardé en flash (`AY38910_HOT` vide, socle-2026-10-01-2).
- **Telestrat v0.16.15** : cassette, puces communes, `osd.h` (avec `OSD_NOINLINE`), WD1793 et
  `oric_dsk.h`, 65C02 commun ; `make test` entier, rejeu 3/3 ; charge 58 % en moyenne, 66 % au
  pire. Restent en copie : `mos6522via.h` (étape 9), `ay38910psg.h`, `hid_app.c`.
- **NES v0.5.1** : `w65c02cpu.h` commun (copie figée, provenance notée) ; son par `audio.c`,
  verrous `spin_lock_claim_unused`.

Statut sans disque (réglé dans socle-2026-09-30-6) : une commande de type II ou III sur un
lecteur vide n'est pas exécutée, INTRQ immédiat, statut $80 seul, d'après la fiche FD179X
(l'ancien WD1793 du Telestrat faisait de même ; Oricutron renvoie $10 ou attend le disque).
Le câblage réel de READY sur la Microdisc et le Telestrat n'est pas mesuré.

Choix encore ouverts, à trancher par les deux suites d'intégration : pause après
l'en-tête de cassette (1248 ou 1281 cycles), fin de bande (4 bits 1 ou 2 alternances).

## La NES (Neo6502nes) dans la fusion

Ajoutée le 2026-09-30. La NES consomme déjà le socle par `RELOAD_DIR` (SDK, PicoDVI,
TinyUSB 0.21.0, `hid_app.c`), vérifié à la compilation par `tools/socle_consommateurs.sh`.
Inventaire de ses composants (lecture seule chez elle, v0.4.1) :

| Composant | État à la NES | Décision |
|---|---|---|
| `chips_common.h`, `mos6502cpu.h` | identiques à reload | la NES supprime ses copies |
| `w65c02cpu.h` (65C02 logiciel, PC) | **divergent** : la NES scrute IRQ/NMI à l'avant-dernier cycle (mesuré sur carte : `cli_latency`, `nmi_control`) ; reload et le Telestrat ont la soustraction décimale corrigée (SingleStepTests) | **fait** (socle-2026-09-30-5) : scrutation de la NES v0.4.2 + corrections de reload ; NES `make test` et références carte, Telestrat `make test`, CTest reload : tout passe ; la NES supprime sa copie quand elle le décide |
| Pilote de bus `neo6502_bus.h` | copie de celui du Telestrat | étape 10 (pilote commun, SIO puis PIO) |
| Minutage vidéo, verrous | 960 × 544 défini localement, `next_striped_spin_lock_num` | `neo_dvi_timings.h` et verrous `spin_lock_claim_unused` du socle |
| Jeux sur la clé et menu F1 (US-15, à faire) | absents | à construire sur `msc_app.c` (étape 8) et le moteur de menu commun (étape 7 : `osd.h`, `osd_browser.h`) |
| Son de l'APU (à faire) | absent | `audio.c` du socle (PWM, volume, touches multimédia) |
| Vidéo 16 bits (RGB565) | propre à la NES | reste à la NES |

Conséquence pour l'étape 7 : le moteur de menu commun doit servir trois machines
(Oric, Telestrat, NES) ; le sélecteur de fichiers est le premier besoin de la NES.

## Tangerine (Neo6502Tangerine, Microtan 65) dans la fusion

Ajouté le 2026-09-30 (lecture seule chez Tangerine, v0.8.0). Tangerine est **autonome** :
son propre SDK (2.2.0) et PicoDVI, TinyUSB 0.21.0 (le sien ou celui de reload), licence
EUPL-1.2. Il applique les règles communes (section 3 de la stratégie) avec deux écarts
justifiés dans son ADR 0004 : `DVI_VERTICAL_REPEAT` 1 (256 lignes mises à l'échelle
vers 480) et bus en bit-bang SIO (co-simulation Phosphoneo). **Décision de bmarty (2026-09-30) :
option B avec reprise des briques** — Tangerine reste autonome (SDK 2.2.0, bus bit-bang
pour la co-simulation) et reprend les fichiers du socle quand il en a besoin (`msc_app.c`,
`audio.c`, `neo_multiboot.h`) plutôt que des copies modifiées ; sa VIA entre dans
l'arbitrage de l'étape 4b. Colonne « Si B » ci-dessous.

| Composant | État chez Tangerine | Si A (socle) | Si B (autonome) |
|---|---|---|---|
| VIA 6522 `src/via6522.c` (207 lignes, `IFR_DELAY 3`) | **4e VIA** (reload, Telestrat — même que reload —, Vic20, Tangerine) | VIA unique de l'étape 4b | à inclure dans l'arbitrage 4b (tests VICE du Vic20) |
| Clavier `pico/usb_kbd.c` + `src/hid_keys.c` | propre (touches F1, F12, Échap) | `hid_app.c` + `kbd.h` du socle | reste |
| Bus `pico/bus.h` (bit-bang SIO, 750 kHz) | propre, donnée tenue jusqu'à PHI2 | pilote commun (étape 10), co-simulation à adapter | reste (écart justifié) |
| `tusb_config.h` | clavier seul, énumération 512 | celui du socle (réglable) | reste |
| Menu F1 (jeux intégrés) | texte dans la RAM vidéo du Microtan (32 × 16, monochrome) | `osd.h` (un plan sur trois suffit) + sélecteur commun | reste |
| Clé USB, son (AY-3-8910), Pause → multiboot | absents | `msc_app.c`, `audio.c`, `neo_multiboot.h` | reprise des fichiers du socle au besoin (son ADR le prévoit) |

Tangerine n'est pas dans `tools/socle_consommateurs.sh` (il ne compile rien du socle).

## Ordre

1 → 4 (sans risque, surtout côté Telestrat), puis **5 (WD1793)** et **6 (cassette)**
côté reload avec les tests des deux projets, puis 7, 8, 9, 10 ; pour la NES : fusion de
`w65c02cpu.h`, puis clé et menu sur le socle. Chaque composant :
commit dans reload, nouvelle étiquette `socle-…`, message à la session Telestrat, qui
migre et retire sa copie quand elle le décide.
