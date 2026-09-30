# BBC Micro (Model B, Master 128)

Émulation du BBC Micro dans reload-emulator : sur PC (sokol, et `bbc_headless` sans
fenêtre pour les tests) et sur la carte Neo6502, où le vrai W65C02 exécute le MOS.
Le travail vient du projet `~/Neo6502bbc` (couche de compatibilité MOS → Neo6502), qui
garde le mode d'emploi détaillé (`docs/UTILISATION.md`) et le banc sur carte
(`tools/carte/`).

## Ce qui est émulé (`src/systems/bbc.h`)

- **Model B** : OS 1.20, BASIC 2, DFS 2.26 (WD1770 Acorn), 16 Ko de RAM latérale ;
  **Master 128** : MOS 3.20, horloge temps réel, écran fantôme (ROM en flash sur la
  carte, `bbc_master`).
- Vidéo : CRTC 6845 et Video ULA (écritures en cours de ligne rejouées à leur
  position), SAA5050 pour le MODE 7 (police Bedstead, graphismes, double hauteur…).
- Son : SN76489 par le latch IC32. Manette : convertisseur uPD7002.
- Disques : `.ssd` / `.dsd`, lecteur 0 ; WD1770 sans formatage (pas de WRITE TRACK).
- Tube : second processeur 6502 (`src/devices/tube.h`), **sur PC seulement**.
- Pas de cassette (ACIA 6850 sans réception).

ROM à fournir (non versionnées) : `src/roms/bbc_roms.h`, facultatives
`bbc_master_roms.h`, `bbc_tube_rom.h` (modèles `.example` ; MD5 et fabrication dans
`~/Neo6502bbc/docs/UTILISATION.md`).

## Sur PC

```
cd platforms/pc/build && cmake .. && make bbc bbc_headless
./systems/bbc/bbc disc=jeu.ssd                 # F12 = BREAK, SHIFT+F12 = amorçage
./systems/bbc/bbc model=master tube=1
./systems/bbc/bbc_headless -f 150 -s -t $'PRINT 6*7\n'   # écran MODE 7 en texte
```

`bbc_headless` : options en tête de `platforms/pc/systems/bbc/src/bbc_headless.c`
(capture PPM, RAM, WAV, disque, manette, Master, Tube, traces du MOS, contrôle de
compatibilité 65C02). Le texte à taper doit contenir de vrais retours à la ligne
(`$'…\n'` en bash).

## Sur la carte Neo6502 (`platforms/rp2040/systems/bbc/src/bbc.c`)

- **Bus** : machine d'état PIO (`platforms/rp2040/src/chips/wdc65C02bus.h`,
  `bus6502.pio`), `-DBBC_BUS_SIO` pour le bus piloté broche par broche ; 2 MHz,
  40 000 cycles par trame de 20 ms, accès à 1 MHz étirés de 1 ou 2 cycles.
- **Vidéo** : 800×480 à 60 Hz, RP2040 à 345 MHz sous 1,25 V (effacement horizontal
  allongé) ; `-DBBC_VIDEO_480` (295,2 MHz, 1,20 V) ; `-DBBC_VIDEO_544` (960×544,
  372 MHz, 1,30 V, les 256 lignes BBC). Le cœur 0 capture chaque ligne, le cœur 1 la
  dessine en plans de 1 bit (pas d'image complète en mémoire).
- **Disques** : clé USB, `.ssd` / `.dsd` à la racine (lecture et écriture) ; **F11** =
  menu des disques (Entrée = insérer et SHIFT+BREAK, Espace = insérer, Échap =
  fermer) ; images en flash (`src/images/bbc_images.h`) sans clé.
- **Clavier** : touches HID vers la matrice BBC ; manette USB = manette 1 ; **Pause** =
  retour au firmware Neo6502 (multiboot, `NEO_SLOT_BBC`, `NEO_SLOT_BBC_MASTER`).
- **Banc** : `bbc_diag` (ligne de diagnostic, file de touches par SWD) et
  `~/Neo6502bbc/tools/carte/carte.py` (taper, écran, mesure, recette).

## Tests (`ctest -R bbc` dans `platforms/pc/build`)

`bbc_integration` lance `tests/bbc/test_bbc_pc.py` (Python 3 + Pillow ; ignoré sans
eux) sur `bbc_headless` et `bbc_headless_lut` :

| Test | Vérifie |
|---|---|
| boot MODE 7, `PRINT 1+1` | MOS, DFS, BASIC, clavier |
| écrans de référence (MODE 7, 0, 2, 5, couleurs teletext) | rendu 6845 / ULA / SAA5050 (`tests/bbc/golden`) |
| `SOUND 1,-15,89,100` | 449,6 Hz (SN76489) |
| `ADVAL` | manette, bouton de tir |
| barres de raster | changement de palette en cours de ligne |
| `*SAVE` puis `*INFO` | écriture sur disque (WD1770, `-W`) |
| Spellbinder salle 1 | jeu (image hors dépôt : `SPELLBINDER_SSD`) |
| Master 128, Tube | si les ROM sont présentes |
| `bbc_tick4` = `bbc_tick` | chemin de la carte = chemin de référence (RAM, écran, son) |
| tables progressives = rendu PC | tables de pixels construites comme sur la carte (raster, logo de Battlezone : `BATTLEZONE_SSD`) |

Les écrans et la RAM de référence ont été comparés à l'émulateur b2
(`~/Neo6502bbc/tools/compare_b2.py`). Le cœur 65C02 (Tube, PC) est testé par
`cpu_harte_*` et `cpu_klaus_*` ([CPU-TESTS.md](CPU-TESTS.md)).

## Limites

- Tube absent sur la carte ; pas de cassette ; un seul lecteur ; pas de formatage.
- Master 128 sur la carte sans RAM latérale.
