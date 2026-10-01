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
#   ORIC_TNFSD         TNFS server (reference tnfsd: git clone
#                      https://github.com/FujiNetWIFI/spectranet ; make -C tnfs/tnfsd OS=LINUX)
#                      ~/spectranet/tnfs/tnfsd/bin/tnfsd ; it listens on UDP port 16384

HEADLESS=${1:?usage: run_integration.sh path/to/oric_headless}
SEDORIC_DSK=${ORIC_SEDORIC_DSK:-$HOME/legacy/disk/test.dsk}
GAME_DSK=${ORIC_GAME_DSK:-$HOME/Téléchargements/3dfongus.dsk}
NOOS_DSK=${ORIC_NOOS_DSK:-$HOME/Téléchargements/arcade1j.dsk}
FTDOS_DSK=${ORIC_FTDOS_DSK:-$HOME/oriclib/games/dsk/FTDOS.dsk}
BASIC_TAP=${ORIC_BASIC_TAP:-$HOME/oric-bench/sieve_cc65.tap}
HIRES_TAP=${ORIC_HIRES_TAP:-$HOME/pichires/spell.tap}
TNFSD=${ORIC_TNFSD:-$HOME/spectranet/tnfs/tnfsd/bin/tnfsd}
# Wi-Fi modem simulated on the PC (ESP8266 AT: the twin of Neo6502picowifi), built next to oric_headless
NETBRIDGE=${ORIC_NETBRIDGE:-$(dirname "$HEADLESS")/../../netbridge/neo_esp_modem_pc}

TMP=$(mktemp -d)
TNFSD_PID=
BRIDGE_PID=
trap '[ -n "$BRIDGE_PID" ] && kill "$BRIDGE_PID" 2>/dev/null; [ -n "$TNFSD_PID" ] && kill "$TNFSD_PID" 2>/dev/null; rm -rf "$TMP"' EXIT
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
check "tape: CLOAD fast (patched BASIC 1.1)" "531 CALL#50D" "$BASIC_TAP" -T "$BASIC_TAP" -F -f 400 -w 150 -t 'CLOAD""\n~LIST\n' -s
# CSAVE to a .tap file, then CLOAD of that file
"$HEADLESS" -f 1600 -w 150 -R "$TMP" -t '10 PRINT 4242\nCSAVE"RLTEST"\n' > /dev/null 2>&1
check "tape: CSAVE then CLOAD" "10 PRINT 4242" "$TMP/RLTEST.TAP" -T "$TMP/RLTEST.TAP" -f 3500 -w 150 -t 'CLOAD""\n~~~~~~~~~~~~~~~~~~~~~~~~LIST\n' -s
# 8014 bytes, one short of its header: the padding $00 is sent too (position 8015)
check "tape: CLOAD 8000 bytes, padded" "tape_pos=8015 " "$HIRES_TAP" -T "$HIRES_TAP" -f 5500 -w 150 -t 'CLOAD""\n' -i
check "Microdisc: Sedoric 4.0 boot + DIR" "1414 free sectors" "$SEDORIC_DSK" -c microdisc -0 "$SEDORIC_DSK" -f 1300 -w 900 -t ' DIR\n' -s
check "Microdisc: auto-detected interface" "fdc=microdisc" "$SEDORIC_DSK" -c auto -0 "$SEDORIC_DSK" -f 10 -i
check "Microdisc: disk without OS -> BASIC" "No operating system on disc" "$NOOS_DSK" -c microdisc -0 "$NOOS_DSK" -f 300 -s
# Framebuffer hash of the title screen, checked visually on 2026-09-30 (screen of the Loriciels game)
check "Microdisc: 3D Fongus title screen" "fb_hash=535B847E" "$GAME_DSK" -c microdisc -0 "$GAME_DSK" -f 1500 -H
check "Jasmin: FT-DOS boot (auto-detected)" "fdc=jasmin" "$FTDOS_DSK" -c auto -0 "$FTDOS_DSK" -f 1000 -i
check "Jasmin: BASIC usable after the boot" " 12" "$FTDOS_DSK" -c auto -0 "$FTDOS_DSK" -f 1000 -w 700 -t 'PRINT 12\n' -s

# Files over the network (TNFS, devices/neo_tnfs.h): a reference tnfsd serving a
# temporary directory; tape and disk read as streams, then a Sedoric SAVE read back
if [ ! -x "$TNFSD" ]; then
    echo "SKIP TNFS tests (missing $TNFSD)"; skip=$((skip + 5))
elif ss -lun 2>/dev/null | grep -q ':16384 '; then
    echo "SKIP TNFS tests (UDP port 16384 already in use)"; skip=$((skip + 5))
else
    mkdir -p "$TMP/tnfs"
    [ -f "$BASIC_TAP" ] && cp "$BASIC_TAP" "$TMP/tnfs/SIEVE.TAP"
    [ -f "$SEDORIC_DSK" ] && cp "$SEDORIC_DSK" "$TMP/tnfs/SED.DSK"
    "$TNFSD" "$TMP/tnfs" > "$TMP/tnfsd.log" 2>&1 &
    TNFSD_PID=$!
    sleep 1
    check "TNFS: CLOAD over the network" "531 CALL#50D" "$TMP/tnfs/SIEVE.TAP" -N 127.0.0.1 -T /SIEVE.TAP -f 3500 -w 150 -t 'CLOAD""\n~~~~~~~~~~~~~~~~~~~~~~~~LIST\n' -s
    check "TNFS: Sedoric streamed, DIR" "1414 free sectors" "$TMP/tnfs/SED.DSK" -N 127.0.0.1 -c microdisc -0 /SED.DSK -f 1300 -w 900 -t ' DIR\n' -s
    if [ -f "$TMP/tnfs/SED.DSK" ]; then
        "$HEADLESS" -N 127.0.0.1 -c microdisc -0 /SED.DSK -f 1800 -w 900 -t ' 10 PRINT 4242\nSAVE"RLNET"\n' > /dev/null 2>&1
    fi
    check "TNFS: Sedoric SAVE, then LOAD in a new session" "10 PRINT 4242" "$TMP/tnfs/SED.DSK" -N 127.0.0.1 -c microdisc -0 /SED.DSK -f 1600 -w 900 -t ' LOAD"RLNET"\nLIST\n' -s
    # Through a Wi-Fi modem on a serial port (devices/neo_esp_at.h), as with Neo6502picowifi
    if [ -x "$NETBRIDGE" ]; then
        "$NETBRIDGE" -p "$TMP/pty.txt" > /dev/null 2> "$TMP/bridge.log" &
        BRIDGE_PID=$!
        sleep 1
        PTY=$(cat "$TMP/pty.txt" 2>/dev/null)
        check "Modem: CLOAD through the AT Wi-Fi modem" "531 CALL#50D" "$TMP/tnfs/SIEVE.TAP" -S "$PTY" -N 127.0.0.1 -T /SIEVE.TAP -f 3500 -w 150 -t 'CLOAD""\n~~~~~~~~~~~~~~~~~~~~~~~~LIST\n' -s
        check "Modem: Sedoric streamed through the modem (sees the SAVE)" "RLNET    .COM" "$TMP/tnfs/SED.DSK" -S "$PTY" -N 127.0.0.1 -c microdisc -0 /SED.DSK -f 1300 -w 900 -t ' DIR\n' -s
        kill "$BRIDGE_PID" 2>/dev/null; BRIDGE_PID=
    else
        echo "SKIP modem tests (missing $NETBRIDGE)"; skip=$((skip + 2))
    fi
    kill "$TNFSD_PID" 2>/dev/null; TNFSD_PID=
fi

echo "integration: $pass passed, $fail failed, $skip skipped"
[ $fail -gt 0 ] && exit 1
[ $pass -eq 0 ] && exit 77
exit 0
