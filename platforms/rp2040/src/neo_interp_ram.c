// neo_interp_ram.c — interp_save / interp_restore du SDK en RAM
//
// L'encodeur 16 bpp de PicoDVI (tmds_encode_data_channel_16bpp, sur le cœur 1, à
// chaque ligne) appelle interp_save et interp_restore de hardware_interp, que le
// SDK place en flash : pendant que le cœur 0 exécute du code en flash (montage
// USB, FatFs), le cœur 1 attend le cache XIP et la ligne arrive en retard (trait
// rouge). Mesuré sur la carte par Neo6502nes v0.5.4 : 20-22 lignes manquées au
// démarrage, pire ligne 113 µs, puis 0 ligne et 39 µs avec ces copies en RAM.
// Trouvé dans le binaire par Neo6502Trinity (tools/core1_flash.py le détecte).
//
// À ajouter aux sources d'un firmware qui utilise l'encodeur 16 bpp, avec :
//   target_link_options(cible PRIVATE -Wl,--wrap=interp_save -Wl,--wrap=interp_restore)
// Même code que hardware_interp/interp.c du SDK.
//
// Copyright (c) 2026 bmarty — licence zlib/libpng (voir src/osd/osd.h)

#include "pico.h"
#include "hardware/interp.h"

void __not_in_flash_func(__wrap_interp_save)(interp_hw_t *interp, interp_hw_save_t *saver) {
    saver->accum[0] = interp->accum[0];
    saver->accum[1] = interp->accum[1];
    saver->base[0] = interp->base[0];
    saver->base[1] = interp->base[1];
    saver->base[2] = interp->base[2];
    saver->ctrl[0] = interp->ctrl[0];
    saver->ctrl[1] = interp->ctrl[1];
}

void __not_in_flash_func(__wrap_interp_restore)(interp_hw_t *interp, interp_hw_save_t *saver) {
    interp->accum[0] = saver->accum[0];
    interp->accum[1] = saver->accum[1];
    interp->base[0] = saver->base[0];
    interp->base[1] = saver->base[1];
    interp->base[2] = saver->base[2];
    interp->ctrl[0] = saver->ctrl[0];
    interp->ctrl[1] = saver->ctrl[1];
}
