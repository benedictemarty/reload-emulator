#!/usr/bin/env python3
"""Tests d'intégration du BBC Micro (version PC sans fenêtre, bbc_headless).

Lancés par CTest (test bbc_integration, platforms/pc/build) avec les chemins des
binaires dans BBC_HEADLESS et BBC_HEADLESS_LUT. ROMs : src/roms/bbc_roms.h (OS 1.20,
BASIC 2, DFS 2.26), non versionnées ; Master 128 et Tube facultatifs ; images de
jeux hors dépôt (SPELLBINDER_SSD, BATTLEZONE_SSD), tests sautés si absentes.
Écran et RAM de référence comparés à b2 (docs/BBC.md). Repris de ~/Neo6502bbc
(tests/test_bbc_pc.py) le 2026-09-30.

Usage : tests/bbc/test_bbc_pc.py [--update-golden]
Code de sortie : 0 tout passe, 1 échec, 77 binaire ou Pillow absent (sauté).
"""
import os, sys, subprocess, tempfile, hashlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
GOLDEN = HERE / "golden"
ROOT = HERE.parent.parent
BIN = Path(os.environ.get("BBC_HEADLESS", ROOT / "platforms/pc/build/systems/bbc/bbc_headless"))

def run(frames, text=None, ppm=None):
    cmd = [str(BIN), "-f", str(frames), "-s"]
    if text is not None:
        cmd += ["-t", text]
    if ppm:
        cmd += ["-p", str(ppm)]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    assert r.returncode == 0, r.stderr
    lines = r.stdout.splitlines()
    return lines[:25], lines[25]

def ppm_to_png(ppm, png):
    from PIL import Image
    Image.open(ppm).save(png)

def png_bytes(path):
    from PIL import Image
    return Image.open(path).convert("RGB").tobytes()

def check_screen(name, frames, text, update):
    with tempfile.TemporaryDirectory() as d:
        ppm = Path(d) / "out.ppm"
        screen, status = run(frames, text, ppm)
        png = GOLDEN / f"{name}.png"
        if update or not png.exists():
            ppm_to_png(ppm, png)
            print(f"  golden écrit : {png.name}")
            return screen, status
        from PIL import Image
        got = Image.open(ppm).convert("RGB").tobytes()
        assert got == png_bytes(png), f"{name}: écran différent du golden {png.name}"
        return screen, status

def main():
    update = "--update-golden" in sys.argv
    if not BIN.exists():
        print(f"SKIP : {BIN} absent"); return 77
    try:
        import PIL  # noqa: F401  (captures d'écran comparées aux références)
    except ImportError:
        print("SKIP : Pillow absent (pip install pillow)"); return 77
    failures = 0
    tests = []

    # 1. Boot : MOS 1.20 + BASIC 2, invite en MODE 7 (US-32)
    screen, status = run(100)
    tests.append(("boot MODE 7", screen[1].startswith("BBC Computer 32K") and screen[3].startswith("Acorn 1770 DFS")
                  and screen[5].startswith("BASIC") and screen[7].startswith(">") and "ula=4A" in status))

    # 2. Clavier : PRINT 1+1 -> 2 (US-39)
    screen, _ = run(200, "PRINT 1+1\n")
    tests.append(("PRINT 1+1", screen[7].startswith(">PRINT 1+1") and screen[8].strip() == "2"))

    # 3. Captures d'écran comparées aux goldens (rendu ULA/CRTC)
    for name, frames, text in [
        ("mode7_boot", 100, None),
        ("mode0_line", 400, 'MODE 0\nMOVE 0,0:DRAW 1279,1023:PRINT "MODE 0 - 80 COLUMNS"\n'),
        ("mode2_bars", 400, '*FX9,0\nMODE 2\nFOR I=0 TO 15:GCOL 0,I:MOVE I*80,0:DRAW I*80,1023:NEXT\n'),  # *FX9,0 : clignotement figé
        ("mode5_text", 300, 'MODE 5\nCOLOUR 1:PRINT "MODE 5":COLOUR 2:PRINT "ABC"\n'),
        ("mode7_colours", 300, 'PRINT CHR$(129);"ROUGE";CHR$(130);"VERT";CHR$(145);"\x01pjw\x01"\n'),  # ^A = CAPS LOCK
    ]:
        try:
            check_screen(name, frames, text, update)
            tests.append((f"golden {name}", True))
        except AssertionError as e:
            print(" ", e); tests.append((f"golden {name}", False))

    # 4. Son : SOUND 1,-15,89,100 -> le MOS programme N=278 -> 125000/278 = 449,6 Hz
    with tempfile.TemporaryDirectory() as d:
        wav = Path(d) / "a.wav"
        subprocess.run([str(BIN), "-f", "250", "-w", "60", "-t", "SOUND 1,-15,89,100\n", "-a", str(wav)],
                       check=True, capture_output=True, timeout=120)
        data = wav.read_bytes()[44:]
        seg = [b - 128 for b in data[int(3.0 * 22050):int(3.5 * 22050)]]
        crossings = sum(1 for i in range(1, len(seg)) if seg[i - 1] <= 0 < seg[i])
        freq = crossings / 0.5
        amp = max(seg) if seg else 0
        ok = abs(freq - 449.6) < 4 and amp > 10
        if not ok:
            print(f"  son : {freq:.1f} Hz, amplitude {amp}")
        tests.append(("SOUND 449,6 Hz (SN76489)", ok))

    # 4b. Manette : canaux ADC et bouton feu (US-49)
    r = subprocess.run([str(BIN), "-f", "300", "-J", "65535,0,1", "-s", "-t", "PRINT ADVAL(1),ADVAL(2),ADVAL(0) AND 3\n"],
                       capture_output=True, text=True, timeout=300)
    tests.append(("manette ADVAL (65535, 0, feu)", any(l.split() == ["65535", "0", "1"] for l in r.stdout.splitlines())))

    # 4c. Raster intra-ligne (US-48) : palette réécrite en boucle ~ toutes les 48 cycles
    # -> plusieurs transitions clair/sombre sur une même ligne (impossible en rendu par ligne)
    prog = (HERE / "data/raster_bars.bas").read_text() + "\n"
    with tempfile.TemporaryDirectory() as d:
        ppm = Path(d) / "raster.ppm"
        subprocess.run([str(BIN), "-f", "2400", "-t", prog, "-p", str(ppm)], check=True, capture_output=True, timeout=600)
        from PIL import Image
        im = Image.open(ppm).convert("L")
        px = im.load()
        transitions = sum(1 for x in range(1, 640) if (px[x, 100] > 0) != (px[x - 1, 100] > 0))
        tests.append(("raster intra-ligne (transitions sur une ligne)", transitions >= 4))

    # 5. Écriture disque : *SAVE sur une image vierge, relue par une seconde session
    with tempfile.TemporaryDirectory() as d:
        img = Path(d) / "vide.ssd"
        # Catalogue DFS vide : secteurs 0-1 = titre vide, 0 fichier, 80 pistes (0x320 secteurs)
        cat = bytearray(512)
        cat[0x106] = 0x03; cat[0x107] = 0x20
        img.write_bytes(bytes(cat) + bytes(200 * 1024 - 512))
        out = Path(d) / "ecrit.ssd"
        subprocess.run([str(BIN), "-f", "500", "-0", str(img), "-W", str(out), "-t", '*SAVE "ESSAI" 1900+100\n'],
                       check=True, capture_output=True, timeout=300)
        r = subprocess.run([str(BIN), "-f", "400", "-0", str(out), "-s", "-t", "*INFO ESSAI\n"],
                           capture_output=True, text=True, timeout=300)
        tests.append(("écriture disque (*SAVE, -W, *INFO)", "$.ESSAI       001900 001900 000100" in r.stdout))

    # 6. Spellbinder (US-40) : disquette hors dépôt, test sauté si absente
    ssd = Path(os.environ.get("SPELLBINDER_SSD", Path.home() / "spillbender/Disc035-Spellbinder.ssd"))
    if ssd.exists():
        with tempfile.TemporaryDirectory() as d:
            ppm = Path(d) / "spb.ppm"
            cmd = [str(BIN), "-f", "3400", "-w", "600", "-0", str(ssd), "-b", "-s", "-p", str(ppm),
                   "-t", " ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~~~~~Z~Z~X~:~/~ ~Y~Y~~~~~~~~~~"]
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
            status = r.stdout.splitlines()[-1]
            # Zones statiques seulement (titre, panneau) : la pose du personnage dépend du timing
            raw = ppm.read_bytes()
            body = raw[raw.index(b"255\n") + 4:]
            row = 640 * 3
            static = body[0:20 * row] + body[140 * row:200 * row]
            digest = hashlib.md5(static).hexdigest()
            golden = GOLDEN / "spellbinder_salle1.md5"
            if update or not golden.exists():
                golden.write_text(digest + "\n")
                print(f"  golden écrit : {golden.name} = {digest}")
            ok = "ula=89" in status or "ula=88" in status   # MODE 4 (ULA 0x88, bit 0 = clignotement)
            ok = ok and digest == golden.read_text().strip()
            tests.append(("Spellbinder salle 1 (MODE 4)", ok))
    else:
        print(f"  SKIP Spellbinder : {ssd} absent")

    # 7. Master 128 (MOS 3.20) : boot, RTC, écran fantôme (roms/bbc_master_roms.h requis)
    r = subprocess.run([str(BIN), "-m", "-f", "500", "-s", "-t", "PRINT TIME$\n"],
                       capture_output=True, text=True, timeout=300)
    if "bbc_master_roms.h absent" in r.stderr:
        print("  SKIP Master 128 : ROMs MOS 3.20 absentes")
    else:
        lines = r.stdout.splitlines()
        ok = lines[1].startswith("Acorn MOS") and lines[3].startswith("Acorn 1770 DFS") and lines[5].startswith("BASIC")
        ok = ok and any(l.startswith("Tue,15 Sep 1926.12:00:0") for l in lines[:25])
        r2 = subprocess.run([str(BIN), "-m", "-f", "700", "-s", "-t", "MODE 135\nPRINT HIMEM\n"],
                            capture_output=True, text=True, timeout=300)
        ok = ok and "ula=4" in r2.stdout.splitlines()[-1] and "32768" in r2.stdout   # MODE 7 fantôme : HIMEM = &8000
        tests.append(("Master 128 : boot, TIME$, MODE 135 fantôme", ok))

    # 7b. Tube : second processeur 6502 (ROM Tube 1.10 requise)
    r = subprocess.run([str(BIN), "-2", "-f", "500", "-s", "-t", "PRINT HIMEM,PAGE\n"], capture_output=True, text=True, timeout=300)
    if "bbc_tube_rom.h absent" in r.stderr:
        print("  SKIP Tube : ROM Tube absente")
    else:
        lines = r.stdout.splitlines()
        ok = lines[1].startswith("Acorn TUBE 6502 64K") and any(l.split() == ["32768", "2048"] for l in lines[:25])
        tests.append(("Tube 6502 : boot, BASIC dans le parasite (PAGE=&800)", ok))

    # 7c. Chemins d'exécution : bbc_exec (bbc_tick4 : CRTC paresseux, 4 cycles
    #     déroulés, comme sur la carte) et bbc_tick seul (-1) doivent donner la
    #     même RAM, le même écran et le même son, au cycle près
    prog = "MODE 2\nFOR I=1 TO 200:GCOL 0,I MOD 8:DRAW RND(1279),RND(1023):SOUND 1,-10,I,1:NEXT\n"
    with tempfile.TemporaryDirectory() as d:
        out = {}
        for mode in ("", "-1"):
            ram, scr, snd = (Path(d) / f"{n}{mode}" for n in ("ram", "scr.ppm", "snd.wav"))
            cmd = [str(BIN)] + ([mode] if mode else []) + ["-f", "600", "-t", prog, "-r", str(ram), "-p", str(scr), "-a", str(snd)]
            subprocess.run(cmd, check=True, capture_output=True, timeout=600)
            out[mode] = [f.read_bytes() for f in (ram, scr, snd)]
        tests.append(("bbc_tick4 = bbc_tick (RAM, écran, son)", out[""] == out["-1"]))

    # 7d. Tables octet -> pixels construites progressivement comme sur la carte
    #     (bbc_headless_lut : BBC_LUT_PROGRESSIVE=16, 8 emplacements) : l'écran
    #     doit être celui du rendu PC, même quand la trame a plus de palettes que
    #     d'emplacements (logo ATARI de Battlezone : 9 palettes par trame)
    lut_bin = Path(os.environ.get("BBC_HEADLESS_LUT", BIN.with_name("bbc_headless_lut")))
    if lut_bin.exists():
        cas = [("raster", ["-f", "2400", "-t", (HERE / "data/raster_bars.bas").read_text() + "\n"])]
        bz = Path(os.environ.get("BATTLEZONE_SSD", Path.home() / "batllezone/Disc173-BATTLEZONERSTD.ssd"))
        if bz.exists():
            cas.append(("Battlezone logo ATARI", ["-f", "2150", "-w", "700", "-0", str(bz), "-b",
                                                   "-t", " ~~~ ~~~ ~~~ ~~~ ~~~ ~~~ ~~~ ~~~"]))
        else:
            print(f"  SKIP Battlezone : {bz} absent")
        for nom, args in cas:
            with tempfile.TemporaryDirectory() as d:
                imgs = []
                for exe in (BIN, lut_bin):
                    ppm = Path(d) / f"{exe.name}.ppm"
                    subprocess.run([str(exe)] + args + ["-p", str(ppm)], check=True, capture_output=True, timeout=600)
                    imgs.append(ppm.read_bytes())
                tests.append((f"tables progressives = rendu PC ({nom})", imgs[0] == imgs[1]))
    else:
        print(f"  SKIP tables progressives : {lut_bin} absent")

    # (Cœur 65C02 : tests CTest cpu_harte_* et cpu_klaus_*, docs/CPU-TESTS.md)

    for name, ok in tests:
        print(f"  [{'OK' if ok else 'ÉCHEC'}] {name}")
        failures += not ok
    print(f"{len(tests) - failures}/{len(tests)} tests OK")
    return 1 if failures else 0

if __name__ == "__main__":
    sys.exit(main())
