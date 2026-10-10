#!/bin/sh
# PicoDVI : nombre de lignes de sortie par tampon TMDS choisi par l'application.
# Les trois tests « v_ctr % DVI_VERTICAL_REPEAT == DVI_VERTICAL_REPEAT - 1 » de
# libdvi/dvi.c (dernière ligne de sortie d'un tampon) passent par la macro
# DVI_LAST_REPEAT(v), qui vaut ce même test par défaut (aucun effet pour les autres
# machines) ; une application la redéfinit dans un fichier dvi_line_repeat.h de son
# chemin d'inclusion (BBC à 800x480 : 256 lignes en 480, certaines affichées une fois).
# Idempotent. $1 = dossier source de PicoDVI.
set -e
F="$1/software/libdvi/dvi.c"
[ -f "$F" ] || { echo "PicoDVI : $F absent" >&2; exit 1; }
if grep -q "DVI_LAST_REPEAT" "$F"; then
    echo "PicoDVI : correctif DVI_LAST_REPEAT déjà appliqué"
    exit 0
fi
N=$(grep -c 'inst->timing_state.v_ctr % DVI_VERTICAL_REPEAT == DVI_VERTICAL_REPEAT - 1' "$F")
[ "$N" = 3 ] || { echo "PicoDVI : 3 tests de répétition attendus, $N trouvés" >&2; exit 1; }
sed -i 's/inst->timing_state.v_ctr % DVI_VERTICAL_REPEAT == DVI_VERTICAL_REPEAT - 1/DVI_LAST_REPEAT(inst->timing_state.v_ctr)/' "$F"
sed -i '0,/^#include "dvi_timing.h"/s//#include "dvi_timing.h"\n\n\/\/ Dernière ligne de sortie du tampon TMDS courant (patches\/picodvi-line-repeat.sh de reload)\n#if __has_include("dvi_line_repeat.h")\n#include "dvi_line_repeat.h"\n#endif\n#ifndef DVI_LAST_REPEAT\n#define DVI_LAST_REPEAT(v) ((v) % DVI_VERTICAL_REPEAT == DVI_VERTICAL_REPEAT - 1)\n#endif/' "$F"
[ "$(grep -c 'DVI_LAST_REPEAT(inst->timing_state.v_ctr)' "$F")" = 3 ] && grep -q '#define DVI_LAST_REPEAT(v)' "$F" \
    || { echo "PicoDVI : échec du correctif DVI_LAST_REPEAT" >&2; exit 1; }
echo "PicoDVI : correctif DVI_LAST_REPEAT appliqué"
