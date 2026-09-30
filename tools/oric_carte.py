#!/usr/bin/env python3
"""Banc d'essai de l'Oric sur la carte Neo6502 par sonde SWD (Debugprobe),
firmware oric_diag (platforms/rp2040/build/systems/oric/oric_diag.elf).

  oric_carte.py etat              trames, panneau ouvert, clé USB, interface, lecteurs
  oric_carte.py taper "CLOAD\\"\\"\\n"   frappe (file diag_keyq) ; jetons {F1} {ESC} {ENTREE}
                                  {HAUT} {BAS} {GAUCHE} {DROITE} {SUPPR} {DEBUT} {FIN}
  oric_carte.py panneau           texte du panneau de contrôle (surface osd, Latin-1)
  oric_carte.py ecran             écran texte de l'Oric ($BB80, 40 x 28)

Variables : NEO_ELF (oric_diag.elf), NEO_OPENOCD (OpenOCD qui connaît la flash
Puya P25Q16 du Neo6502).
"""
import os
import re
import subprocess
import sys
import tempfile

ELF = os.environ.get("NEO_ELF", os.path.expanduser(
    "~/reload-emulator/platforms/rp2040/build/systems/oric/oric_diag.elf"))
OPENOCD = os.environ.get("NEO_OPENOCD", os.path.expanduser("~/.local/openocd-dev/bin/openocd"))

# Grille du panneau sur le Neo6502 (800 x 480) : src/osd/osd.h avec OSD_COLS / OSD_ROWS de oric.c
OSD_COLS, OSD_ROWS = 100, 30

# Codes de hid_app.c : ASCII, ou usage HID | 0x100
SPECIALES = {"F1": 0x13A, "ESC": 0x1B, "ENTREE": 0x0D, "HAUT": 0x152, "BAS": 0x151, "GAUCHE": 0x150,
             "DROITE": 0x14F, "SUPPR": 0x7F, "DEBUT": 0x14A, "FIN": 0x14D, "PGPREC": 0x14B, "PGSUIV": 0x14E}


def symboles():
    sortie = subprocess.run(["arm-none-eabi-nm", "-S", ELF], capture_output=True, text=True, check=True).stdout
    table = {}
    for ligne in sortie.splitlines():
        champs = ligne.split()
        if len(champs) >= 3:
            table[champs[-1]] = int(champs[0], 16)
    return table


def openocd(*commandes, timeout=120):
    args = [OPENOCD, "-f", "interface/cmsis-dap.cfg", "-f", "target/rp2040.cfg", "-c", "adapter speed 2000", "-c", "init"]
    for c in commandes:
        args += ["-c", c]
    args += ["-c", "shutdown"]
    r = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
    journal = r.stdout + r.stderr
    if "Error" in journal:
        raise SystemExit("OpenOCD : " + "\n".join(l for l in journal.splitlines() if "Error" in l))
    return journal


def lire(adresse, n):
    """n octets à partir de adresse (RAM)."""
    with tempfile.TemporaryDirectory() as d:
        f = os.path.join(d, "m.bin")
        openocd(f"dump_image {f} 0x{adresse:08x} {n}")
        return open(f, "rb").read()


def mot(b, i):
    return int.from_bytes(b[i:i + 4], "little")


def taper(texte):
    s = symboles()
    codes = []
    for jeton in re.findall(r"\{[A-Z0-9]+\}|.", texte.replace("\\n", "\n"), re.S):
        if jeton.startswith("{") and jeton[1:-1] in SPECIALES:
            codes.append(SPECIALES[jeton[1:-1]])
        elif jeton == "\n":
            codes.append(0x0D)
        else:
            # hid_app.c donne les minuscules sans SHIFT ; oric.c inverse la casse
            codes.append(ord(jeton.lower()) if jeton.isalpha() else ord(jeton))
    tail = mot(lire(s["diag_keyq_tail"], 4), 0)
    cmds = []
    for i, c in enumerate(codes):
        cmds.append(f"mwh 0x{s['diag_keyq'] + 2 * ((tail + i) & 255):08x} 0x{c:04x}")
    cmds.append(f"mww 0x{s['diag_keyq_tail']:08x} {tail + len(codes)}")
    openocd(*cmds)


def panneau():
    s = symboles()
    # Double tampon : osd_surfaces[osd_front] est la surface affichée (ch, puis attr, puis big)
    front = lire(s["osd_front"], 1)[0]
    b = lire(s["osd_surfaces"] + front * 3 * OSD_COLS * OSD_ROWS, OSD_COLS * OSD_ROWS)
    for r in range(OSD_ROWS):
        print(b[r * OSD_COLS:(r + 1) * OSD_COLS].decode("latin-1").rstrip())


def ecran():
    s = symboles()
    # Décalage de ram dans state : diag_layout[0] (layout exporté par le firmware)
    off = mot(lire(s["diag_layout"], 4), 0)
    b = lire(s["state"] + off + 0xBB80, 40 * 28)
    for r in range(28):
        ligne = "".join(chr(c & 0x7F) if 32 <= (c & 0x7F) < 127 else " " for c in b[r * 40:(r + 1) * 40])
        print(ligne.rstrip())


def etat():
    s = symboles()
    frames = mot(lire(s["diag_frames"], 4), 0)
    print("trames", frames)
    print("panneau ouvert", lire(s["panel_open"], 1)[0])
    print("clé USB (msc_inquiry_complete)", lire(s["msc_inquiry_complete"], 1)[0], "scannée", lire(s["usb_scanned"], 1)[0])


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    cmd = sys.argv[1]
    if cmd == "etat":
        etat()
    elif cmd == "taper":
        taper(sys.argv[2])
    elif cmd == "panneau":
        panneau()
    elif cmd == "ecran":
        ecran()
    else:
        print(__doc__)
        sys.exit(1)
