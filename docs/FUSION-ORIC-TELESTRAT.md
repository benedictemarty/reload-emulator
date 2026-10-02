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
| 7 | **Menu à l'écran** — `osd.h` **fait** (socle-2026-09-30-4 : un seul rendu en plans, celui de reload, sans table en RAM ; coût : un peu moins cher dans un banc isolé, 6 % de plus en moyenne dans le firmware du Telestrat ; option `OSD_NOINLINE` ; noms du Telestrat `osd_render_line` / `osd_render_cells` ; `osd_frame` à titre facultatif ; `OSD_RAM` accepté ; grille par défaut 100 × 30, le Telestrat définit 120 × 34 ; aperçus de l'Oric identiques au bit près ; suite du Telestrat 458/458 avec ce fichier) | `osd.h` unifié ; **style des panneaux commun : `osd_panel.h`** (2026-10-02, socle-2026-10-01-23 : bandeau, panneaux titrés, barre ▶, boutons, message, pied à pastilles, sélecteur avec ombre et défilement, tailles ; l'Oric s'en sert, rendu identique octet pour octet ; demandé par Tangerine) ; reste le moteur commun (navigation), `osd_browser.h` (liste de fichiers), `osd_cfgfile.h` (fichier de configuration, profils) | chaque machine garde son `<machine>_menu.h` et sa configuration ; `rom_pool.h`, `rom_builtin.h` restent au Telestrat ; côté Telestrat : définir `OSD_COLS=120`, `OSD_ROWS=34`, retirer l'appel à `osd_init_luts` (banc) | moyen |
| 8 | Clé USB — `MSC_VOLUMES` **fait** (essai sur carte à faire) | `msc_app.c` de reload | option « un seul volume » (RAM), diagnostics SWD en option ; essai sur carte | moyen |
| 9 | VIA 6522 — **adoptée par le Vic20 et Tangerine** (2026-10-01, socle-2026-10-01-20) ; **Oric : via6522 par défaut** (2026-10-02, décision de bmarty ; `ORIC_MOS6522VIA` pour l'ancienne), Telestrat en adoption (décision de bmarty) | candidate : `src/chips/via6522.c/.h` du Vic20 (61/61 tests VICE, mode paresseux prouvé, EUPL-1.2 ou zlib), copiée dans le socle avec deux ajouts à remonter au Vic20 : accesseurs `via_get_ca2/cb2/pb7` en ligne, placement `VIA6522_HOT`, avance par lots `via_quiet_cycles` / `via_advance`, lecture sans effet `via_peek` | Oric : couche `_oric_via_*` dans `oric.h`, option `ORIC_VIA6522` (défaut inchangé : tests identiques) ; avec via6522 : intégration Oric 22/22, écran de 3D Fongus identique, −0,8 % d'instructions, puis −3,8 % avec `via_advance` dans le tick de 4 cycles (callgrind, 400 trames de 3D Fongus : 1 505 contre 1 564 millions) ; clavier lu par le rappel du port B ; ACK imprimante en impulsion basse. Firmware `oric_diag_via6522` compilé (VIA en RAM, 5 424 o de marge). **Carte (2026-10-01)** : boucle BASIC `10 A=A+1:GOTO 10`, 10 s, émulation 12 959 µs par trame (64,9 % du budget) contre 14 328 µs (71,8 %) avec mos6522via, soit −9,6 % ; F12 différée vérifiée. **Reste** : rejeu du Telestrat | élevé |
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
- **Telestrat v0.16.21** (socle-2026-10-01-8) : `msc_app.c` du socle (sa copie `usb_msc.c`
  retirée) et `neo_storage` pour tous ses fichiers (plus aucun appel FatFs direct) ; 481 tests,
  rejeu 3/3 ; RAM 64 Ko : 724 o au-delà du tas ; charge 59 / 67 %. Essai sur carte à venir.
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

## VIA unique : critères d'acceptation (étape 9, 2026-10-01)

Candidate : `src/chips/via6522.c/.h` (VIA du Vic20, 61/61 tests VICE). Exigences reçues :

**Telestrat** (rejeu cycle à cycle, charge 59 / 67 %) :
1. Résultat **identique à des pas de 4 cycles**, IRQ comprise (test_replay A, B, C) ; une API
   publique « combien de pas de 4 cycles sans événement » et « avancer de n pas sans événement »
   (il lit aujourd'hui t1/t2.counter, t_out, intr.pip, ifr/ier, acr, CB2 de mos6522via).
   **Fait (étape 2)** : `via_quiet_cycles(via)` = cycles à venir sans événement,
   `via_advance(via, n)` = n cycles ; égal à n `via_tick` (tests/devices/test_via6522.c :
   200 000 pas aléatoires de 1 à 40 cycles, registres, IRQ, PB7, CA2, CB2 comparés, 0 écart).
2. Correctif IER (désactiver une source active relâche l'IRQ) : STRATSED en dépend.
3. Même retard de pipeline de l'IRQ que mos6522via, ou le rejeu change (démarrage à froid en
   RAM 64 Ko : bus flottant, sensible au cycle près).
4. Lignes : VIA 1 comme l'Oric (AY par PA/CA2/CB2, clavier PB0-3, imprimante PB4/CA1, PB7 pour
   CSAVE, CB1 cassette) ; VIA 2 : PA0-2 = banque avec DDR (une ligne en entrée garde la banque
   précédente), PA4 série, PB manettes, CB1 sonnerie redonnée à chaque pas.
5. Instantanés : version de format + 1 si la structure change (accepté).

**Tangerine** (vérifié contre MAME au cycle près, 11 jeux identiques en RAM) :
1. Calage de MAME 6522via (IFR_DELAY 3) : IRQ de T1/T2 N+3 cycles après l'écriture ; roue
   libre toutes les N+3 (N+2 entre bascules de PB7) ; lecture du compteur = reste − 3 ; écrire
   l'ACR en roue libre relance T1 depuis son compteur.
2. `via_peek` : lecture sans effet de bord (vidages, bus tenu en reset) — **ajoutée**
   (étape 2) : même valeur que `via_read`, structure inchangée (vérifié par le test).
3. PB7 piloté par T1 et lu par le port B (sortie cassette).
4. CB2 en entrée, front choisi par PCR, mode « indépendant » (présent dans la candidate).
5. Latches de mise sous tension comme MAME, puis reset.

**Question ouverte** : calage temporel. La candidate suit VICE (période N+2 en roue libre),
Tangerine suit MAME (IRQ à N+3, lecture − 3). À départager par les tests VICE du Vic20 et le
test golden MAME de Tangerine, sur la même VIA ; on ne choisit pas sans mesure.

**Mesure (2026-10-01) : golden MAME de Tangerine avec la VIA du socle.** Un clone de
Neo6502Tangerine (sans toucher à son dépôt), dans lequel `src/via6522.[ch]` devient un adaptateur
vers la VIA du socle. Un banc fait aussi tourner les deux VIA côte à côte et journalise chaque
lecture, chaque niveau d'IRQ et chaque PB7 qui diffèrent.

| Essai | Golden (15 cas) |
|---|---|
| VIA de Tangerine (référence) | 15 réussis ; écarts connus : berzerk 83, defender 15, hangman 2 octets |
| VIA du socle telle quelle | 13 ; **échecs** : mooncresta (31 octets), son de berzerk ; berzerk 145, defender 150 |
| + latches de MAME seuls | identique (13) |
| + compteurs en marche dès la mise sous tension + latches MAME | 14 ; mooncresta réussi, defender 15, berzerk 52 ; son de berzerk en échec |
| + monostable sans rechargement (comme MAME) | **15** ; berzerk 83, defender 15, hangman 2 (identique à la référence) |

Différences trouvées entre les deux VIA. **Aucune ne relève du calage N+2 / N+3** :
- **A. Compteurs avant toute écriture.** MAME les fait décompter dès la mise sous tension. Le
  socle les laisse figés à FFFF jusqu'à l'écriture de T1CH / T2CH, et `via_reset` les arrête.
  mooncresta et defender lisent T1CL / T2CL sans les avoir écrits (hasard, a priori).
- **B. Mise sous tension et reset.** MAME met les latches à B5F3 et son reset les garde ; le
  socle les remet à FFFF. B5F3 vient d'un relevé sur un VIC-20 : MAME 6522via.cpp, device_start, « via at 0x9110 in vic20 show these values / ports are not written by kernel! » (T2 : « taken from vice »). Méthode non précisée, et ce n'est pas une mesure d'un 6522 isolé à l'allumage.
- **C. Lecture du compteur.** Le socle lit 1 cycle de plus que MAME, et après l'écriture de
  T1CH il tient FFFF deux cycles. Visible dans la trace, **sans effet sur le golden**.
- **D. Monostable après le délai.** Le socle recharge depuis le latch (période latch + 2) ;
  `via6522.c` dit que c'est mesuré sur un vrai VIC-20 (viavarious via1). MAME boucle par FFFF
  sans recharger. Le son de berzerk dépend de D.

Conséquences :
- Le golden est un **autre émulateur**. Pour D, la référence matérielle du Vic20 contredit MAME :
  on suit le matériel (proposition du Vic20), et l'écart de berzerk devient un écart de MAME, à
  documenter chez Tangerine.
- A et B ne sont pas couverts par les tests du Vic20, à ma connaissance. À vérifier avec lui.
  Ce qu'on a : `vic20-testprogs/via_defaults/readme.txt` cite la datasheet Rockwell : « A low
  reset input clears all internal registers to logic 0 (except T1 and T2 latches and counters
  and the Shift Register) [...] disables the timers ». Le reset **garde** donc latches et
  compteurs, ce qui va dans le sens de MAME pour B et contre `via_reset` du socle. Les valeurs à la
  mise sous tension sont notées « ? », non mesurées. « disables the timers » ne dit pas si
  les compteurs continuent de décompter (A) : je ne sais pas.
- **Décision (2026-10-01, validée par le Vic20)** :
  - B : `via_reset` garde latches, compteurs et SR (fiche Rockwell) ; valeurs d'allumage posées
    par `via_init` (FFFF / FF).
  - A : option par machine `via_set_free_counters` (compteurs en marche dès maintenant et après
    chaque reset, sans IRQ), désactivée par défaut. Le Vic20 ne l'active pas (pas de mesure).
  - D : on garde le matériel (rechargement depuis le latch).
  - Golden de Tangerine avec l'option et les latches B5F3 posés par l'hôte : 14/15. Seul le
    son de berzerk échoue (D ; écart de MAME, à documenter chez Tangerine si bmarty valide).
  - Vic20 : tous ses tests passent, comportement identique à l'octet, charge 83 % contre 82 %
    (arrondi).
  - **Adoptions** : le Vic20 en v0.3.20 (fichiers identiques au socle, `cmp`, sans
    `free_counters`). Tangerine, sur décision de bmarty : via6522 du socle extraite telle
    quelle, sa VIA retirée ; `via_set_free_counters(true)`, `t1_latch = 0xB5F3` après
    `via_init`, `via_set_cb2_input(true)` après `via_reset` ; golden 14/14, le son de berzerk
    devenant un écart connu (première écriture AY différente : n° 37 sur 86).
- IER (exigence 2 du Telestrat) : son cas STRATSED est repris dans `test_via6522.c`
  (`test_ier_release`) et via6522 le passe déjà. Le défaut venait de l'ancienne mos6522via.
- Le banc se trouve dans le dossier temporaire de la session (`tgr/src/via6522.c`, variables
  `SV_DRIVE`, `SV_FREE`, `SV_LATCH`, `VIA_DIFF`, `VIA_T1`) ; il n'est pas versionné.

**Écarts berzerk / defender du golden (explication de Tangerine, 2026-10-02).** Ils ne
viennent d'aucune des deux VIA mais d'un arrondi de MAME. À 750 kHz, la période vaut
1 333 333 333 333,33 as ; MAME la tronque, et après 750 000 cycles son 6502 est à
999 999 999 999 750 000 as, juste avant la seconde 1. Au-delà, `attotime_to_clocks` compte un
cycle de moins pour les timers libres de la VIA, qui se lisent alors une unité trop haut (defender,
première lecture de T2 après 1 s : $9D chez MAME, $9C en décompte régulier). Comparés à
0,9 s, les deux jeux sont identiques à MAME octet pour octet avec la VIA du socle : golden de
Tangerine 16/16 (v0.21.0). Seul écart restant : le son de berzerk (point D, monostable).

## VIA unique : rejeu du Telestrat (2026-10-02)

Portage dans une copie de Neo6502TeleStrat (3ce3d59), sans écrire dans son dépôt :
- option `TELESTRAT_VIA6522`, couche `_tv_*` dans `telestrat.h` et `telestrat_ref.h` ;
- clavier et manettes lus par les rappels du port B ;
- ACK de l'imprimante inversé (CA1 au repos haut), CB1 et CA1 donnés seulement quand ils
  changent (mode paresseux gardé) ;
- saut des cycles calmes par `via_quiet_cycles` / `via_advance`.

Diff proposé au Telestrat (723 lignes, 8 fichiers) : dossier temporaire de la session,
`telestrat_via6522.diff`.

| Vérification | Résultat |
|---|---|
| Rejeu, tout sur mos6522via (réorganisation seule) | 3/3 identiques |
| Rejeu, système optimisé sur via6522, référence sur mos6522via | 0/3 : écarts attendus (voir ci-dessous) |
| Rejeu, tout sur via6522 | **3/3 identiques** (bus, IRQ, audio, série, image ; démarrage à froid) |
| Écran final du scénario A (STRATSED, HYPER-BASIC, DIR), mos6522via contre via6522 | identique |
| test_telestrat | 510/510 avec les deux VIA |
| callgrind, 400 trames du scénario A | 1 109 contre 1 130 millions d'instructions (−1,8 %) |
| Firmware standard, via6522 en RAM | marge 4 276 → 836 o (−3 440 o) |
| Firmware RAM 64 Ko, via6522 en RAM | **dépasse la RAM** |
| Firmware RAM 64 Ko, via6522 en flash | marge 476 → 1 020 o ; charge à mesurer sur la carte |

**Défaut de mos6522via trouvé en route.** Avec `mos6522via_tick(v, 4)`, le pas des systèmes
Oric et Telestrat, T1 en roue libre a une période de N+8 au lieu de N+2 (N+2 en pas de 1,
comme via6522 et le matériel). Mesuré sur la copie du Telestrat et sur celle du socle :
N=10 → 16 au lieu de 12, N=100 → 108 au lieu de 102, N=10 000 → 10 008 au lieu de 10 002.
Effets : sons produits par T1 trop graves (6 % pour N=100), IRQ BASIC de l'Oric toutes les
10 008 cycles. Le rejeu du Telestrat ne pouvait pas le voir : sa référence a le même défaut.
Premier écart de la trace A : T1CL lue $6C par la référence, $B9 par via6522. $6C se retrouve
exactement avec une période de N+8 ; $B9 avec N+2.

## Oric sur la VIA unique par défaut (2026-10-02, décision de bmarty)

- `oric.h` : via6522 par défaut ; `ORIC_MOS6522VIA` garde l'ancienne VIA (comparaisons).
- Instantanés : version 5 (structure de la VIA changée ; les anciens sont refusés).
- Instantanés : à la reprise, les rappels de la VIA (clavier sur le port B) et leurs données
  restent ceux de la machine en cours. Les adresses sauvées viennent d'un autre lancement (ASLR sur
  PC, autre firmware) ; signalé par le Telestrat, qui avait le défaut. Chez l'Oric il est
  latent : `oric_save_snapshot` / `oric_load_snapshot` n'ont aucun appelant aujourd'hui.
- PC : `oric`, `oric_headless`, `oric_devices_test` compilent `via6522.c` ;
  `oric_headless_mos` + CTest `oric_integration_mos` pour l'ancienne VIA.
- RP2040 : `oric` et les `oric_diag*` avec la VIA en RAM (`VIA6522_HOT`) ; `oric_diag_mos`
  remplace `oric_diag_via6522` pour comparer. Marge RAM : oric 7 188 o, oric_diag 4 960 o.
- Vérifié : CTest 16/16 (intégration 22/22 avec les deux VIA), firmwares compilés, cœur 1 :
  0 fonction en flash. Sur la carte (2026-10-02, tête f718721, boucle BASIC vérifiée à
  l'écran) : oric_diag 12 959 µs par trame (64,9 %) contre oric_diag_mos 14 328 µs (71,8 %),
  soit −9,6 %, comme la veille ; le panneau F1 (osd_panel.h) s'ouvre normalement.

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
