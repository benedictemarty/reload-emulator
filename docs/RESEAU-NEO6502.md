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
  `AT`, `ATE0`, `AT+CIPCLOSE` (ERROR toléré), `AT+CWJAP_DEF` si un réseau est donné,
  `AT+CIPSTART="UDP","hôte",port` → `CONNECT`, `OK` ; `AT+CIPSEND=n` → `\r\nOK\r\n> `, n octets →
  `Recv n bytes`, `SEND OK` | `SEND FAIL` ; réception `\r\n+IPD,n:` + n octets, **un par
  datagramme** ; port local éphémère ; CIPMUX=0 ; pas de mode transparent en UDP.

## Essais (sur PC)

| Chemin | Où | Résultat |
|---|---|---|
| Client TNFS ↔ faux serveur (pertes, réponse périmée, EAGAIN) | `test_neo_tnfs` | 35 vérifications |
| Client → modem AT simulé → faux serveur (écho, lignes spontanées, +IPD avant ou après SEND OK, SEND FAIL) | `test_neo_esp` | 11 vérifications |
| `oric_headless -N 127.0.0.1` ↔ `tnfsd` (UDP direct) | intégration | CLOAD, Sedoric DIR en flux, SAVE puis LOAD |
| `oric_headless -S <pty> -N 127.0.0.1` ↔ `neo_esp_modem_pc` ↔ `tnfsd` | intégration | CLOAD, Sedoric en flux (voit le SAVE) |
| Neo6502picowifi réel | — | **attend sa version UDP** |

Intégration : `ORIC_TNFSD=…/tnfsd tests/oric/run_integration.sh …/oric_headless` (19 essais ;
sans `tnfsd`, les 5 essais réseau sont sautés). `neo_esp_modem_pc` (pseudo-terminal, UDP réel)
et `platforms/pc/neo_esp_fake.h` reproduisent le dialogue du modem.

## Essayer avec le Pico W (dès que picowifi a l'UDP)

1. Flasher Neo6502picowifi sur le Pico W, le brancher sur le PC : `/dev/ttyACM0`.
2. Lancer `tnfsd` sur une machine du réseau Wi-Fi.
3. `NEO_WIFI_SSID=… NEO_WIFI_PASS=… oric_headless -S /dev/ttyACM0 -N <serveur> -T /JEU.TAP …`
   (sans `NEO_WIFI_SSID`, le réseau enregistré dans le modem est utilisé).

## Reste à faire

- Sur la Neo6502 : liaison série vers le modem (TinyUSB CDC hôte : `CFG_TUH_CDC` est à 0 dans
  le socle ; ou UART de l'UEXT), configuration réseau lue sur la clé, volume « Réseau » dans
  le menu.
