# Apple ][+ et Apple //e : état de l'émulation

Fichiers : `src/systems/apple2.h`, `src/systems/apple2e.h`, `src/devices/disk2_*.h`,
`src/devices/prodos_hd*.h`, `src/devices/apple2_lc.h`. Test : `tests/apple2/`.

## Disque dur ProDOS (`prodos_hdc.h`, `prodos_hdd.h`)
- Une seule unité (`$70`), image **brute** `.po` / `.hdv` (les `.2mg` sont refusés), au moins un bloc.
- STATUS annonce au plus 65 535 blocs (16 bits).
- Après une lecture de bloc, l'écran est redessiné en entier (le contrôleur écrit en RAM sans passer par le bus).

## Disk II (`disk2_fdc.h`, `disk2_fdd.h`) — désactivé dans les quatre builds
- Deux lecteurs sélectionnables ; le lecteur 2 reste vide.
- Format d'image **maison** : 35 pistes × 16 secteurs × 374 octets (5984 octets par piste), produit par
  `tools/dsk2nib`. Ce n'est **pas** le `.nib` standard (6656 octets par piste).
  `disk2_fdd_insert_disk_sized` refuse toute autre taille.
- Lecture seule : la protection est signalée sur le bit 7 (le RWTS refuse d'écrire) ; la tête avance d'un
  octet par lecture, sans notion de temps : les protections de copie échouent.

## Vidéo
- Le tampon d'image est commun à tous les modes. Un changement de mode (TEXT, MIXED, PAGE2, HIRES, DHIRES,
  80COL, ALTCHARSET, 80STORE) redessine tout. Les drapeaux de page sales ne sont effacés qu'une fois par
  image, après la fenêtre de texte du mode mixte.
- //e : avec 80STORE, PAGE2 choisit la banque, pas la page affichée.

## //e
- RESET remet à zéro 80STORE, RAMRD, RAMWRT, ALTZP, INTCXROM, SLOTC3ROM, 80COL, ALTCHARSET et replace la
  carte langage sur la ROM, banque 2, écriture protégée. **Choix à vérifier contre la Technical Reference.**
- `$C019` (RDVBLBAR) : bit 7 à 0 pendant la suppression verticale. Image de 17 030 cycles.

## Clavier (PC)
- Échap = ESC (`$1B`), Retour arrière = DELETE (`$7F`), flèche gauche = `$08`. Le //e envoie les minuscules.

## Reste à faire
Voir ROADMAP.md (local) : factorisation `apple2.h` / `apple2e.h`, instantanés (code mort), clavier
(AKD, miroirs `$C000-$C01F` du ][+), slots vides du //e, MouseText.
