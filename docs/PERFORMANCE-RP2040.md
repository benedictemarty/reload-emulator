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

## Mesure avant / après (Oric)

`oric_diag_cold` est `oric_diag` compilé avec `ORIC_COLD` : le même code, laissé en
flash. `oric_diag` et `oric_diag_cold` comptent le temps des 19 968 cycles d'une
trame (`diag_tick_us`, `diag_tick_frames`, `diag_tick_max`) et de la trame entière
(`diag_exec_us`, `diag_exec_max`).

```
tools/oric_carte.py mesure 10                                   # oric_diag flashé
NEO_ELF=…/oric_diag_cold.elf tools/oric_carte.py mesure 10      # oric_diag_cold flashé
```

Le budget d'une trame est de 19 968 µs (1 MHz, 50 Hz) ; l'émulation seule doit en
laisser assez pour l'USB, le panneau et la mise à jour de l'écran.

Résultats sur la carte : **à mesurer** (carte partagée, prise de la sonde à
décider).
