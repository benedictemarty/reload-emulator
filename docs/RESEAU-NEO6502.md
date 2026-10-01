# Réseau pour les émulateurs Neo6502 (TNFS par le modem Wi-Fi Neo6502picowifi)

Décisions de bmarty (2026-10-01) : les émulateurs montent un répertoire distant ; le réseau
passe par un **Pico W**, et plus précisément par le firmware **Neo6502picowifi**
(`~/Neo6502picowifi` : modem Wi-Fi au dialecte AT des ESP8266, USB CDC et UART, configuration
persistante, DNS, TLS, validé sur carte). Il ajoute l'UDP au format ESP8266 standard ; reload
parle ce dialecte. Une clé Wi-Fi USB est écartée : le RP2040 de la Neo6502, hôte USB, devrait
porter le pilote de sa puce, le Wi-Fi (WPA2) et une pile TCP/IP dans 264 Ko de RAM déjà pris.
Une passerelle maison (trames SLIP, firmware Pico W propre) a été écrite puis retirée le même
jour au profit de picowifi.

## Chaîne

```
émulateur (fichiers par neo_storage, volume « Réseau »)
  → client TNFS            src/devices/neo_tnfs.h
  → modem AT ESP8266       src/devices/neo_esp_at.h (AT+CIPSTART="UDP", AT+CIPSEND, +IPD)
  → port série             USB (hub de la Neo6502 : /dev/ttyACM sur PC) ou UART de l'UEXT
  → Neo6502picowifi        (Pico W : Wi-Fi, UDP) — ou un vrai module ESP8266 sur l'UEXT
  → serveur TNFS           tnfsd (PC, NAS…), port UDP 16384
```

- **TNFS** : protocole de fichiers réseau des 8 bits (Spectranet, FujiNet). Spécification et
  serveur de référence `tnfsd` (licence MIT) : https://github.com/FujiNetWIFI/spectranet/tree/master/tnfs.
  Le client suit les commandes obligatoires, plus UNLINK. TNFS par TCP n'est pas utilisable
  avec `tnfsd` à travers un modem : le serveur prend chaque `read()` pour un message entier.
- **Dialogue AT** (convenu avec la session Neo6502picowifi, chaînes de son `at_modem.c`) :
  `AT`, `ATE0` (nécessaire : l'écho peut être gardé en flash), `AT+CWJAP_DEF` si un réseau est
  donné (35 s : le modem relance jusqu'à 30 s), `AT+CIPSTATUS` jusqu'à `STATUS:2/3/4` (adresse
  IP obtenue : après son branchement, le modem rejoint son réseau en tâche de fond),
  `AT+CIPCLOSE` (ERROR toléré), `AT+CIPSTART="UDP","hôte",port` → `CONNECT`, `OK` (12 s : DNS
  jusqu'à 10 s dans le modem ; une adresse IP évite l'attente) ; `AT+CIPSEND=n` → `\r\nOK\r\n> `, n octets →
  `Recv n bytes`, `SEND OK` | `SEND FAIL` ; réception `\r\n+IPD,n:` + n octets, **un par
  datagramme** ; port local éphémère ; CIPMUX=0 ; pas de mode transparent en UDP.

## Essais (sur PC)

| Chemin | Où | Résultat |
|---|---|---|
| Client TNFS ↔ faux serveur (pertes, réponse périmée, EAGAIN) | `test_neo_tnfs` | 35 vérifications |
| Client → modem AT simulé → faux serveur (écho, lignes spontanées, +IPD avant ou après SEND OK, SEND FAIL) | `test_neo_esp` | 11 vérifications |
| `oric_headless -N 127.0.0.1` ↔ `tnfsd` (UDP direct) | intégration | CLOAD, Sedoric DIR en flux, SAVE puis LOAD |
| `oric_headless -S <pty> -N 127.0.0.1` ↔ `neo_esp_modem_pc` ↔ `tnfsd` | intégration | CLOAD, Sedoric en flux (voit le SAVE) |
| **Interpréteur AT réel de Neo6502picowifi** (`at_modem.c` de b054568, non modifié, compilé sur PC dans un harnais : pseudo-terminal, socket UDP connecté) ↔ `tnfsd` | essai du 2026-10-01 | CLOAD (7 requêtes), Sedoric SAVE (350) puis LOAD dans une nouvelle session ; 640 datagrammes, 0 perdu, 0 répétition |
| Pico W réel (Wi-Fi, lwIP) | — | à faire quand bmarty flashe `picow_modem.uf2` |

Intégration : `ORIC_TNFSD=…/tnfsd tests/oric/run_integration.sh …/oric_headless` (19 essais ;
sans `tnfsd`, les 5 essais réseau sont sautés). `neo_esp_modem_pc` (pseudo-terminal, UDP réel)
et `platforms/pc/neo_esp_fake.h` reproduisent le dialogue du modem.

## Essayer avec le Pico W

1. Flasher Neo6502picowifi (≥ b054568, `~/Neo6502picowifi/build/picow_modem.uf2`) sur le Pico W, le brancher sur le PC : `/dev/ttyACM0`.
2. Lancer `tnfsd` sur une machine du réseau Wi-Fi.
3. `NEO_WIFI_SSID=… NEO_WIFI_PASS=… oric_headless -S /dev/ttyACM0 -N <serveur> -T /JEU.TAP …`
   (sans `NEO_WIFI_SSID`, le réseau enregistré dans le modem est utilisé).

## Second port TNFS sur PicoWiFiModemUSB (décision de bmarty, 2026-10-01)

Un seul lien à la fois chez les deux modems : la Minitel du Telestrat (TCP) et les fichiers
(UDP) ne peuvent pas cohabiter sur le port AT. Décision : **PicoWiFiModemUSB** (`~/picowifi`,
le modem du Minitel du Telestrat) gagne un **second port série USB** réservé à TNFS, qui relaie
des datagrammes ; Neo6502picowifi et son AT UDP restent (module ESP8266, essais).

Côté reload, prêt et testé sur PC :
- `src/devices/neo_dgram_serial.h` : un datagramme par trame, **longueur sur 2 octets
  petit-boutiste** (proposé), puis le datagramme ; trames en retard vidées avant chaque
  requête, longueur fausse resynchronisée (`test_neo_dgram` : 7 vérifications) ;
- `platforms/rp2040/src/neo_cdc_serial.c` : deux ports série USB par ordre d'interface
  (port 0 = AT, port 1 = TNFS), `ctx` = numéro de port.

Convenu avec le Telestrat (2026-10-01) : longueur **petit-boutiste** ; serveur donné par
`AT$TNFS=hôte:port` sur le port 0, gardé en flash par `AT&W` ; le port 1 ne porte que des
datagrammes. Reste : l'accord de la session qui tient PicoWiFiModemUSB, puis son
implémentation. L'Oric n'utilise pas encore ce port (il parle AT UDP sur le port 0).
Coût mesuré par le Telestrat : `CFG_TUH_CDC=2` = +504 o de RAM (sa variante 64 Ko tomberait
à 220 o, sous son seuil de 300 o : place à regagner avant).

## Sur la Neo6502 (Oric, 2026-10-01, compilé, non essayé sur la carte)

- `platforms/rp2040/src/neo_cdc_serial.c` (socle) : port série USB côté hôte (TinyUSB CDC,
  `CFG_TUH_CDC=1` dans le CMake du firmware) au format de `neo_esp_init`.
- Oric : `ORIC.CFG` `reseau=`, `wifi=`, `wifi_mdp=` ; au branchement du modem, le volume
  `NEO_VOL_NET` est monté et ses fichiers listés `net:/NOM` dans le menu ; `neo_storage_split`
  choisit le volume d'après le préfixe du nom. RAM : 3,6 Ko (oric : 9 416 o de marge,
  **oric_diag : 220 o, très serré**).
- Liste d'un répertoire distant : STAT de chaque entrée pendant la lecture (vérifié contre
  `tnfsd` : tailles, répertoires).

## Reste à faire

- Essai du Pico W sur PC, puis sur la carte ; UART de l'UEXT en option ; autres systèmes.
