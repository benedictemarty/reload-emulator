# Performances sur le RP2040 (Neo6502)

## Code exécuté à chaque cycle : en RAM

Le RP2040 exécute la flash à travers un cache XIP de 16 Ko. Le code appelé à chaque
cycle émulé (pilote du bus du 65C02, décodage mémoire, VIA, PSG, lecteurs, cassette)
y provoque des défauts de cache : il est donc placé en RAM (`.time_critical.<système>`,
copié au démarrage par le SDK).

Chaque en-tête déclare une macro de placement, vide par défaut (version PC) ; le
fichier RP2040 de chaque système la définit avant les `#include` :

| Macro | En-têtes | Définie par |
|---|---|---|
| `WDC65C02_HOT` | `platforms/rp2040/src/chips/wdc65C02cpu.h` | `bbc.c`, `oric.c`, `apple2.c`, `apple2e.c` |
| `CHIPS_HOT` | `mos6522via.h`, `ay38910psg.h`, `beeper.h`, `wd1793.h`, `wd1770.h`, `oric_td.h`, `disk2_fdc.h`, `disk2_fdd.h` | idem |
| `BBC_HOT` | `src/systems/bbc.h` | `bbc.c` |
| `ORIC_HOT` | `src/systems/oric.h` | `oric.c` |
| `APPLE2_HOT`, `APPLE2E_HOT` | `src/systems/apple2.h`, `apple2e.h` | `apple2.c`, `apple2e.c` |

La boucle d'une trame (`run_ticks`, fonction `__no_inline_not_in_flash_func`) est
elle aussi en RAM, avec la fonction `*_tick` du système intégrée.

RAM consommée (compilation Release, 2026-09-30, avec les ROM réelles) :

| Firmware | Code déplacé en RAM | Tas |
|---|---|---|
| `oric` | 9,8 Ko | 22 Ko réservés (`PICO_HEAP_SIZE`) |
| `apple2` | 3,6 Ko | 22 Ko réservés |
| `apple2e` | 4,0 Ko | 22 Ko réservés, 0,5 Ko de marge au-delà |

**Tas et PicoDVI** : `dvi_init` alloue par `malloc` 3 tampons TMDS de
`3 × largeur / 2 × 4` octets (14,4 Ko en 800 pixels de large) ; newlib arrondit, et
il faut davantage de tas (≈ 21,8 Ko mesurés sur le Telestrat en 960 de large, voir
`~/Neo6502TeleStrat/docs/SONDE_SWD.md`). Comme le BBC, l'Oric et les Apple réservent
`PICO_HEAP_SIZE=0x5800` à l'édition des liens : un manque de RAM fait échouer la
compilation (« region RAM overflowed ») au lieu de planter la carte au démarrage.

Sur le IIe (ROM de 16 Ko en RAM, lue à chaque cycle) :
- les fonctions de changement de banque (`_apple2e_*_bank_update`,
  `_apple2e_lc_control`, `apple2_lc_control`, ≈ 9,6 Ko), appelées seulement sur les
  commutateurs mémoire, restent en flash ;
- la ROM de caractères (4 Ko), lue seulement par la mise à jour de l'écran, est en
  flash (`__in_flash`, voir `src/roms/apple2ee_roms.h.example`).

(Apple : non essayé sur la carte.)

## Bus du W65C02 : appels, SIO intégré ou PIO

`platforms/rp2040/src/chips/wdc65C02bus.h` (repris du BBC, binaires BBC identiques
après l'extraction) intègre le cycle de bus dans la fonction `*_tick` du système :

- par défaut, accès directs aux registres SIO (même séquence et mêmes délais que
  `wdc65C02cpu.h`, sans les appels) ;
- `WDC65C02_BUS_PIO` : cycles séquencés par une machine d'état PIO
  (`platforms/rp2040/src/chips/bus6502.pio`, PIO1 SM0 ; PicoDVI utilise PIO0) ; les
  délais des 74HC245 se recouvrent avec le travail de l'émulation. `bus_pio_start()`
  après `wdc65C02cpu_init()`.

| Système | Bus |
|---|---|
| BBC | PIO par défaut, SIO intégré avec `-DBBC_BUS_SIO` (validé sur la carte) |
| Oric | **PIO par défaut** (`oric`, `oric_diag`) depuis le 2026-09-30 : le plus rapide, validé sur la carte (clavier, Sedoric et FT-DOS depuis la clé, cassette) ; `oric_diag_calls` (appels), `oric_diag_inline` (SIO intégré) |
| Apple II / IIe | appels de `wdc65C02cpu.h` |

## Mesure avant / après (Oric)

Firmwares de banc (même code, définitions en plus) :

| Firmware | Code par cycle | Bus |
|---|---|---|
| `oric_diag_cold` | en flash (`ORIC_COLD`, avant le sprint 1) | appels |
| `oric_diag_calls` | en RAM | appels |
| `oric_diag_inline` | en RAM | SIO intégré |
| `oric_diag` | en RAM | PIO (comme `oric`) |

(Avant le 2026-09-30 au soir, `oric_diag` avait le bus par appels et la variante PIO
s'appelait `oric_diag_pio` : c'est sous ces noms que les tableaux ci-dessous ont été
mesurés.)
 `oric_diag` et `oric_diag_cold` comptent le temps des 19 968 cycles d'une
trame (`diag_tick_us`, `diag_tick_frames`, `diag_tick_max`) et de la trame entière
(`diag_exec_us`, `diag_exec_max`).

```
NEO_ELF=…/systems/oric/oric_diag_XXX.elf tools/oric_carte.py mesure 10   # firmware flashé
```

Le budget d'une trame est de 19 968 µs (1 MHz, 50 Hz) ; l'émulation seule doit en
laisser assez pour l'USB, le panneau et la mise à jour de l'écran.

### Résultats sur la carte (2026-09-30, BASIC `10 A=A+1:GOTO 10`, 800×480 à 295,2 MHz)

| Firmware | Émulation par trame | Budget (19 968 µs) | Au démarrage, 40 000 cycles : bus seul / `oric_tick` complet |
|---|---|---|---|
| `oric_diag_cold` (flash, appels) | 21 784 µs | 109,1 % | 9 324 / 44 557 µs |
| `oric_diag` (RAM, appels) | 22 507 µs | 112,7 % | 9 578 / 45 212 µs |
| `oric_diag_inline` (RAM, SIO intégré) | 22 144 µs | 110,9 % | — |
| `oric_diag_pio` (RAM, PIO) | 21 289 µs | **106,6 %** | 9 911 / 43 617 µs |

- **Aucune variante ne tient 1 MHz** : l'Oric tourne à ≈ 93 % de sa vitesse sur la
  carte (déjà le cas avant ces travaux : variante `cold`).
- **Le bus n'est qu'un quart du coût** (≈ 0,24 µs par cycle) : les trois quarts
  sont l'émulation des périphériques dans `oric_tick` (≈ 0,85 µs par cycle).
- Code en RAM : pas de gain, même un peu plus lent que la flash (le code tenait
  dans le cache XIP ; en RAM il partage le bus mémoire avec le cœur 1 et le DMA de
  la vidéo, qui ont la priorité : hypothèse non vérifiée).
- Bus PIO : le meilleur des quatre (−2,3 % par rapport au code en flash).

Échantillonnage du compteur ordinal du cœur 0 par la sonde (800 échantillons,
`oric_diag_pio` compilé avec `-g`, même code) : `oric_tick` et ses fonctions
intégrées ≈ 77 %, `mos6522via_tick` ≈ 14 % ; dans `oric_tick`, le bus (attente du
FIFO, clôture du cycle, donnée) ≈ 18 %, le reste réparti ligne par ligne (tests par
cycle : PSG, horloge d'échantillons, FDC, Jasmin, ligne IRQ réécrite à chaque cycle
par `gpio_put`, VIA). Pistes : regrouper le travail par 4 cycles comme
`bbc_tick4`, VIA « paresseuse » (`mos6522via_tick_inline`, déjà utilisée par le
BBC), broche IRQ écrite seulement quand elle change.

### `oric_tick` restructuré (2026-09-30, même programme, même carte)

| Firmware | Avant | Après | 40 000 cycles : bus seul / `oric_tick` |
|---|---|---|---|
| `oric_diag_pio` | 106,6 % | **82,9 %** (16 546 µs) | 9 896 / 33 942 µs |
| `oric_diag` | 112,7 % | **86,3 %** (17 230 µs) | 9 583 / 34 318 µs |

Les périphériques passent de ≈ 0,85 à ≈ 0,60 µs par cycle ; l'Oric tient 1 MHz avec
≈ 17 % de marge. Changements (comportement émulé inchangé, tests PC identiques) :
- `mos6522via_set_pb` et `mos6522via_set_cb1` appelés seulement quand l'entrée change
  (ou qu'un front est en attente) : chaque appel remettait la VIA hors de son mode
  paresseux, et le clavier l'appelait tous les 4 cycles ;
- travail « tous les 4 cycles » regroupé (PSG, Disk II, horloge des échantillons par
  pas de 4, VIA) ;
- broche IRQ écrite seulement quand elle change.

### Sous charge de jeu, et horloge à 345 MHz (2026-09-30)

La boucle BASIC ne suffit pas : l'écran-titre de *3D Fongus* (Microdisc, VIA et son
actifs) prend **105,1 %** du budget à 295,2 MHz (bus PIO). Intégrer le corps de
`oric_tick` dans la boucle de trame (`_oric_tick`) n'y change presque rien (105,1 %).
Le timing 800×480 à **345 MHz** du BBC (même image, effacement horizontal allongé,
accepté par l'écran de bmarty pour le BBC ; `platforms/rp2040/src/neo_dvi_timings.h`)
donne 17 % de temps processeur en plus :

| Charge | 295,2 MHz | 345 MHz |
|---|---|---|
| BASIC `10 A=A+1:GOTO 10` | 82,5 % | **70,2 %** |
| *3D Fongus*, écran-titre | 105,1 % | **89,6 %** |

`oric` passe donc à 345 MHz (1,25 V) par défaut ; `-DORIC_VIDEO_295` garde l'ancien
mode (`oric_diag_295`). **Image de l'Oric à 345 MHz non encore regardée** ;
échauffement à 1,25 V non mesuré (comme pour le BBC).

Méthode d'échantillonnage (sans laisser un cœur arrêté) : OpenOCD avec
`-c "set USE_CORE 0"` avant `target/rp2040.cfg`, boucle Tcl `halt` / `get_reg pc` /
`resume` ; sans `USE_CORE 0`, les deux cœurs sont arrêtés ensemble et la reprise
échoue (« core1 not halted ») : `reset run` a été nécessaire.
