# Stratégie commune des émulateurs sur Neo6502

Proposition du 2026-09-30 (session reload-emulator, à la demande de bmarty). Les
décisions encore ouvertes sont marquées **À décider (bmarty)**.

## 1. Constat

Cinq projets ont du code pour la carte (RP2040 + vrai W65C02, DVI par PicoDVI, USB
hôte TinyUSB, clé FatFs, son PWM) :

| Projet | Base RP2040 | Remarque |
|---|---|---|
| reload-emulator (Oric, BBC, Apple II / IIe) | la sienne | sert déjà de base aux deux suivants |
| Neo6502TeleStrat | reload via `RELOAD_DIR` (SDK, PicoDVI, `audio.c`) + ses copies (`hid_app.c`, `usb_msc.c`, pilote de bus, OSD, WD1793) | |
| Neo6502nes | reload via `RELOAD_DIR` (SDK, PicoDVI, TinyUSB, `hid_app.c` seul) + son pilote de bus (copie de celui du Telestrat), sa copie divergente de `w65c02cpu.h` ; pas de son ni de clé USB | voir FUSION-ORIC-TELESTRAT.md, section NES |
| Neo6502Tangerine | autonome (son PicoDVI, TinyUSB du SDK) | |
| Neo6502Trinity | firmware C++ à part | références : `README-TRINITY.md`, `Neo6502monlivre/manuscrit/15-firmware.md` |

Vic20 et Atari2600 n'ont pas encore de port pour la carte.

Les mêmes pannes ont été rencontrées et résolues plusieurs fois, chaque projet de son
côté : traits rouges, tas de PicoDVI, clé USB, donnée du bus qui fuit, verrous
partagés… Les correctifs sont **copiés**, pas partagés : un correctif fait dans un
projet n'arrive pas dans les autres. Trois défauts déjà corrigés ailleurs restaient
dans du code en service (tableau MSC indexé par l'adresse USB dans reload, verrous
« striped » de PicoDVI au Telestrat, au NES et aux Apple, impulsion OE3 brève à
Tangerine).

## 2. Principe : un socle, des systèmes

**Un seul socle Neo6502**, dans `reload-emulator/platforms/rp2040/src` (déjà la base
du Telestrat et du NES), qui porte **tout ce qui dépend de la carte**. Un émulateur
n'y ajoute que son système : puces, carte mémoire, rendu d'une ligne, touches.

| Brique du socle | Fichier (actuel ou prévu) | Contenu |
|---|---|---|
| Bus du W65C02 | `chips/wdc65C02bus.h`, `chips/bus6502.pio` | cycle en PIO (défaut) ou SIO intégré ; donnée tenue jusqu'à la descente de PHI2 ; 6 `nop` par lecture d'adresse |
| Vidéo | *prévu* `neo_video.c` | `dvi_init` avec verrous dédiés ; tableau des modes (800×480 à 295,2 / 345 MHz, 960×544 à 372 MHz) ; rendu par ligne en 3 plans de 1 bit ; tas réservé |
| Son | `audio.c` | PWM + DMA, `audio_push_sample` en RAM, volume commun (`audio_set_volume`) |
| USB | `hid_app.c`, `msc_app.c`, `tusb_config.h` | TinyUSB 0.21.0 ; clavier (ASCII ou brut), manettes, touches multimédia (`devices/hid_media.h`) ; clé montée depuis la boucle principale, délai de 1 s par transfert, tables indexées par emplacement |
| Stockage | `lib/fatfs` | FatFs, réglages communs (voir 3) |
| Menu à l'écran | `src/osd/osd.h` + un moteur de menu | surface de cellules 8×8, police unscii-8, bandeaux (cassette, volume) ; chaque système décrit ses entrées |
| Multiboot | `neo_multiboot.h` | Pause → firmware Neo6502 ; `NEO_SLOT_<SYSTÈME>` |
| Boucle principale | *prévu* `neo_platform.c` | horloge et tension, cœur 1 (DVI, son), échéances absolues avec rattrapage, service USB, boucle de trame en RAM |
| Banc SWD | *prévu* `tools/carte/` commun | boîtes aux lettres `diag_*` (touches, fichiers, mesures), un seul outil paramétré par système |

Un système fournit : `*_init`, `*_tick` (ou un lot de cycles), `render_line(y,
planes)`, `on_key`, et son menu. Un projet externe (Telestrat, NES…) compile contre
le socle par `RELOAD_DIR` **sans copier ses fichiers** ; s'il doit s'en écarter, il
le dit dans un ADR.

## 3. Règles communes (acquises, valables pour tous)

1. **Vidéo** : 8 couleurs (Oric, Telestrat, BBC) : encoder en 3 plans de 1 bit
   (`tmds_encode_1bpp`), pas l'encodeur à palette pleine résolution (traits rouges
   sur l'écran de bmarty). Plus de 8 couleurs (Apple II : 16 couleurs NTSC) : les
   plans ne suffisent pas ; l'encodage 16 bits (RGB565) du NES n'a pas de défaut
   signalé ; l'encodeur à palette des Apple est à juger sur la carte.
2. **Vidéo** : `dvi_init(&dvi0, spin_lock_claim_unused(true), spin_lock_claim_unused(true))`,
   jamais `next_striped_spin_lock_num()` (verrous partagés avec FatFs et TinyUSB).
3. **Vidéo** : PicoDVI répète chaque tampon (`DVI_VERTICAL_REPEAT` = 2) : un tampon
   par ligne source ; 3 tampons TMDS.
4. **Mémoire** : `PICO_HEAP_SIZE=0x5800` pour tout firmware qui appelle `dvi_init`
   (un manque de RAM fait échouer l'édition des liens, pas la carte).
5. **Mémoire** : le code appelé à chaque cycle émulé est en RAM (`*_HOT`,
   `__not_in_flash_func`), la boucle de trame aussi.
6. **Bus** : donnée d'une lecture tenue jusqu'à la descente de PHI2 (le 65C02 est
   statique, les pauses durent des millisecondes) ; PIO par défaut.
7. **USB** : TinyUSB 0.21.0 ; `f_mount` hors des rappels TinyUSB ; délai maximal
   par transfert ; `CFG_TUH_ENUMERATION_BUFSIZE` 512 ; tables par emplacement
   (adresse USB − 1), bornées ; toute boucle d'attente sert `tuh_task`.
8. **Son** : horloge d'échantillons fractionnaire (fréquence exacte), mélange en
   entiers sans débordement, volume du socle.
9. **Démarrage** : la carte démarre sans clé USB (attente bornée) ; Pause = retour
   au firmware Neo6502.
10. **Sonde** : `~/Neo6502TeleStrat/docs/SONDE_SWD.md` (une session à la fois,
    bmarty attribue la carte, prise et restitution annoncées).

## 4. Validation commune

- **Sur PC, sans carte** (CTest ou `make test`) : cœurs 6502 (SingleStepTests de
  Tom Harte, Klaus Dormann : `tools/cputest/harte.c`, interface stable), `hid_media`,
  moteur de menu, et pour chaque système un banc sans fenêtre avec écrans de
  référence.
- **Charge du RP2040 mesurée sans carte** (Telestrat : `tools/rp2040_load.py`, NES) :
  à généraliser.
- **Sur la carte, même protocole pour tous** : démarrage sans clé, clavier, image
  (pas de trait rouge, 0 ligne en retard), temps par trame ≤ budget, son, clé USB
  (lecture, écriture), manette, touches multimédia, Pause.

## 5. Gouvernance

- **Le socle a un responsable** : la session reload-emulator. Un correctif trouvé
  ailleurs est d'abord signalé (SendMessage), puis fait dans le socle ; les
  projets le reprennent en suivant le socle, pas en recopiant.
- **Avant tout commit qui touche le socle** (`platforms/rp2040/src`, `src/`) :
  `tools/socle_consommateurs.sh` compile le NES et les deux variantes du Telestrat
  contre lui et affiche la RAM restant au-delà du tas de chaque firmware
  (leçon du 2026-09-30 : une inclusion ajoutée à `hid_app.c` a cassé le NES, qui
  n'a pas `src/` dans ses chemins ; les sources du socle incluent donc par chemin
  relatif).
- **Versions du socle épinglées** : reload pose une étiquette `socle-AAAA-MM-JJ[-n]`
  sur un commit vérifié (tests PC, firmwares de reload, consommateurs). Un projet
  consommateur compile contre une étiquette, pas contre la tête de reload :
  ```
  git -C ~/reload-emulator worktree add ~/.cache/reload-socle/socle-2026-09-30 socle-2026-09-30
  cmake … -DRELOAD_DIR=~/.cache/reload-socle/socle-2026-09-30
  ```
  et change d'étiquette quand il le décide. Le socle garde une interface stable,
  annoncée avant tout changement (comme `harte.c`, le volume de `audio.c`).

### Contraintes des consommateurs (à respecter par le socle)

| Consommateur | Contrainte | Conséquence pour le socle |
|---|---|---|
| Telestrat, variante RAM 64 Ko | 376 octets de RAM au-delà du tas réservé (2026-09-30) ; `CFG_TUH_ENUMERATION_BUFSIZE` 256 faute de place (ADR du Telestrat) | tout ajout de RAM (code en RAM, tampons) est vérifié par `socle_consommateurs.sh` avant commit |
| Telestrat | volume : `audio_set_volume`, `audio_volume_level`, `audio_volume_muted`, `audio_volume_serial`, `hid_media_key_down` ; `tools/cputest/harte.c` (`-x`, codes 0/1/77, `USE_65C02`) | interfaces stables |
| Telestrat | pilote de bus SIO intégré au tick (`neo6502_bus.h`), sans PIO | écart à la règle 6 jusqu'à l'étape 5 |
| NES | compile `hid_app.c` sans `src/` dans ses chemins d'inclusion | les sources du socle incluent par chemin relatif |
| Apple IIe (reload) | 0,6 Ko au-delà du tas | idem Telestrat 64 Ko |
| Atari2600 (futur) | chaque accès du 65C02 visible, factices compris | le pilote de bus ne filtre aucun cycle |
- Tout écart aux règles de la section 3 fait l'objet d'un ADR dans le projet.

## 6. Plan

| Étape | Contenu | Qui |
|---|---|---|
| 1 | ~~Corriger dans reload les défauts connus~~ (fait : tables MSC, verrous dédiés des Apple, IIe en 800×480 à 295,2 MHz ; encodeur des Apple à juger sur la carte) | reload |
| 2 | Signaler leurs défauts aux autres projets : verrous (Telestrat, NES), tas, OE3, TinyUSB 0.16 (Tangerine) | reload → sessions |
| 3 | Extraire `neo_platform.c` et `neo_video.c` (boucle, horloge, DVI) des `main` de l'Oric et du BBC | reload |
| 4 | Moteur de menu commun (Oric et Telestrat), un seul banc SWD | reload + Telestrat |
| 4b | **Puces communes** : une seule VIA 6522 pour tous. Trois existent : `src/chips/mos6522via.h` de reload (chemin paresseux), sa copie au Telestrat, `~/Neo6502Vic20/src/chips/via6522.c` (61/61 tests VIA de VICE, chemin paresseux prouvé par rejeu, EUPL/zlib, bmarty) : faire passer les tests VICE à celle de reload, puis garder la meilleure | reload + Vic20 |
| 5 | Le Telestrat et le NES remplacent leurs copies (`hid_app.c`, `usb_msc.c`, pilote de bus) par le socle | Telestrat, NES |
| 6 | Vic20 et Atari2600 démarrent leur port pour la carte directement sur le socle (Atari2600 d'accord, sous réserve de bmarty ; contraintes dans `~/Neo6502Atari2600/docs/adr/ADR-001-architecture-neo6502.md` : TIA rendu par tronçons ou sur le cœur 1 depuis un journal d'écritures, le bus doit laisser le système voir chaque accès, factices compris, car un accès factice peut changer de banque de cartouche) | Vic20, Atari2600 |
| 7 | Tangerine : adopter le socle, ou appliquer les règles de la section 3 (ADR) — règles appliquées en v0.6.1-0.6.2 (OE3 tenu jusqu'à PHI2, tas réservé, TinyUSB 0.21.0, BUFSIZE 512) ; écarts dans `~/Neo6502Tangerine/docs/adr/0004-regles-communes-neo6502.md` (DVI_VERTICAL_REPEAT 1 pour 256 lignes, bus SIO pour la co-simulation) ; choix du socle laissé à bmarty | Tangerine |

## 7. À décider (bmarty)

- **Horloge vidéo standard** (le BBC et l'Oric sont à 800×480 / 345 MHz depuis le
  2026-09-30, timing commun `neo_dvi_timings.h` ; l'Oric en a besoin pour tenir
  1 MHz en jeu) : 800×480 à 345 MHz (seul mode accepté par l'écran pour
  le BBC, d'après son ADR-04) ou 960×544 à 372 MHz (affiché d'après le Telestrat et
  le NES) : contradiction à trancher ce soir sur la carte, avec le même firmware.
- **Telestrat** : système de reload (fusion) ou projet séparé qui consomme le socle.
- **Trinity** : hors socle (firmware C++ du Neo6502), mais source des règles ;
  jusqu'où l'aligner.
- **Interfaces du socle déjà utilisées ailleurs**, à garder stables : `tools/cputest/harte.c`
  (Telestrat), volume de `audio.c` et `hid_media_key_down` (Telestrat v0.16.10).
- **Priorité mémoire au DMA** : Trinity a vu les traits rouges empirer avec elle ; le
  BBC, le NES et le Telestrat l'utilisent sans défaut mesuré.
