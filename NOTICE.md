# Licences et origines

Ce dépôt n'a **pas de fichier `LICENSE` unique** : le dépôt amont (vsladkov/reload-emulator) n'en a pas non plus. Ce document
dit ce que les en-têtes des fichiers déclarent ; il ne choisit pas de licence à la place de l'auteur. Un `LICENSE` global reste
à décider par bmarty (plusieurs en-têtes écrits par bmarty renvoient à « LICENSE » ou à « devices/neo_storage.h »).

| Partie | Déclaré dans les fichiers |
|---|---|
| `src/chips/*` repris de floooh/chips (6502, AY, mem, kbd, beeper…), `src/systems/apple2*.h`, `oric.h` | zlib/libpng, © 2018 Andre Weissflog, © 2023 et 2025 Veselin Sladkov, © 2026 bmarty |
| fichiers écrits pour ce projet (`neo_*`, `osd*`, `bbc*`, `oric_*`, `wd17*`, tests…) | zlib/libpng, © 2026 bmarty |
| `src/chips/via6522.[ch]` | SPDX `EUPL-1.2 OR Zlib`, bmarty (repris de Phosphoric) |
| `platforms/pc/src/{gfx,clock,prof,sokol}.*` | **aucun en-tête** : repris du modèle chips-test (floooh, zlib) — origine et licence à confirmer |
| `src/chips/mos6522via.h` | **aucun en-tête** (dérivé de floooh/chips, zlib) — à confirmer |
| `platforms/rp2040/src/neo_interp_ram.c` | déclaré zlib © bmarty, mais reprend le code du SDK Pico (`hardware_interp`, BSD-3-Clause © Raspberry Pi) : l'avis BSD du SDK devrait y figurer |
| `platforms/rp2040/src/tusb_config.h` | issu des exemples TinyUSB (MIT © Ha Thach) : en-tête absent |
| `platforms/pc/lib/sokol` | zlib (sous-module) |
| `platforms/pc/lib/imgui`, `ImGuiFileDialog` | MIT (sous-modules) |
| `platforms/rp2040/lib/fatfs` | licence FatFs (ChaN), voir `lib/fatfs/LICENSE.txt` |
| `platforms/rp2040/lib/PicoDVI` | BSD 3-clauses (sous-module, forké) |
| `platforms/rp2040/lib/tinyusb`, `pico-sdk` | MIT, BSD 3-clauses (sous-modules ; l'état des patchs est dans `platforms/rp2040/patches/`) |
| ROM et images (`src/roms`, `src/images`) | **non distribuées** : produire les en-têtes à partir de ses propres exemplaires |

Aucune incompatibilité entre zlib, MIT, BSD-3 et EUPL n'a été relevée, mais je n'ai pas fait d'analyse juridique.
