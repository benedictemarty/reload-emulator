# Tests des cœurs 6502 (version PC)

Deux cœurs logiciels, pas à pas au cycle, servent sur PC (et pour le 6502 du Tube
du BBC) : `src/chips/mos6502cpu.h` (NMOS, floooh) et `src/chips/w65c02cpu.h`
(W65C02S). Sur la carte, le vrai W65C02 est piloté sur le bus.

## Jeux de tests

| Test CTest | Programme | Données (hors du dépôt) |
|---|---|---|
| `cpu_harte_wdc65c02` | `harte_c02` (`w65c02cpu.h`) | SingleStepTests `wdc65c02/v1` |
| `cpu_harte_6502` | `harte_nmos` (`mos6502cpu.h`) | SingleStepTests `6502/v1` |
| `cpu_klaus_6502_c02`, `cpu_klaus_6502_nmos` | `cputest_c02`, `cputest_nmos` | `6502_functional_test.bin` |
| `cpu_klaus_65c02` | `cputest_c02` | `65C02_extended_opcodes_test.bin` |

- **SingleStepTests** de Tom Harte (https://github.com/SingleStepTests/65x02) :
  10 000 tests par opcode, une instruction chacun, avec l'accès au bus de chaque cycle.
  `tools/cputest/fetch_harte.sh` les télécharge (≈ 2 Go) dans `~/.cache/65x02`
  (variable CMake `HARTE_DIR`).
- **Tests de Klaus Dormann** : images du dépôt st3fan/ewm (`roms/`), dans
  `~/.cache/klaus` (variable `KLAUS_DIR`) ; succès en `$3399` et `$24A8` pour ces
  images (`ewm-core/tests/dormann.rs`), alors que les images de `bin_files/` de
  Klaus Dormann s'arrêtent en `$3469` et `$24F1`.

Sans données, les tests sont ignorés (code 77, « Not Run »).

## Deux niveaux de comparaison (`tools/cputest/harte.c`)

- **fonctionnel** (compte comme échec) : registres et RAM finals, nombre de cycles,
  octets écrits (adresse, valeur, ordre) ;
- **bus** (signalé, non compté) : en plus, l'adresse et la valeur de chaque lecture,
  lectures factices comprises. Elles ne changent le résultat que si elles tombent
  sur un registre d'entrée-sortie.

P est comparé sans les bits 4 (B) et 5.

```
harte_c02 [-b] [-v] [-x 5c] ~/.cache/65x02/wdc65c02/v1 [OPCODES…]
```

`-b` : premier écart de bus de chaque opcode ; `-v` : tous les échecs ; `-x` :
opcodes aux écarts connus, exécutés et affichés mais non comptés.

## Correctif BRK (2026-10-02)

Signalé par Neo6502nes (v0.8.3, mesuré sur la carte) : pour un BRK en A, le cycle 2 lit
l'octet de signature en **A+1** (puis PC = A+2), et non en A+2. Avant : 10 000 écarts de bus
sur l'opcode 00 (« cycle 2 : A+2 read, attendu A+1 ») ; après : 0. Effet visible quand A+1
tombe sur un registre d'entrée-sortie (nes-test-roms cpu_exec_space_ppuio, $2007).

## Résultats (2026-09-30)

| Cœur | Tests | Échecs fonctionnels | Écarts de bus seuls |
|---|---|---|---|
| `w65c02cpu.h` | 2 540 000 (254 opcodes, sans WAI/STP) | 0 hors `$5C` | 482 360 |
| `mos6502cpu.h` | 2 560 000 (256 opcodes) | 0 hors JAM et opcodes instables | 0 |

Corrigé dans `w65c02cpu.h` grâce à ces tests :
- **SBC décimal** avec des opérandes BCD invalides (338 échecs sur 10 000 pour
  `SBC #`, aucun sur des opérandes valides) : algorithme 65C02 de B. Clark
  (« Decimal Mode », 6502.org, annexe A) ;
- **BBRx / BBSx** : un cycle de moins que prévu quand le branchement est pris ; la
  valeur de page zéro est lue deux fois avant l'offset.

Repris de la NES (Neo6502nes v0.4.2, mesuré sur le W65C02S de la carte, fusion du
2026-09-30) — les tests de Harte ne couvrent pas les interruptions :
- **IRQ et NMI scrutées à l'avant-dernier cycle** d'une instruction, avec le drapeau I
  d'avant CLI/SEI/PLP (champs `nmi_prev`/`nmi_poll`/`irq_prev`/`irq_poll`/`iflag_top`) ;
- **pas de détournement de vecteur** : une NMI arrivée pendant BRK/IRQ est prise juste
  après la séquence, avant la première instruction du gestionnaire.
- Validé par les ROM de la communauté (`cpu_interrupts_v2` 1, 2, 3, 5 ; `ppu_vbl_nmi`
  04, 05, 07, 08 : mêmes résultats que la carte), 52 ROM réussies et Popeye identique
  trame par trame côté NES ; Harte, Klaus et intégrations reload ; `make test` du
  Telestrat (rejeu à l'identique). Coût par cycle du parasite Tube du BBC sur la carte
  (quelques écritures de plus) : non mesuré.

Écarts connus (option `-x` des tests CTest) :
- **WDC `$5C`** : 4 cycles dans les tests (WDC, Rockwell et Synertek), 8 dans le
  cœur. **Mesuré sur le W65C02S de la carte Neo6502 le 2026-09-30 : 8 cycles.** La
  VIA de l'Oric (émulée, un tic par cycle de bus du vrai 65C02) chronomètre une
  routine avec K instructions `5C 00 00` : K = 1, 10, 20 → 24, 96, 176 cycles, soit
  8 cycles par `$5C` (pente) ; le cœur est juste, les SingleStepTests ne le sont pas
  pour ce processeur. L'exclusion `-x 5c` reste.
- **NMOS JAM** (`$02`, `$12`… `$F2`) : le processeur se bloque, ce qui ne se
  teste pas par instruction.
- **NMOS SHA / TAS / SHY / SHX** (`$93`, `$9B`, `$9C`, `$9E`, `$9F`) : opcodes non
  documentés instables (≈ 48 % d'écarts, cas de changement de page).

Écarts de bus du W65C02 (non corrigés) : le cœur relit l'octet d'opérande là où
les tests attendent une lecture de l'adresse de base (page zéro indexée, `(zp,X)`,
branchements avec changement de page…), et `BRK` lit l'octet qui suit la
signature. À corriger avant que la version PC d'un système à entrées-sorties
sensibles aux lectures (Apple II, commutateurs `$C0xx`) n'utilise ce cœur.

Le Telestrat (`~/Neo6502TeleStrat`, copie de `w65c02cpu.h`) a repris ces
corrections en v0.16.7 (0 échec fonctionnel hors `$5C`).

**Interface à garder stable** : la cible `make cpu_harte` du Telestrat compile
`tools/cputest/harte.c` (avec son propre `-Isrc` en premier) et s'appuie sur
l'option `-x`, les codes de sortie (0, 1, 77) et le choix du cœur par
`-DUSE_65C02`.
