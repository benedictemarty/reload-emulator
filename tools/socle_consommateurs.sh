#!/bin/sh
# Compile the projects that build against this socle (RELOAD_DIR), in temporary
# directories, before committing a change to platforms/rp2040/src or src/
# (docs/STRATEGIE-NEO6502.md, section 5). A project that is absent is skipped.
#   tools/socle_consommateurs.sh
set -u
RELOAD="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
status=0
for p in Neo6502nes Neo6502TeleStrat; do
    src="$HOME/$p/platforms/rp2040"
    if [ ! -d "$src" ]; then
        echo "SKIP $p : $src absent"
        continue
    fi
    if cmake -S "$src" -B "$TMP/$p" -DRELOAD_DIR="$RELOAD" >"$TMP/$p.log" 2>&1 &&
       cmake --build "$TMP/$p" -j8 >>"$TMP/$p.log" 2>&1; then
        echo "OK   $p"
    else
        echo "ÉCHEC $p :"
        grep -E "error|Error" "$TMP/$p.log" | head -5
        status=1
    fi
done
exit $status
