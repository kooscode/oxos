#!/usr/bin/env python3
"""Boot a BIOS in xemu (HDD copy, no disc), wait, then save the NV2A scanout surface (640x480x32) as a PNG.

xemu has no QMP screendump, so the surface is read with pmemsave from the CRTC start address.
usage: fbshot.py BIOS OUT.png [SECONDS]
"""
import os, sys, shutil, subprocess, tempfile, time, json, socket, struct, zlib
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tests"))
import boot_test as bt
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
bios, out = sys.argv[1], sys.argv[2]
work = tempfile.mkdtemp(prefix="xk-shot-")
sock = os.path.join(work, "qmp.sock")
shutil.copyfile(f"{root}/xbox-files/hdd/xbox_hdd.qcow2", f"{work}/hdd.qcow2")
open(f"{work}/xemu.toml", "w").write("[general]\nshow_welcome = false\nskip_boot_anim = true\n\n[sys.files]\n"
    f"bootrom_path = '{os.environ.get('MCPX_ROM', '')}'\nflashrom_path = '{bios}'\n"
    f"eeprom_path = '{work}/eeprom.bin'\nhdd_path = '{work}/hdd.qcow2'\n")
p = subprocess.Popen([os.path.expanduser("~/.local/bin/xemu"), "-config_path", f"{work}/xemu.toml", "-qmp", f"unix:{sock},server,nowait"],
                     stdout=open(f"{work}/log", "w"), stderr=subprocess.STDOUT)
try:
    time.sleep(float(sys.argv[3]) if len(sys.argv) > 3 else 12)
    mon = bt.Monitor(sock, {})
    start = int(mon.hmp("xp/1wx 0xfd600800").split(":")[1].split()[0], 16) & 0x03FFFFFF
    w, h, pitch = 640, 480, 640 * 4
    dump = os.path.join(work, "fb.bin")
    s = socket.socket(socket.AF_UNIX); s.connect(sock); f = s.makefile("rw")
    f.readline(); f.write('{"execute":"qmp_capabilities"}\n'); f.flush(); f.readline()
    f.write(json.dumps({"execute": "pmemsave", "arguments": {"val": start, "size": pitch * h, "filename": dump}}) + "\n"); f.flush()
    print(f.readline().strip())
    data = open(dump, "rb").read()
    raw = b"".join(b"\0" + b"".join(bytes((data[y*pitch+4*x+2], data[y*pitch+4*x+1], data[y*pitch+4*x])) for x in range(w)) for y in range(h))
    def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")
    open(out, "wb").write(png)
    print("fb at", hex(start), "->", out)
finally:
    p.terminate(); p.wait(5); shutil.rmtree(work, ignore_errors=True)
