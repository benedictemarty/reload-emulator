# BBC — recette sur la carte (US-47 et US-48)

Liste de vérifications à faire par un humain sur la carte. Les messages cités sont ceux du
code (`platforms/rp2040/systems/bbc/src/bbc.c`). Cocher chaque ligne, noter l'écart observé.
Aucune de ces vérifications n'a été faite : US-47 et US-48 restent « à valider sur carte ».

Préparation : flasher `bbc` (puis `bbc_master`), clé USB avec au moins deux images `.ssd` à la
racine, panneau ouvert avec **F11**.

## 0. Panneau F11 (commit 6307946)

- [ ] Variante `bbc` : l'en-tête affiche « Model B » ; variante `bbc_master` : « Master 128 ».
- [ ] Au démarrage, le volume n'est pas à 0 et le lancement automatique est actif.

## 1. US-47 — BBC.CFG

Format (`src/osd/bbc_config.h`) : `lecteur0=`, `lecteur1=` (fichier de la clé, ou `@Nom` pour une
image en flash), `insertion=demarrer|inserer`, `volume=0..8`.

- [ ] Clé sans BBC.CFG : première image de la liste insérée, comme avant.
- [ ] BBC.CFG avec `lecteur0=<fichier>` et `insertion=demarrer` : au montage de la clé, le
      lecteur 0 est inséré et la machine démarre dessus.
- [ ] Même fichier avec `insertion=inserer` : image insérée, sans SHIFT+BREAK.
- [ ] `lecteur1=@<nom d'une image en flash>` : lecteur 1 sur l'image en flash.
- [ ] `lecteur1=` vide : lecteur 1 éjecté.
- [ ] Nom inconnu : le lecteur reste inchangé (message série « BBC.CFG: no image … »).
- [ ] `volume=3` : volume audible réduit, valeur visible dans le panneau.
- [ ] Bouton « Enregistrer la configuration » : message « Configuration enregistrée dans
      BBC.CFG » ; relire le fichier sur le PC : lecteurs, insertion, volume à jour, **commentaires
      et lignes d'autres clés conservés**.
- [ ] Même bouton clé retirée : message « BBC.CFG : écriture impossible (clé USB ?) ».
- [ ] Après enregistrement, redémarrer la carte : la configuration est réappliquée.

## 2. US-48 — instantanés BBCnnnn.STA

- [ ] Panneau : action « nouvel instantané » : message « Instantané BBC0001.STA enregistré
      (N Ko) » ; le fichier existe sur la clé ; la numérotation suit (BBC0002…).
- [ ] L'écran de l'émulation reprend normalement après l'enregistrement.
- [ ] Reprise d'un instantané depuis le sélecteur : la machine reprend exactement où elle était
      (jeu, position, son).
- [ ] Reprise après un changement de MODE ou une session différente : même résultat.
- [ ] Enregistrer pendant un accès disque : refus « Instantané refusé : accès disque en cours »,
      aucun fichier à moitié écrit.
- [ ] Clé retirée : « Instantané : écriture impossible (clé USB ?) ».
- [ ] Fichier `.STA` d'un autre format ou d'une autre version : message « Pas un instantané de ce BBC (version ou modèle) » ;
      fichier **tronqué** : « Instantané tronqué : machine inchangée » ; sur la variante Master (ou avec le Tube), tout
      instantané : « Instantanés : Model B seulement ». Dans tous ces cas la machine continue sans être touchée.
- [ ] F12 (BREAK) et F11 pendant un accès disque long : pris en compte entre deux images, sans plantage ni état mélangé.
- [ ] Taille d'un instantané relevée : ______ Ko ; durée d'enregistrement et de reprise : ______ s.

## Résultat

Date : ________  Carte : ________  Version flashée : ________
Écarts : ____________________________________________
