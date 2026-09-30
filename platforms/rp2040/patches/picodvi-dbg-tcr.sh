#!/bin/sh
# PicoDVI + pico-sdk 2.x : dma_debug_channel_hw_t.tcr a été renommé dbg_tcr dans le SDK 2.0.
# Remplace l'accès de libdvi/dvi.c par une macro choisie selon PICO_SDK_VERSION_MAJOR
# (tcr en 1.x, dbg_tcr en 2.x) : sans effet avec le SDK 1.5.1. Idempotent.
# $1 = dossier source de PicoDVI.
set -e
F="$1/software/libdvi/dvi.c"
[ -f "$F" ] || { echo "PicoDVI : $F absent" >&2; exit 1; }
if grep -q "DVI_DMA_DBG_TCR" "$F"; then
    echo "PicoDVI : correctif tcr/dbg_tcr déjà appliqué"
    exit 0
fi
grep -q '\]\.tcr != ' "$F" || { echo "PicoDVI : accès .tcr introuvable (déjà dbg_tcr ?), rien à faire"; exit 0; }
sed -i 's/\]\.tcr != /].DVI_DMA_DBG_TCR != /' "$F"
sed -i '0,/^#include "dvi.h"/s//#include "dvi.h"\n\n\/\/ pico-sdk 2.x : dma_debug_channel_hw_t.tcr renommé dbg_tcr\n#if PICO_SDK_VERSION_MAJOR >= 2\n#define DVI_DMA_DBG_TCR dbg_tcr\n#else\n#define DVI_DMA_DBG_TCR tcr\n#endif/' "$F"
grep -q "#define DVI_DMA_DBG_TCR dbg_tcr" "$F" || { echo "PicoDVI : échec du correctif tcr/dbg_tcr" >&2; exit 1; }
echo "PicoDVI : correctif tcr/dbg_tcr appliqué"
