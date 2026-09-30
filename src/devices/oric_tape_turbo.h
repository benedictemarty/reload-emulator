#pragma once

// oric_tape_turbo.h — chargement accéléré des cassettes (ROM BASIC 1.1)
//
// Deux routines de la ROM ORIC EXTENDED BASIC V1.1 (mode Atmos) sont
// remplacées dans la copie en RAM de la banque, l'option active :
//   $E6C9 (lire un octet : A et $2F, X et Y préservés, C = 0 sans erreur)
//     -> LDA $03FE / STA $2F / CLC / RTS
//   $E735 (chercher la synchro : trois $16, X = 0 au retour)
//     -> LDX #0 / LDA $03FF / BMI (attente) / RTS
// Le système répond en $03FE (octet suivant de la bande) et en $03FF (0 :
// bande placée après une synchro ; $80 : pas de synchro, on attend comme la
// ROM qui cherche). Adresses relevées sur la ROM désassemblée (da65, md5
// a330779c42ad7d0c4ac6ef9e92788ec6) ; les octets d'origine servent de
// signature : une autre ROM n'est pas touchée, l'option coupée les remet.
// BASIC 1.0 (banque 5 de STRATORIC) : routines ailleurs, non accéléré.
// Les chargeurs propres aux jeux (qui lisent CB1 eux-mêmes) gardent la
// vitesse normale.
//
// Composant commun à reload-emulator (Oric) et Neo6502TeleStrat, repris du Telestrat
// (fusion : docs/FUSION-ORIC-TELESTRAT.md de reload).
//
// ## Licence zlib/libpng
//
// Copyright (c) 2026 bmarty
// This software is provided 'as-is', without any express or implied warranty.
// In no event will the authors be held liable for any damages arising from the
// use of this software.
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
//     1. The origin of this software must not be misrepresented; you must not
//     claim that you wrote the original software. If you use this software in a
//     product, an acknowledgment in the product documentation would be
//     appreciated but is not required.
//     2. Altered source versions must be plainly marked as such, and must not
//     be misrepresented as being the original software.
//     3. This notice may not be removed or altered from any source
//     distribution.

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define ORIC_TURBO_BYTE_REG 0xFE  // $03FE
#define ORIC_TURBO_SYNC_REG 0xFF  // $03FF

typedef struct {
    uint16_t offset;  // dans la banque de 16 Ko ($C000 = 0)
    uint8_t len;
    uint8_t orig[8];
    uint8_t patch[8];
} _oric_turbo_patch_t;

static const _oric_turbo_patch_t _oric_turbo_patches[2] = {
    {0x26C9, 7, {0x98, 0x48, 0x8A, 0x48, 0x20, 0x1C, 0xE7}, {0xAD, 0xFE, 0x03, 0x85, 0x2F, 0x18, 0x60}},
    {0x2735, 8, {0x20, 0xFC, 0xE6, 0x66, 0x2F, 0xA9, 0x16, 0xC5}, {0xA2, 0x00, 0xAD, 0xFF, 0x03, 0x30, 0xFB, 0x60}},
};

// Banque de 16 Ko : 1 si c'est BASIC 1.1 d'origine, 2 si déjà patchée, 0 sinon
static inline int oric_turbo_state(const uint8_t* rom) {
    bool orig = true, patched = true;
    for (int i = 0; i < 2; i++) {
        const _oric_turbo_patch_t* p = &_oric_turbo_patches[i];
        orig = orig && !memcmp(rom + p->offset, p->orig, p->len);
        patched = patched && !memcmp(rom + p->offset, p->patch, p->len);
    }
    return orig ? 1 : patched ? 2 : 0;
}

// Option appliquée à une banque modifiable ; true si c'est BASIC 1.1
static inline bool oric_turbo_apply(uint8_t* rom, bool on) {
    const int st = oric_turbo_state(rom);
    if (!st) return false;
    for (int i = 0; i < 2; i++) {
        const _oric_turbo_patch_t* p = &_oric_turbo_patches[i];
        memcpy(rom + p->offset, on ? p->patch : p->orig, p->len);
    }
    return true;
}

// Option appliquée à n banques de 16 Ko (emplacements de ROM en RAM) ;
// retourne le nombre de BASIC 1.1 trouvés
static inline int oric_turbo_apply_all(uint8_t (*roms)[0x4000], int n, bool on) {
    int k = 0;
    for (int i = 0; i < n; i++) k += oric_turbo_apply(roms[i], on);
    return k;
}

// Messages du menu
static inline const char* oric_turbo_message(bool on, int found) {
    if (!on) return "Cassette : vitesse réelle";
    return found ? "Cassette rapide : CLOAD du BASIC 1.1 immédiat" : "Cassette rapide (aucun BASIC 1.1 en banque pour l'instant)";
}
