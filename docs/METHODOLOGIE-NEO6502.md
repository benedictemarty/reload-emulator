# Méthodologie et enseignements du Neo6502, expliqués simplement

Ce document explique **comment** le travail est mené sur les émulateurs de la carte
Neo6502 (la méthode), **avec quels mots** (le glossaire), et **ce qu'on en a appris
de général** sur cette carte (les enseignements). Les exemples sont réels : ils
viennent du 2026-09-30.

---

## 1. Le décor en une page

**Le Neo6502** est une petite carte d'Olimex qui porte deux processeurs :

- un **vrai 6502** (le W65C02, version moderne du processeur des années 1980 qui
  animait l'Apple II, le BBC Micro, l'Oric, le Commodore 64…) ;
- un **RP2040**, le microcontrôleur du Raspberry Pi Pico, beaucoup plus rapide.

Le 6502 n'a ni mémoire, ni écran, ni clavier à lui : **c'est le RP2040 qui joue
tout le reste de l'ordinateur**. À chaque battement d'horloge du 6502, le RP2040
lit l'adresse que le 6502 demande, lui répond (ou reçoit ce qu'il écrit), et fait
évoluer les autres puces imitées (vidéo, son, clavier, lecteurs). Il fabrique aussi
l'image envoyée à l'écran et parle au clavier et à la clé USB.

**Un émulateur** est donc ici un programme du RP2040 qui fait croire au vrai 6502
qu'il est dans un Oric, un BBC Micro ou un Apple II.

---

## 2. Glossaire (les mots, redéfinis)

**Cycle** — Un battement de l'horloge du 6502. À 1 MHz, il y en a un million par
seconde : le RP2040 a donc **1 microseconde** pour tout faire à chaque cycle.

**Budget** — Le temps disponible pour une tâche. Une image de l'Oric dure
19 968 µs (50 images par seconde). Si l'émulation d'une image prend plus que ce
budget, l'ordinateur imité tourne **moins vite que le vrai**. On parle en
pourcentage : 83 % du budget = il reste 17 % de marge.

**Temps réel** — L'émulation va exactement à la vitesse de la machine d'origine :
ni plus vite, ni plus lentement. Condition : rester sous 100 % du budget.

**Bus** — Les fils qui relient le 6502 au reste : adresse (16 fils), donnée
(8 fils), lecture ou écriture (1 fil), horloge (PHI2). Sur le Neo6502, adresse et
donnée **partagent les mêmes 8 fils** du RP2040 : il faut les lire en plusieurs
fois, à travers trois petits aiguillages (les 74HC245 : OE1, OE2, OE3).

**Bit-bang / SIO** — Piloter les fils du bus « à la main », instruction par
instruction, depuis le programme du RP2040. Simple, mais le processeur attend
pendant chaque manœuvre.

**PIO** — Des petits automates matériels du RP2040 qui pilotent des fils tout
seuls. Avec eux, le bus avance **pendant que** le programme calcule autre chose.

**Cœur** — Le RP2040 a deux processeurs internes (cœur 0 et cœur 1). Ici : cœur 0
= le 6502 et les puces imitées ; cœur 1 = l'image et le son.

**Flash / RAM / cache XIP** — La **flash** garde le programme même carte éteinte,
mais elle est lente ; un **cache** (16 Ko) en garde les morceaux récents. La **RAM**
est rapide mais petite (264 Ko en tout) et doit aussi contenir la mémoire de la
machine imitée, les images, etc.

**DVI / TMDS / PicoDVI** — Le signal vidéo numérique (celui d'un câble HDMI). Le
RP2040 le fabrique par logiciel avec la bibliothèque **PicoDVI** ; chaque ligne doit
être **encodée** (TMDS) à temps, sinon l'écran montre des défauts.

**Tas (heap)** — La réserve de RAM où un programme prend de la place à la demande.
PicoDVI y prend ≈ 22 Ko au démarrage ; s'il n'y en a pas assez, la carte reste muette.

**Mode paresseux (lazy)** — Une puce imitée qui ne recalcule rien tant que rien ne
peut arriver (par exemple un minuteur loin de zéro) : elle se contente de compter
les cycles sautés. Énorme économie… tant que rien ne la réveille pour rien.

**Test / test de non-régression** — Un programme qui vérifie automatiquement un
comportement. **Non-régression** : vérifier qu'un changement n'a rien cassé de ce
qui marchait. Ici ils tournent **sur PC** (sans la carte), en quelques minutes.

**Référence / oracle** — Un émulateur réputé exact (Oricutron, b2, VICE, les
SingleStepTests de Tom Harte pour le 6502) à qui l'on compare nos résultats.

**Mesure** — Un chiffre relevé **sur la vraie carte** (par exemple le temps d'une
image), pas une estimation.

**Hypothèse** — Une explication **pas encore vérifiée**. Elle est toujours signalée
comme telle, jamais présentée comme un fait.

**Profilage / échantillonnage** — Savoir où le programme passe son temps : on
arrête le processeur des centaines de fois au hasard, on note où il en était, puis
on le relance. Les endroits les plus souvent notés sont les plus coûteux.

**Sonde SWD** — Un petit boîtier (Debugprobe) branché sur la carte qui permet de la
programmer (« flasher »), de lire sa mémoire et de l'arrêter, depuis le PC.

**Socle** — Le code commun à tous les émulateurs de la carte (bus, vidéo, son, USB,
menus) ; chaque émulateur n'ajoute que sa machine. Voir STRATEGIE-NEO6502.md.

**Sprint** — Une petite étape de travail, avec un but clair, terminée par des tests,
un enregistrement (commit) et une mise à jour de la documentation.

**Commit** — Un enregistrement daté et commenté d'un changement dans l'historique
(git). On peut toujours revenir en arrière.

---

## 3. La méthode, en 8 règles

### Règle 1 — Lire le code avant d'affirmer

Toute affirmation sur le logiciel est vérifiée dans le code (fichier et ligne).
Quand un fait n'est pas trouvé, on écrit « non trouvé » ou « je ne sais pas »,
jamais une supposition habillée en certitude.

*Exemple* : un rapport annonçait que le BBC tournait en 960×544 à 372 MHz ; le code
disait 800×480 à 345 MHz. C'est le code qui fait foi, et le mode d'emploi en retard
a été signalé.

### Règle 2 — Mesurer avant de croire (et après avoir changé)

Une amélioration n'existe que si une mesure sur la carte la montre.

*Exemple vécu* : l'idée « mettre le code en RAM accélère » était raisonnable. Mesurée
sur la carte, elle a donné… **un Oric un peu plus lent** (112,7 % contre 109,1 %).
Sans mesure, on aurait cru avoir gagné.

*Suite vécue* : la boucle BASIC montrait 17 % de marge ; l'écran-titre d'un vrai jeu
(*3D Fongus*) a montré **105 %**. **Mesurer sous une vraie charge**, pas seulement sur
un banc léger.

### Règle 3 — Chercher où va vraiment le temps

Avant d'optimiser, on découpe le coût. Deux outils simples :

- **Une micro-mesure** : chronométrer 40 000 cycles du bus seul, puis 40 000 cycles
  complets. Résultat : bus ≈ ¼ du temps, puces imitées ≈ ¾.
- **Un échantillonnage** : 800 relevés de l'endroit où le processeur se trouve.
  Résultat : la VIA (puce d'entrées-sorties) prenait 14 % et ne profitait jamais de
  son mode paresseux.

C'est ce découpage qui a trouvé la vraie cause (la VIA réveillée tous les 4 cycles
par le clavier), là où deux tentatives « au jugé » (RAM, puis PIO) n'avaient donné
que peu ou rien.

### Règle 4 — Un filet de tests avant de toucher

Chaque changement passe les tests sur PC : cœurs 6502 (2,5 millions de cas),
appareils, menus, et tests d'intégration (démarrer le BASIC, charger une cassette,
lire une disquette Sedoric ou FT-DOS…). Pour une optimisation, la règle est
stricte : **le comportement imité ne doit pas changer**, seulement le temps pris.

*Exemple* : n'appeler la VIA que quand l'entrée change a été justifié ligne par ligne
(« un appel avec la même valeur ne modifie rien »), puis confirmé par les tests
(13/13 en intégration), puis mesuré sur la carte (82,9 % du budget).

### Règle 5 — Comparer à une référence

Quand c'est possible, on compare à un émulateur de référence ou à un jeu de tests
public, plutôt qu'à sa propre idée du bon résultat.

*Exemple* : les SingleStepTests de Tom Harte ont trouvé deux défauts du cœur 65C02
(soustraction décimale, instructions BBR/BBS) qu'aucun jeu n'avait montrés.

### Règle 6 — Petits pas, chacun enregistré

Un changement à la fois, un commit par sujet, un message qui dit **quoi** et
**pourquoi**. Un journal (CHANGELOG) et une feuille de route (ROADMAP) sont tenus à
jour à chaque pas. Si un pas casse quelque chose, on sait lequel.

### Règle 7 — Dire ce qui n'est pas fait ou pas vérifié

Chaque compte rendu sépare : **fait et vérifié**, **fait mais non essayé sur la
carte**, **pas fait**. Les échecs sont rapportés avec leurs chiffres.

*Exemple* : « j'avais cassé la compilation du NES » a été dit, corrigé, puis un
outil a été ajouté pour que cela ne se reproduise pas.

### Règle 8 — Partager la solution, pas la recopier

Un problème réglé pour un émulateur est très souvent le même pour les autres. On le
signale aux autres projets, et on le corrige **une fois**, dans le code commun (le
socle), plutôt que dans cinq copies.

---

## 4. Les enseignements génériques sur le Neo6502

Ce qui suit vaut pour **tout** émulateur ou programme qui utilise le RP2040 de la
carte pour imiter un ordinateur autour du vrai 6502.

### 4.1 Le temps

1. **Le budget est minuscule.** À 1 MHz, avec le RP2040 à 295 MHz, il reste environ
   300 cycles d'horloge du RP2040 (donc au plus autant d'instructions simples) par
   cycle du 6502 ; à 2 MHz, la moitié. Tout ce qui
   est fait « à chaque cycle » compte.
2. **Le bus coûte un quart, le reste coûte plus.** Sur l'Oric : ≈ 0,24 µs par cycle
   pour le bus, ≈ 0,6 à 0,85 µs pour les puces imitées. Accélérer seulement le bus
   ne suffit pas.
3. **Le travail périodique se groupe.** Ce qui n'arrive que tous les 4, 64 ou
   128 cycles ne doit pas être *testé* à chaque cycle.
4. **Les puces paresseuses doivent le rester.** Un appel « pour rien » (même valeur)
   qui réveille une puce annule tout le bénéfice : n'appeler que sur changement.
5. **Écrire une broche coûte** : ne pas réécrire la ligne d'interruption (IRQ) du
   6502 à chaque cycle, seulement quand elle change.
6. **RAM n'est pas toujours plus rapide que flash** : si le code tient dans le cache
   de la flash, le mettre en RAM peut le ralentir, car la RAM est partagée avec
   l'image (cœur 1 et DMA prioritaires). À mesurer, pas à supposer.
7b. **Mesurer sous charge réelle** : un jeu sollicite davantage les minuteurs, le
   son et les lecteurs qu'une boucle BASIC ; la marge doit être mesurée en jeu.
7c. **L'horloge est un levier** : le même affichage 800×480 à 345 MHz au lieu de
   295,2 MHz (effacement allongé) donne 17 % de calcul en plus ; l'Oric en jeu passe
   de 105 % à 90 % du budget. À valider écran par écran, et l'échauffement à 1,25 V
   reste à mesurer.
7. **Le PIO libère le processeur** pendant les délais du bus : gain de quelques
   points seul (106,6 % contre 109,1 % en flash et 112,7 % en RAM), et toujours le
   meilleur une fois le reste allégé (82,9 % contre 86,3 %).

### 4.2 L'image

8. **Pour 8 couleurs, encoder en 3 plans de 1 bit** : l'encodeur « à palette » donne
   des traits rouges sur certains écrans (celui de bmarty).
9. **PicoDVI a besoin de ses propres verrous**, sinon la clé USB et le système de
   fichiers le retardent (lignes en retard, traits rouges pendant les accès disque).
10. **PicoDVI double déjà chaque ligne** : ne pas la doubler une seconde fois.
11. **Réserver ≈ 22 Ko de tas à la compilation** : un manque de mémoire doit faire
    échouer la compilation, pas la carte au démarrage.
12. **L'horloge vidéo dépend de l'écran** : 800×480 à 295 ou 345 MHz est accepté par
    l'écran de bmarty ; 372 et 400 MHz sont à vérifier écran par écran.

### 4.3 Le 6502 réel et son bus

13. **Le 65C02 est statique mais le bus n'est pas une mémoire** : une donnée posée
    brièvement fuit si l'horloge reste arrêtée quelques millisecondes (entre deux
    images). Il faut la **tenir** jusqu'à la fin du cycle.
14. **Le 65C02 n'est pas le 6502 d'origine** : les instructions « illégales » du
    6502 NMOS (utilisées par certains jeux) n'existent pas, et ses accès « factices »
    diffèrent. Un logiciel qui en dépend ne marchera pas sur la carte, quel que soit
    l'émulateur.
15. **Chaque cycle est vu** : le RP2040 voit tous les accès du 6502, factices
    compris ; ne rien filtrer (certains matériels réagissent à une simple lecture).

### 4.4 L'USB et la clé

16. **Utiliser TinyUSB 0.21 ou plus récent** : avant, une clé USB branchée avec un
    clavier pouvait ne jamais être lue.
17. **Monter la clé depuis la boucle principale**, jamais depuis un rappel USB ;
    borner chaque transfert (1 s) ; ranger les volumes par emplacement (adresse − 1).
18. **Toute attente doit servir l'USB**, et la carte doit démarrer **sans** clé.
19. **Un clavier multimédia est plusieurs périphériques** : ses touches de volume
    arrivent sur une interface à part, à décrire (descripteur HID).

### 4.5 Le travail avec la carte

20. **Une seule carte, une seule sonde, une session à la fois** ; bmarty l'attribue,
    on annonce la prise et la restitution (et le firmware laissé).
21. **Ne jamais laisser un cœur arrêté.** Avec OpenOCD, arrêter les deux cœurs
    ensemble peut empêcher de les relancer : pour observer, ne s'attacher qu'au
    cœur 0 (`set USE_CORE 0`), et vérifier ensuite que la carte tourne.
22. **Préparer sur PC, confirmer sur la carte** : tout ce qui peut être testé sans la
    carte l'est avant ; la carte sert à mesurer et à regarder.

---

## 5. Le cas d'école du 2026-09-30, résumé

| Étape | Idée | Mesure sur la carte | Leçon |
|---|---|---|---|
| Départ | — | Oric à 109 % du budget : **pas en temps réel** | on ignorait qu'il était lent |
| Sprint 1 | code en RAM | 112,7 % : **pire** | règle 2 : mesurer |
| Sprint 2 | bus en PIO | 106,6 % : un peu mieux | le bus n'était pas la cause principale |
| Profilage | découper le coût | bus ¼, puces ¾ ; VIA 14 %, jamais paresseuse | règle 3 : chercher où va le temps |
| Correctif | VIA laissée paresseuse, travail groupé par 4, IRQ sur changement | **82,9 %** : temps réel, 17 % de marge | une cause trouvée vaut mieux que deux paris |
| Vrai jeu | *3D Fongus* | **105 %** : pas en temps réel | mesurer sous charge réelle |
| Horloge | 345 MHz, même image | **90 %** en jeu, 70 % en BASIC | l'horloge est un levier, à valider à l'écran |

Chaque étape est dans l'historique (git) et chiffrée dans PERFORMANCE-RP2040.md.

---

Voir aussi : [STRATEGIE-NEO6502.md](STRATEGIE-NEO6502.md) (le socle commun),
[PERFORMANCE-RP2040.md](PERFORMANCE-RP2040.md) (les mesures),
[CPU-TESTS.md](CPU-TESTS.md) (les tests du 6502),
`~/Neo6502TeleStrat/docs/SONDE_SWD.md` (la sonde).
