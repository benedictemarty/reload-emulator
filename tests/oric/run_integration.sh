#!/bin/sh
# run_integration.sh — Oric integration tests with the real ROMs and images
#
#   tests/oric/run_integration.sh path/to/oric_headless
#
# The ROMs (src/roms/oric_roms.h, oric_microdisc_rom.h, oric_jasmin_rom.h) are
# compiled into oric_headless; the disk and tape images are not part of the
# repository: set their paths with the variables below (defaults: bmarty's
# machine). A test whose image (or ROM) is missing is reported as SKIP; the
# script exits 77 (skipped) if every test was skipped, 1 if one failed.
#
#   ORIC_SEDORIC_DSK   Sedoric 4.0 disk (Microdisc)       ~/legacy/disk/test.dsk
#   ORIC_GAME_DSK      Sedoric game disk (3D Fongus)      ~/Téléchargements/3dfongus.dsk
#   ORIC_NOOS_DSK      disk without an OS (Microdisc)     ~/Téléchargements/arcade1j.dsk
#   ORIC_FTDOS_DSK     FT-DOS disk (Jasmin)               ~/oriclib/games/dsk/FTDOS.dsk
#   ORIC_BASIC_TAP     BASIC program (cc65 stub)          ~/oric-bench/sieve_cc65.tap
#   ORIC_HIRES_TAP     8000 bytes at $A000, 1 byte short  ~/pichires/spell.tap

HEADLESS=${1:?usage: run_integration.sh path/to/oric_headless}
SEDORIC_DSK=${ORIC_SEDORIC_DSK:-$HOME/legacy/disk/test.dsk}
GAME_DSK=${ORIC_GAME_DSK:-$HOME/Téléchargements/3dfongus.dsk}
NOOS_DSK=${ORIC_NOOS_DSK:-$HOME/Téléchargements/arcade1j.dsk}
FTDOS_DSK=${ORIC_FTDOS_DSK:-$HOME/oriclib/games/dsk/FTDOS.dsk}
BASIC_TAP=${ORIC_BASIC_TAP:-$HOME/oric-bench/sieve_cc65.tap}
HIRES_TAP=${ORIC_HIRES_TAP:-$HOME/pichires/spell.tap}

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
pass=0; fail=0; skip=0

# check NAME EXPECTED_TEXT [needs FILE] -- oric_headless arguments
check() {
    name=$1; expected=$2; need=$3; shift 3
    if [ -n "$need" ] && [ ! -f "$need" ]; then
        echo "SKIP $name (missing $need)"; skip=$((skip + 1)); return
    fi
    "$HEADLESS" "$@" > "$TMP/out.txt" 2>&1
    rc=$?
    if [ $rc -eq 3 ]; then
        echo "SKIP $name (ROM missing)"; skip=$((skip + 1)); return
    fi
    if grep -q -- "$expected" "$TMP/out.txt"; then
        echo "PASS $name"; pass=$((pass + 1))
    else
        echo "FAIL $name (expected: $expected)"; sed 's/^/    /' "$TMP/out.txt" | grep -v '^ *$' | head -30
        fail=$((fail + 1))
    fi
}

check "BASIC 1.1 boot" "37631 BYTES FREE" "" -f 150 -s
check "BASIC typing" " 12" "" -f 400 -w 150 -t 'PRINT 12\n' -s
check "printer (LPRINT)" "printer: 4 byte(s): AB.." "" -f 400 -w 150 -t 'LPRINT "AB"\n' -P
check "tape: no motion at boot" "tape_pos=0 " "$BASIC_TAP" -T "$BASIC_TAP" -f 150 -i
check "tape: CLOAD BASIC" "531 CALL#50D" "$BASIC_TAP" -T "$BASIC_TAP" -f 3500 -w 150 -t 'CLOAD""\n~~~~~~~~~~~~~~~~~~~~~~~~LIST\n' -s
check "tape: CLOAD 8000 bytes, padded" "tape_pos=8014 " "$HIRES_TAP" -T "$HIRES_TAP" -f 5500 -w 150 -t 'CLOAD""\n' -i
check "Microdisc: Sedoric 4.0 boot + DIR" "1414 free sectors" "$SEDORIC_DSK" -c microdisc -0 "$SEDORIC_DSK" -f 1300 -w 900 -t ' DIR\n' -s
check "Microdisc: auto-detected interface" "fdc=microdisc" "$SEDORIC_DSK" -c auto -0 "$SEDORIC_DSK" -f 10 -i
check "Microdisc: disk without OS -> BASIC" "No operating system on disc" "$NOOS_DSK" -c microdisc -0 "$NOOS_DSK" -f 300 -s
# Framebuffer hash of the title screen, checked visually on 2026-09-30 (screen of the Loriciels game)
check "Microdisc: 3D Fongus title screen" "fb_hash=535B847E" "$GAME_DSK" -c microdisc -0 "$GAME_DSK" -f 1500 -H
check "Jasmin: FT-DOS boot (auto-detected)" "fdc=jasmin" "$FTDOS_DSK" -c auto -0 "$FTDOS_DSK" -f 1000 -i
check "Jasmin: BASIC usable after the boot" " 12" "$FTDOS_DSK" -c auto -0 "$FTDOS_DSK" -f 1000 -w 700 -t 'PRINT 12\n' -s

echo "integration: $pass passed, $fail failed, $skip skipped"
[ $fail -gt 0 ] && exit 1
[ $pass -eq 0 ] && exit 77
exit 0
