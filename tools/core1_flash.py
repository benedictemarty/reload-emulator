#!/usr/bin/env python3
"""core1_flash.py — fonctions et données en flash atteintes depuis le cœur 1

Sur le RP2040, le cœur 1 fabrique l'image (PicoDVI) : s'il exécute du code en
flash, ou lit une table en flash, pendant que le cœur 0 sollicite la flash
(énumération USB, FatFs, code froid), il attend le cache XIP et la ligne arrive
en retard : trait rouge (STRATEGIE-NEO6502.md, section 3 bis). Ces appels se
cachent : un memset recréé par GCC, un relais (« veneer ») vers interp_save du
SDK appelé par l'encodeur 16 bpp de PicoDVI, etc.

L'outil lit le désassemblage de l'ELF (arm-none-eabi-objdump), part des points
d'entrée du cœur 1, suit les appels directs (bl, b vers une autre fonction,
relais résolus par leur littéral) et liste :
  - les fonctions en flash atteintes, avec le chemin le plus court ;
  - les littéraux pointant en flash dans les fonctions atteintes (tables lues
    en flash, ou pointeurs de fonction) ;
  - les appels indirects (blx rN, bx rN), que l'outil ne peut pas suivre :
    vérifier leurs cibles à la main (ou les donner avec --racine).

Usage :
  core1_flash.py FIRMWARE.elf [--racine NOM]... [--seules] [--ignorer NOM]...
  Racines : celles de la liste RACINES présentes dans l'ELF, plus les --racine
  (rappel de ligne, IRQ propres au port) ; --seules : uniquement les --racine.
  --ignorer : fonction à ne pas suivre (appelée seulement au démarrage, par
  exemple dvi_start) ; elle n'est pas signalée.
Code de sortie : 0 si rien en flash n'est atteint, 1 sinon, 2 en cas d'erreur.
"""
import argparse
import bisect
import collections
import re
import subprocess
import sys

OBJDUMP = "arm-none-eabi-objdump"
FLASH = (0x10000000, 0x20000000)
# Points d'entrée usuels du cœur 1 dans les ports Neo6502 (ceux absents de l'ELF sont ignorés)
RACINES = ["core1_main", "core1_entry", "core1_scanline_callback", "_scanline_callback",
           "dvi_dma_irq_handler", "dvi_dma0_irq", "dvi_dma1_irq", "audio_dma_irq_handler"]

# Appelées une fois au démarrage du cœur 1 (ou jamais : panic) : signalées à part,
# sans compter comme faute ; --strict pour les compter quand même
DEMARRAGE = {"audio_init", "dvi_start", "dvi_register_irqs_this_core", "dvi_init", "panic",
             "irq_set_enabled", "irq_set_exclusive_handler", "multicore_lockout_victim_init"}

ENTETE = re.compile(r"^([0-9a-f]+) <(.+)>:$")
APPEL = re.compile(r"\t(bl|blx|b|b\.n|b\.w)\s+([0-9a-f]+) <([^>+]+)(?:\+0x[0-9a-f]+)?>")
INDIRECT = re.compile(r"\t(blx|bx)\s+(r\d+|ip|r1[0-2])\s*$")
LITTERAL = re.compile(r"\t\.word\t0x([0-9a-f]+)")
CHARGE = re.compile(r"\tldr\s+(r\d+),\s*\[pc.*@ \(([0-9a-f]+)")   # ldr rN, [pc, #k] @ (adresse du littéral)
ADRESSE = re.compile(r"^\s*([0-9a-f]+):")


def lire(elf):
    """Fonctions : nom -> (adresse, lignes du désassemblage)."""
    sortie = subprocess.run([OBJDUMP, "-d", elf], capture_output=True, text=True, check=True).stdout
    fonctions, courante = {}, None
    for ligne in sortie.splitlines():
        m = ENTETE.match(ligne)
        if m:
            courante = m.group(2)
            fonctions[courante] = (int(m.group(1), 16), [])
        elif courante and ligne.strip():
            fonctions[courante][1].append(ligne)
    return fonctions


def symboles(elf):
    """Tous les symboles (fonctions et données) triés par adresse, pour nommer un littéral."""
    sortie = subprocess.run(["arm-none-eabi-nm", "-n", "-S", elf], capture_output=True, text=True, check=True).stdout
    table = []
    for ligne in sortie.splitlines():
        p = ligne.split()
        if len(p) == 4:
            table.append((int(p[0], 16), int(p[1], 16), p[3]))
    return table


def nommer(table, adresses, adr):
    i = bisect.bisect_right(adresses, adr) - 1
    if i >= 0:
        debut, taille, nom = table[i]
        if debut <= adr < debut + max(taille, 1):
            return nom if adr == debut else f"{nom}+0x{adr - debut:x}"
    return f"0x{adr:08x}"


def en_flash(adr):
    return FLASH[0] <= adr < FLASH[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("elf")
    ap.add_argument("--racine", action="append", default=[], help="point d'entrée ajouté aux racines par défaut")
    ap.add_argument("--seules", action="store_true", help="ne parcourir que les --racine")
    ap.add_argument("--ignorer", action="append", default=[])
    ap.add_argument("--strict", action="store_true", help="compter aussi les fonctions de démarrage")
    ap.add_argument("--court", action="store_true", help="une ligne de résumé (pour les scripts)")
    a = ap.parse_args()

    fonctions = lire(a.elf)
    par_adresse = {adr: nom for nom, (adr, _) in fonctions.items()}
    table = symboles(a.elf)
    adresses = [t[0] for t in table]
    defaut = [r for r in RACINES if r in fonctions]
    racines = list(a.racine) if a.seules else defaut + [r for r in a.racine if r not in defaut]
    absentes = [r for r in racines if r not in fonctions]
    if absentes:
        print("racine absente de l'ELF :", ", ".join(absentes), file=sys.stderr)
        return 2
    if not racines:
        print("aucune racine trouvée : donner --racine", file=sys.stderr)
        return 2

    def cible_relais(lignes):
        for l in lignes:
            m = LITTERAL.search(l)
            if m:
                return int(m.group(1), 16) & ~1
        return None

    parent = {r: None for r in racines}
    file = collections.deque(racines)
    flash_code, flash_donnees, indirects, panics = [], [], [], []
    while file:
        f = file.popleft()
        adr, lignes = fonctions[f]
        if en_flash(adr):
            flash_code.append(f)
            continue   # Déjà en faute : pas la peine de suivre ce qu'elle appelle
        suivantes = []
        if f.endswith("_veneer"):
            c = cible_relais(lignes)
            if c is not None:
                suivantes.append(par_adresse.get(c, nommer(table, adresses, c)))
                if c not in par_adresse:
                    fonctions.setdefault(suivantes[-1], (c, []))
        else:
            # Littéraux chargés juste avant un appel à panic (message d'erreur) : lus
            # seulement si le firmware s'arrête, sans effet sur l'image
            valeurs = {}
            for l in lignes:
                ma, ml = ADRESSE.match(l), LITTERAL.search(l)
                if ma and ml:
                    valeurs[int(ma.group(1), 16)] = int(ml.group(1), 16)
            de_panic = set()
            for i, l in enumerate(lignes):
                m = CHARGE.search(l)
                if m and any("panic" in s and "\tbl" in s for s in lignes[i + 1:i + 5]):
                    de_panic.add(int(m.group(2), 16))
            for l in lignes:
                m = APPEL.search(l)
                if m and m.group(3) != f:
                    suivantes.append(m.group(3))
                elif INDIRECT.search(l) and not l.rstrip().endswith("lr"):
                    indirects.append((f, l.split("\t")[0].strip().rstrip(":")))
                m = LITTERAL.search(l)
                if m and en_flash(int(m.group(1), 16)):
                    ma = ADRESSE.match(l)
                    if ma and int(ma.group(1), 16) in de_panic:
                        panics.append((f, nommer(table, adresses, int(m.group(1), 16))))
                    else:
                        flash_donnees.append((f, nommer(table, adresses, int(m.group(1), 16) & ~1)))
        for s in suivantes:
            if s in a.ignorer or s in parent or s not in fonctions:
                continue
            parent[s] = f
            file.append(s)

    def chemin(f):
        c = []
        while f is not None:
            c.append(f)
            f = parent[f]
        return " <- ".join(c)

    fautes = sorted(f for f in flash_code if a.strict or f not in DEMARRAGE)
    demarrage = sorted(f for f in flash_code if f not in fautes)
    donnees = sorted(set(flash_donnees))
    if a.court:
        print(f"{len(fautes)} fonction(s) en flash" + (f" ({', '.join(fautes)})" if fautes else "") +
              f", {len(donnees)} littéral(aux) en flash, {len(indirects)} appel(s) indirect(s)")
        return 1 if fautes else 0
    print(f"racines : {', '.join(racines)} ; fonctions atteintes : {len(parent)}")
    print(f"\nFonctions en FLASH atteintes depuis le cœur 1 : {len(fautes)}")
    for f in fautes:
        print(f"  {f} (0x{fonctions[f][0]:08x})\n      {chemin(f)}")
    if demarrage:
        print(f"\nEn flash mais appelées au démarrage seulement (non comptées) : {', '.join(demarrage)}")
    print(f"\nLittéraux pointant en flash (tables lues ou pointeurs de fonction) : {len(donnees)}")
    for f, d in donnees:
        print(f"  {d}   dans {f}")
    if panics:
        print("\nMessages de panic en flash (lus seulement à l'arrêt du firmware, non comptés) : " +
              ", ".join(f"{d} dans {f}" for f, d in sorted(set(panics))))
    print(f"\nAppels indirects non suivis (vérifier leurs cibles) : {len(indirects)}")
    for f, adr in indirects:
        print(f"  {f} @ {adr}")
    return 1 if fautes else 0


if __name__ == "__main__":
    sys.exit(main())
