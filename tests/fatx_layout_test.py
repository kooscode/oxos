#!/usr/bin/env python3
"""FATX disk layout test (spec/oxos_internal/45 §8, §9): large disks laid out and formatted as disk formatters do, each booted with the
partition probe title (tests/partition_probe_title) on a disc. The title reports where the kernel puts every partition, reads oxos_probe.txt on F: and G:
and writes and reads back oxos_probe_write.bin there; the test then reads the written file from the disk image as well.

Cases: tables with 48-bit entries on a 4 TB disk and clusters of 64 KB to 1 MB; F: over a whole 4 TB disk without a table; G: to the end of a 1.2 TB disk
without a table, and spec 45's capped G: on the same disk; a stale table left at the disk's end, and one that fits; a small F: below spec 45's minimum (the
space after E: on xemu's 8 GiB dashboard disk), and the same disk with nothing at F:.

usage: fatx_layout_test.py --bios BIOS --xbe PROBE.xbe --hdd DASHBOARD.qcow2 [--mcpx-rom MCPX] [--only NAME,...] [--keep] [--xemu PATH]
"""
import argparse, os, shutil, struct, subprocess, sys, tempfile, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mkxiso  # noqa: E402
from qcow2 import Qcow2  # noqa: E402
from fatx import FATX, format_partition, fatx_time  # noqa: E402
from controller_test import qmp  # noqa: E402  (fails the test when xemu does not answer: it froze)

RETAIL_END, LBA28_END = 0xEE8AB0, 0x0FFFFFFF
TB4 = 7814037168  # sectors of a 4 TB disk
LBA_DISK = 0x9001FFFF  # the largest disk a formatter lays out without a table with G: to its end (spec/oxos_internal/45 §8.2): G: 0x80020000 sectors
RETAIL = {1: (0x55F400, 0x9896B0), 2: (0x465400, 0xFA000), 3: (0x400, 0x177000), 4: (0x177400, 0x177000), 5: (0x2EE400, 0x177000)}
NAMES = ["XBOX DATA E", "XBOX SHELL C", "XBOX CACHE X", "XBOX CACHE Y", "XBOX CACHE Z", "XBOX EXTRA F", "XBOX EXTRA G"] + \
        [f"XBOX EXTRA {n}" for n in range(8, 15)]
WRITE_BYTES = 640 * 1024  # the probe title's oxos_probe_write.bin


def table(partitions, format_type=0):
    """A PARTINFO sector (spec/oxos_internal/45 §8.4): the retail five and partitions {number: (first sector, sectors)}, 48-bit entries."""
    t = bytearray(512)
    t[0:16] = b"****PARTINFO****"
    t[0x10] = format_type
    for number, (start, count) in {**RETAIL, **partitions}.items():
        e = 0x30 + 0x20 * (number - 1)
        t[e:e + 16] = NAMES[number - 1].encode().ljust(16, b"\0")
        struct.pack_into("<IIIHH", t, e + 0x10, 0x80000000, start & 0xFFFFFFFF, count & 0xFFFFFFFF, start >> 32, count >> 32)
    return bytes(t)


def format_volume(q, start, sectors, cluster, letter):
    """An empty FATX volume as a formatter writes it (spec/oxos_internal/45 §8.5), with oxos_probe.txt in its root: the probe reads that, and writes its own
    file only where it is."""
    format_partition(q, start, sectors, cluster=cluster, zero_fat=False)
    base, length = start * 512, sectors * 512
    entries = length // cluster + 1
    esize = 2 if entries < 0xFFF0 else 4
    data = base + 0x1000 + ((entries * esize + 4095) & ~4095)
    text = f"OXOS PROBE {letter}".encode()
    q.write(base + 0x1000 + 2 * esize, struct.pack("<H" if esize == 2 else "<I", 0xFFFF if esize == 2 else 0xFFFFFFFF))  # cluster 2: the file
    name = b"oxos_probe.txt"
    stamp = fatx_time()
    q.write(data, bytes([len(name), 0x20]) + name.ljust(42, b"\xff") + struct.pack("<IIIII", 2, len(text), stamp, stamp, stamp))
    q.write(data + cluster, text)


def case_disks(base):
    """Each case: (name, disk sectors, how to make it, the expected (first sector, sectors) of F: and G:, None for no partition). base: the dashboard
    disk's sectors (xemu's is 8 GiB: 0x117550 sectors after E:, too few for spec 45's F:)."""
    small = base - RETAIL_END
    f_lba28 = LBA28_END - RETAIL_END
    f_issue = (RETAIL_END, 0x80000000)
    g_issue = (RETAIL_END + 0x80000000, TB4 - RETAIL_END - 0x80000000)
    f_all = (RETAIL_END, TB4 - RETAIL_END)

    def table_4tb(q):  # the issue's disk: F: 64 KB, G: 256 KB, a table (type 1) at sector 0 and its copy in the last sector
        t = table({6: f_issue, 7: g_issue}, 1)
        q.write(0, t)
        q.write((TB4 - 1) * 512, t)
        format_volume(q, *f_issue, 0x10000, "F")
        format_volume(q, *g_issue, 0x40000, "G")

    def table_f_all(q):  # F: over the rest of the disk, 1 MB clusters (format type 3)
        q.write(0, table({6: f_all}, 3))
        format_volume(q, *f_all, 0x100000, "F")

    def no_table_f_all(q):  # no table, F: to the end at 512 KB, nothing at G:
        format_volume(q, *f_all, 0x80000, "F")

    def no_table_g(g_sectors):
        def make(q):
            format_volume(q, RETAIL_END, f_lba28, 0x10000, "F")
            format_volume(q, LBA28_END, g_sectors, 0x10000, "G")
        return make

    def stale_end_table(q):  # the no-table disk of g_to_end with a table from an earlier format left in the last sector
        no_table_g(0x80020000)(q)
        q.write((LBA_DISK - 1) * 512, table({6: (RETAIL_END, 0x1000000)}))

    def fitting_end_table(q):  # a table in the last sector whose volumes are there
        f, g = (RETAIL_END, 0x1000000), (RETAIL_END + 0x1000000, 0x2000000)
        q.write((LBA_DISK - 1) * 512, table({6: f, 7: g}))
        format_volume(q, *f, 0x4000, "F")
        format_volume(q, *g, 0x4000, "G")

    def small_f(q):
        format_volume(q, RETAIL_END, small, 0x4000, "F")

    return [
        ("table_4tb_64k_256k", TB4, table_4tb, f_issue, g_issue),
        ("table_4tb_f_1m", TB4, table_f_all, f_all, None),
        ("no_table_4tb_f_to_end_512k", TB4, no_table_f_all, f_all, None),
        ("no_table_g_to_end", LBA_DISK, no_table_g(0x80020000), (RETAIL_END, f_lba28), (LBA28_END, 0x80020000)),
        ("no_table_g_capped", LBA_DISK, no_table_g(0x74000000), (RETAIL_END, f_lba28), (LBA28_END, 0x74000000)),
        ("stale_table_at_end", LBA_DISK, stale_end_table, (RETAIL_END, f_lba28), (LBA28_END, 0x80020000)),
        ("fitting_table_at_end", LBA_DISK, fitting_end_table, (RETAIL_END, 0x1000000), (RETAIL_END + 0x1000000, 0x2000000)),
        ("small_f", base, small_f, (RETAIL_END, small), None),
        ("small_f_unformatted", base, lambda q: None, None, None),
    ]


def probe_result(sock, work):
    """The probe's answers once it has finished (its magic is written last): Entry[n] = (open, info, start, length), Marker[n], MarkerStatus[n],
    WriteStatus[n] for partitions 0-14. The block is told from the magic's copy in the title's data by C:'s known place."""
    ram = os.path.join(work, "ram.bin")
    qmp(sock, "pmemsave", val=0, size=0x4000000, filename=ram)
    if not os.path.exists(ram):
        return None
    data = open(ram, "rb").read()
    os.remove(ram)
    at = data.find(b"OXOSPRB1")
    while at >= 0:
        block = data[at:at + 760]
        entries = [struct.unpack_from("<iiQQ", block, 40 + 24 * n) for n in range(15)]
        if len(block) == 760 and struct.unpack_from("<I", block, 32)[0] == 512 and entries[2][2] == 0x465400 * 512:
            return {"entries": entries, "marker_status": struct.unpack_from("<15i", block, 400),
                    "markers": [block[460 + 16 * n:476 + 16 * n] for n in range(15)], "write_status": struct.unpack_from("<15i", block, 700)}
        at = data.find(b"OXOSPRB1", at + 1)
    return None


def run_case(a, xbe, name, sectors, make, expected_f, expected_g):
    work = tempfile.mkdtemp(prefix="xk-fatx-")
    sock, cfg, hdd, iso = (os.path.join(work, n) for n in ("qmp.sock", "xemu.toml", "hdd.qcow2", "probe.iso"))
    q = Qcow2(a.hdd)
    if sectors * 512 > q.size:
        q.grow(sectors * 512)
    make(q)
    q.save(hdd)
    mkxiso.write_image(iso, {"default.xbe": xbe})
    open(cfg, "w").write(
        "[general]\nshow_welcome = false\nskip_boot_anim = true\n\n[sys]\nmem_limit = '64'\n\n[sys.files]\n"
        f"bootrom_path = '{os.path.abspath(a.mcpx_rom) if a.mcpx_rom else ''}'\nflashrom_path = '{os.path.abspath(a.bios)}'\n"
        f"eeprom_path = '{os.path.join(work, 'eeprom.bin')}'\ndvd_path = '{iso}'\nhdd_path = '{hdd}'\n")
    log = open(os.path.join(work, "xemu.log"), "w")
    proc = subprocess.Popen([a.xemu, "-config_path", cfg, "-qmp", f"unix:{sock},server,nowait"], stdout=log, stderr=subprocess.STDOUT)
    r = None
    try:
        for _ in range(100):
            if os.path.exists(sock):
                break
            time.sleep(0.1)
        deadline = time.time() + 150
        while r is None and time.time() < deadline:
            time.sleep(3)
            r = probe_result(sock, work)
        if r:
            time.sleep(8)  # the file system cache writes the file back
            qmp(sock, "quit")
            proc.wait(30)
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(5)
            except subprocess.TimeoutExpired:
                proc.kill()
        log.close()
    checks = {}
    if not r:
        checks["the probe title finished"] = False
    else:
        for number, letter, want in ((6, "F", expected_f), (7, "G", expected_g)):
            opened, info, start, length = r["entries"][number]
            got = (start // 512, length // 512) if opened >= 0 and info >= 0 else None
            checks[f"{letter}: " + (f"at sector {want[0]:#x}, {want[1]:#x} sectors" if want else "none") +
                   ("" if got == want else f" (got {'none' if got is None else f'{got[0]:#x}, {got[1]:#x}'})")] = got == want
            if want:
                marker_ok = r["marker_status"][number] >= 0 and r["markers"][number].rstrip(b"\0") == f"OXOS PROBE {letter}".encode()
                checks[f"{letter}: oxos_probe.txt read" + ("" if marker_ok else f" (status {r['marker_status'][number] & 0xFFFFFFFF:#x})")] = marker_ok
                write_ok = r["write_status"][number] == 0
                checks[f"{letter}: 640 KB written and read back" + ("" if write_ok else f" (status {r['write_status'][number] & 0xFFFFFFFF:#x})")] = write_ok
                if write_ok:
                    expect = b"".join(struct.pack("<I", i * 4 + number) for i in range(WRITE_BYTES // 4))
                    try:
                        on_disk = FATX(Qcow2(hdd), None, place=want[0:1] + (want[1] * 512,)).read_file("oxos_probe_write.bin")
                    except Exception as e:  # noqa: BLE001 (any reader failure is a failed check)
                        on_disk = repr(e).encode()
                    checks[f"{letter}: the written file on the disk image"] = on_disk == expect
    ok = all(checks.values())
    print(f"  {name}: {'PASS' if ok else 'FAIL'}")
    for text, good in checks.items():
        print(f"    [{'PASS' if good else 'FAIL'}] {text}")
    if ok and not a.keep:
        shutil.rmtree(work, ignore_errors=True)
    elif not ok:
        print(f"    logs and disk in {work}")
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bios", required=True)
    ap.add_argument("--xbe", required=True, help="the partition probe title's default.xbe (build/tests/partition_probe_title/bin)")
    ap.add_argument("--hdd", required=True, help="the dashboard disk the test disks are grown from (never written)")
    ap.add_argument("--mcpx-rom")
    ap.add_argument("--only", help="comma-separated case names")
    ap.add_argument("--keep", action="store_true", help="keep each case's folder (disk image, logs)")
    ap.add_argument("--xemu", default=shutil.which("xemu") or os.path.expanduser("~/.local/bin/xemu"))
    a = ap.parse_args()
    xbe = open(a.xbe, "rb").read()
    cases = [c for c in case_disks(Qcow2(a.hdd).size // 512) if not a.only or c[0] in a.only.split(",")]
    print(f"FATX layout test: {os.path.basename(a.bios)}, {len(cases)} disks")
    results = [run_case(a, xbe, *c) for c in cases]
    print(f"{sum(results)} of {len(results)} disks pass")
    ok = all(results) and results
    print("RESULT:", "PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
