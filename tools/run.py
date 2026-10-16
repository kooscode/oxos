#!/usr/bin/env python3
"""Boot a BIOS in xemu interactively, with a temporary copy of the HDD image and an optional disc image.

usage: run.py BIOS [--iso GAME.iso] [--hdd HDD.qcow2] [--keep]
The HDD copy lives in a temporary directory and is deleted when xemu exits (unless --keep); the original image is
never written. xemu stays open until you close it.
"""
import argparse, os, shutil, subprocess, sys, tempfile

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
ap = argparse.ArgumentParser()
ap.add_argument("bios")
ap.add_argument("--iso", help="disc image (XISO) for the DVD drive")
ap.add_argument("--hdd", default=os.path.join(root, "xbox-files/hdd/xbox_hdd.qcow2"))
ap.add_argument("--mcpx-rom", help="MCPX boot ROM dump (needed only for a Microsoft BIOS)")
ap.add_argument("--xemu", default=shutil.which("xemu") or os.path.expanduser("~/.local/bin/xemu"))
ap.add_argument("--keep", action="store_true", help="keep the temporary directory (HDD copy, EEPROM, log)")
a = ap.parse_args()

work = tempfile.mkdtemp(prefix="xk-run-")
hdd = os.path.join(work, "hdd.qcow2")
shutil.copyfile(a.hdd, hdd)
cfg = os.path.join(work, "xemu.toml")
open(cfg, "w").write(
    "[general]\nshow_welcome = false\nskip_boot_anim = true\n\n[sys.files]\n"
    f"bootrom_path = '{os.path.abspath(a.mcpx_rom) if a.mcpx_rom else ''}'\n"
    f"flashrom_path = '{os.path.abspath(a.bios)}'\n"
    f"eeprom_path = '{os.path.join(work, 'eeprom.bin')}'\n"
    f"hdd_path = '{hdd}'\n"
    + (f"dvd_path = '{os.path.abspath(a.iso)}'\n" if a.iso else ""))
print(f"xemu: {a.bios}" + (f" + {a.iso}" if a.iso else " (no disc)") + f"; work dir {work}")
try:
    subprocess.run([a.xemu, "-config_path", cfg], stdout=open(os.path.join(work, "xemu.log"), "w"),
                   stderr=subprocess.STDOUT)
finally:
    if not a.keep:
        shutil.rmtree(work, ignore_errors=True)
