# Réseau pour les émulateurs Neo6502 (TNFS par une passerelle Pico W)

Décision de bmarty (2026-10-01) : les émulateurs montent un répertoire distant ; matériel
disponible : un Pico W (retenu) ou un adaptateur USB → RJ45. Une clé Wi-Fi USB est écartée :
le RP2040 de la Neo6502, hôte USB, devrait porter le pilote de sa puce, le Wi-Fi (WPA2) et
une pile TCP/IP dans 264 Ko de RAM déjà pris par l'émulation.

## Chaîne

```
émulateur (fichiers par neo_storage, volume « Réseau »)
  → client TNFS            src/devices/neo_tnfs.h
  → liaison série          src/devices/neo_netlink.h (trames SLIP + CRC16)
  → port série USB         (hub de la Neo6502 ; ou UART de l'UEXT)
  → passerelle Pico W      platforms/picow/netbridge (Wi-Fi CYW43, lwIP, UDP)
  → serveur TNFS           tnfsd (PC, NAS…), port UDP 16384
```

- **TNFS** : protocole de fichiers réseau des 8 bits (Spectranet, FujiNet). Spécification et
  serveur de référence `tnfsd` (licence MIT) : https://github.com/FujiNetWIFI/spectranet/tree/master/tnfs.
  Le client suit les commandes obligatoires, plus UNLINK.
- **Trames** (`neo_netlink.h`) : `SLIP(type, corps, CRC16-CCITT)`. Types : requête et réponse
  TNFS, configuration (réseau Wi-Fi, mot de passe, adresse IP du serveur, port), demande
  d'état et état (Wi-Fi, adresses), journal.
- **Passerelle** : un seul cœur (`neo_netbridge_*`), compilé pour le Pico W et pour le PC
  (`platforms/pc/netbridge/neo_netbridge_pc`, sur un pseudo-terminal), sa jumelle des tests.

## Essais

| Chemin | Où | Résultat |
|---|---|---|
| Client TNFS ↔ faux serveur (pertes, réponse périmée, EAGAIN) | `test_neo_tnfs` | 35 vérifications |
| Trames, chaîne client → liaison → passerelle → faux serveur | `test_neo_netlink` | 19 vérifications |
| `oric_headless -N 127.0.0.1` ↔ `tnfsd` (UDP direct) | intégration | CLOAD, Sedoric DIR en flux, SAVE puis LOAD |
| `oric_headless -S <pty>` ↔ `neo_netbridge_pc` ↔ `tnfsd` | intégration | CLOAD, Sedoric en flux (voit le SAVE) ; 275 requêtes, 0 trame abîmée |
| Firmware Pico W | compilé (SDK 1.5.1 du socle, `pico_w`) | **non essayé** (pas de Pico W branché) |

Intégration : `ORIC_TNFSD=…/tnfsd tests/oric/run_integration.sh …/oric_headless` (19 essais ;
sans `tnfsd`, les 5 essais réseau sont sautés).

## Essayer le Pico W (sur le PC d'abord)

1. Compiler : `cmake -S platforms/picow/netbridge -B platforms/picow/build && cmake --build platforms/picow/build`
   (sous-modules du SDK `lib/cyw43-driver` et `lib/lwip` initialisés le 2026-10-01).
2. Flasher `platforms/picow/build/neo_netbridge.uf2` (BOOTSEL), le brancher sur le PC : `/dev/ttyACM0`.
3. Lancer `tnfsd` sur un PC du réseau Wi-Fi (adresse IP, pas de nom : pas de DNS dans cette version).
4. `NEO_WIFI_SSID=… NEO_WIFI_PASS=… oric_headless -S /dev/ttyACM0 -N <IP du serveur> -T /JEU.TAP …`
   (la DEL du Pico W s'allume quand le Wi-Fi est connecté).

## Reste à faire

- Sur la Neo6502 : transport série USB hôte (TinyUSB CDC hôte : `CFG_TUH_CDC` est à 0 dans le
  socle aujourd'hui), configuration réseau lue sur la clé, volume « Réseau » dans le menu.
- Pico W : noms de serveur (DNS), configuration gardée en flash, UART de l'UEXT en option.
