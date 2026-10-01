#!/bin/sh
# Compile the projects that build against this socle (RELOAD_DIR), in temporary
# directories, before committing a change to platforms/rp2040/src or src/
# (docs/STRATEGIE-NEO6502.md, section 5), and print the RAM left beyond the
# reserved heap of each firmware (a socle change that adds RAM can break a
# consumer's link: Telestrat RAM 64 KB had 376 bytes on 2026-09-30).
# A project that is absent is skipped.
#   tools/socle_consommateurs.sh
set -u
RELOAD="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
status=0
# project|cmake options|name of the build
for v in "Neo6502nes||nes" "Neo6502TeleStrat||telestrat" "Neo6502TeleStrat|-DTELESTRAT_RAM64K=ON|telestrat_ram64k" "Neo6502Vic20||vic20" "Neo6502Atari2600/platforms/rp2040/neo||atari2600"; do
    p="${v%%|*}"
    rest="${v#*|}"
    opts="${rest%%|*}"
    name="${rest#*|}"
    # A project, or a path to its CMake root when it is not platforms/rp2040
    case "$p" in
        */*) src="$HOME/$p" ;;
        *) src="$HOME/$p/platforms/rp2040" ;;
    esac
    if [ ! -d "$src" ]; then
        echo "SKIP  $name : $src absent"
        continue
    fi
    # shellcheck disable=SC2086
    if cmake -S "$src" -B "$TMP/$name" -DRELOAD_DIR="$RELOAD" $opts >"$TMP/$name.log" 2>&1 &&
       cmake --build "$TMP/$name" -j8 >>"$TMP/$name.log" 2>&1; then
        margins=""
        for map in $(find "$TMP/$name" -name '*.elf.map'); do
            lim=$(grep -m1 "__HeapLimit = " "$map" | awk '{print $1}')
            [ -n "$lim" ] && margins="$margins $(basename "$map" .elf.map)=$(( 0x20040000 - lim )) o"
        done
        echo "OK    $name : RAM au-delà du tas :$margins"
        # Core 1 path: functions in flash reached (tools/core1_flash.py; red lines)
        for elf in $(find "$TMP/$name" -maxdepth 3 -name '*.elf' ! -name '*bench*'); do
            echo "      cœur 1 de $(basename "$elf") : $(python3 "$RELOAD/tools/core1_flash.py" --court "$elf" 2>&1)"
        done
    else
        echo "ÉCHEC $name :"
        grep -E "error|Error|overflowed" "$TMP/$name.log" | head -5
        status=1
    fi
done
exit $status
