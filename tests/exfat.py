"""exFAT on GPT disks for the tests (spec/oxos-internal/07-exfat-driver.md, 05-exfat-storage-spec.md): a writer that builds
a disk image from a tree of folders and files, and a reader that checks a volume completely.

Writer: Builder(size).add(path, data) ... .write(out.qcow2). The disk gets a protective MBR, a GPT with one
basic-data partition from sector 2048 to the end, and an exFAT volume in it. Files are stored contiguously (NoFatChain)
unless their name ends with ".frag", which are spread over every other cluster with a FAT chain (to exercise both
kinds). The retail reserved area (Partition0 bytes 0-0x7FFFF: refurb record, cache database) is kept at sectors
1024-2047, before the partition.

Reader: Exfat(Qcow2(path)) finds the volume through the GPT (or an MBR), and .check() verifies the boot checksums,
the up-case table checksum, every entry set's checksum and name hash, FAT chains, that no cluster is used twice,
and that the allocation bitmap marks exactly the clusters in use.

Shell:
  exfat.py --from-fatx SRC.qcow2 OUT.qcow2 [--size 2T] [--cluster 128K]   copy a FATX disk's C: and E: into /C and /E;
       [--add /F/dir=HOST ...]                                              also copy a host file or folder in
       (OUT.raw: a sparse raw disk, the added files streamed: for disc images larger than memory)
  exfat.py --check IMG.qcow2                                                check a volume, list problems
  exfat.py --has IMG.qcow2 'F:\\dir\\x.xbe'                                    is that file there (exit 0)
  exfat.py --ls IMG.qcow2 [/path]                                           list a directory
"""
import os, struct, sys, zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qcow2 import Qcow2  # noqa: E402

SECTOR = 512
PART_START = 2048          # first partition sector (1 MiB)
RESERVED_AT = 1024         # where the retail reserved area (0x400 sectors) is kept
BASIC_DATA = bytes.fromhex("A2A0D0EBE5B9334487C068B6B72699C7")
FOLDERS = ("E", "C", "X", "Y", "Z", "F", "OXOS")


def crc32(b):
    return zlib.crc32(b) & 0xFFFFFFFF


def sum32(data, skip=()):
    s = 0
    for i, b in enumerate(data):
        if i in skip:
            continue
        s = (((s >> 1) | (s << 31)) + b) & 0xFFFFFFFF
    return s


def sum16(data, skip=()):
    s = 0
    for i, b in enumerate(data):
        if i in skip:
            continue
        s = (((s >> 1) | (s << 15)) + b) & 0xFFFF
    return s


def upcase_table():
    """A compressed up-case table: ASCII and Latin-1 letters; every other character maps to itself."""
    t = [0xFFFF, 0x61] + [c - 0x20 for c in range(0x61, 0x7B)] + [0xFFFF, 0xE0 - 0x7B]
    t += [c if c == 0xF7 else c - 0x20 for c in range(0xE0, 0xFF)] + [0x178, 0xFFFF, 0x10000 - 0x100]
    return struct.pack(f"<{len(t)}H", *t)


def expand_upcase(raw):
    vals = struct.unpack(f"<{len(raw) // 2}H", raw)
    out, i = [], 0
    while i < len(vals) and len(out) < 0x10000:
        if vals[i] == 0xFFFF and i + 1 < len(vals):
            n = vals[i + 1]
            out.extend(range(len(out), len(out) + n))
            i += 2
        else:
            out.append(vals[i])
            i += 1
    out.extend(range(len(out), 0x10000))
    return out[:0x10000]


def name_hash(name, up):
    data = bytearray()
    for ch in name:
        c = up[ord(ch)] if ord(ch) < 0x10000 else ord(ch)
        data += struct.pack("<H", c)
    return sum16(data)


def dos_time(t=1735689600):  # 2025-01-01 00:00 by default, so images are reproducible
    import time
    lt = time.gmtime(t)
    return ((lt.tm_year - 1980) << 25 | lt.tm_mon << 21 | lt.tm_mday << 16 | lt.tm_hour << 11 | lt.tm_min << 5
            | lt.tm_sec // 2)


def parse_size(s):
    units = {"K": 1 << 10, "M": 1 << 20, "G": 1 << 30, "T": 1 << 40}
    return int(s[:-1]) * units[s[-1].upper()] if s[-1].upper() in units else int(s)


# --- Writer

class Node:
    def __init__(self, name, data=None, attrs=None):
        self.name, self.data = name, data
        self.is_dir = data is None
        self.children = {}
        self.attrs = attrs if attrs is not None else (0x10 if self.is_dir else 0x20)
        self.clusters = []


class HostFile:
    """A host file added by path and read only while the disk is written (for files too large to hold in memory)."""

    def __init__(self, path):
        self.path, self.size = path, os.path.getsize(path)

    def __len__(self):
        return self.size

    def __bool__(self):
        return self.size > 0

    def __getitem__(self, k):
        start, stop, _ = k.indices(self.size)
        with open(self.path, "rb") as f:
            f.seek(start)
            return f.read(max(0, stop - start))

    def chunks(self, size):
        with open(self.path, "rb") as f:
            while True:
                b = f.read(size)
                if not b:
                    return
                yield b


class RawDisk:
    """A raw (sparse) disk file written in place: the Qcow2.blank stand-in for disks larger than memory."""

    def __init__(self, path, size):
        self.f = open(path, "wb")
        self.f.truncate(size)

    def write(self, offset, data):
        self.f.seek(offset)
        self.f.write(data)

    def save(self, path):
        self.f.close()


class Builder:
    def __init__(self, size, cluster=128 << 10, folders=FOLDERS, serial=0x0A0B0C0D):
        self.size, self.cluster, self.serial = size, cluster, serial
        self.root = Node("")
        self.reserved = None
        for f in folders:
            self.add("/" + f)

    def add(self, path, data=None, attrs=None):
        cur = self.root
        parts = [p for p in path.replace("\\", "/").split("/") if p]
        for i, p in enumerate(parts):
            last = i == len(parts) - 1
            key = p.upper()
            if key not in cur.children:
                cur.children[key] = Node(p, data if last else None, attrs if last else None)
            elif last and data is not None:
                cur.children[key].data = data
            cur = cur.children[key]
        return cur

    def add_tree(self, path, host, lazy=False):
        for dirpath, dirs, files in os.walk(host):
            rel = os.path.relpath(dirpath, host).replace(os.sep, "/")
            base = path if rel == "." else f"{path}/{rel}"
            self.add(base)
            for f in files:
                self.add(f"{base}/{f}", HostFile(os.path.join(dirpath, f)) if lazy else
                         open(os.path.join(dirpath, f), "rb").read())

    def _geometry(self, sectors):
        csec = self.cluster // SECTOR
        spc_shift = csec.bit_length() - 1
        fat_off, count = 128, (sectors - 128) // csec
        while True:
            fat_len = ((count + 2) * 4 + SECTOR - 1) // SECTOR
            heap = (fat_off + fat_len + csec - 1) // csec * csec
            new = (sectors - heap) // csec
            if new == count:
                return spc_shift, fat_off, fat_len, heap, count
            count = new

    @staticmethod
    def _set(node, up, first, length, nofat):
        name = node.name
        n, parts = len(name), (len(name) + 14) // 15
        f = bytearray(32)
        f[0], f[1] = 0x85, 1 + parts
        struct.pack_into("<H", f, 4, node.attrs)
        struct.pack_into("<III", f, 8, dos_time(), dos_time(), dos_time())
        s = bytearray(32)
        s[0], s[1], s[3] = 0xC0, 1 | (2 if nofat else 0), n
        struct.pack_into("<H", s, 4, name_hash(name, up))
        struct.pack_into("<QIIQ", s, 8, length, 0, first, length)
        out = f + s
        for i in range(parts):
            e = bytearray(32)
            e[0] = 0xC1
            chunk = name[15 * i:15 * i + 15].encode("utf-16-le")
            e[2:2 + len(chunk)] = chunk
            out += e
        struct.pack_into("<H", out, 2, sum16(out, (2, 3)))
        return bytes(out)

    def write(self, path, reserved=None, raw=False):
        sectors = self.size // SECTOR
        last_usable = sectors - 34
        part_sectors = last_usable - PART_START + 1
        spc_shift, fat_off, fat_len, heap, count = self._geometry(part_sectors)
        cs = self.cluster
        up_raw = upcase_table()
        up = expand_upcase(up_raw)
        nxt = [2]
        used = set()
        fat = {0: 0xFFFFFFF8, 1: 0xFFFFFFFF}

        def alloc(n, frag=False):
            out = []
            for _ in range(max(n, 0)):
                out.append(nxt[0])
                nxt[0] += 2 if frag else 1
            assert nxt[0] < count + 2, "volume full"
            used.update(out)
            return out

        def chain(clusters):
            for a, b in zip(clusters, clusters[1:] + [None]):
                fat[a] = b if b is not None else 0xFFFFFFFF

        bitmap_len = (count + 7) // 8
        bitmap_cl = alloc((bitmap_len + cs - 1) // cs)
        up_cl = alloc((len(up_raw) + cs - 1) // cs)
        chain(bitmap_cl)
        chain(up_cl)

        def dir_entries(node):
            n = 3 if node is self.root else 0
            return n + sum(2 + (len(c.name) + 14) // 15 for c in node.children.values())

        def layout(node):
            if node.is_dir:
                node.clusters = alloc(max(1, (dir_entries(node) * 32 + cs - 1) // cs))
                if node is self.root:
                    chain(node.clusters)
                for c in node.children.values():
                    layout(c)
            elif node.data:
                frag = node.name.lower().endswith(".frag")
                node.clusters = alloc((len(node.data) + cs - 1) // cs, frag)
                if frag:
                    chain(node.clusters)

        layout(self.root)
        q = RawDisk(path, self.size) if raw else Qcow2.blank(self.size)
        base = PART_START * SECTOR
        heap_at = base + heap * SECTOR

        def coff(c):
            return heap_at + (c - 2) * cs

        def put(clusters, data):
            if isinstance(data, HostFile):  # contiguous clusters: stream the file
                assert clusters == list(range(clusters[0], clusters[0] + len(clusters)))
                at = coff(clusters[0])
                for piece in data.chunks(8 << 20):
                    if piece.count(0) != len(piece):
                        q.write(at, piece)
                    at += len(piece)
                return
            for i, c in enumerate(clusters):
                piece = data[i * cs:(i + 1) * cs]
                if piece.count(0) != len(piece):
                    q.write(coff(c), piece)

        def serialize(node):
            out = bytearray()
            if node is self.root:
                label = bytearray(32)
                label[0], label[1] = 0x83, 4
                label[2:10] = "OXOS".encode("utf-16-le")
                bm = bytearray(32)
                bm[0] = 0x81
                struct.pack_into("<IQ", bm, 20, bitmap_cl[0], bitmap_len)
                uc = bytearray(32)
                uc[0] = 0x82
                struct.pack_into("<I", uc, 4, sum32(up_raw))
                struct.pack_into("<IQ", uc, 20, up_cl[0], len(up_raw))
                out += label + bm + uc
            for c in node.children.values():
                if c.is_dir:
                    length = len(c.clusters) * cs
                    out += self._set(c, up, c.clusters[0], length, True)
                    serialize(c)
                else:
                    size = len(c.data or b"")
                    first = c.clusters[0] if c.clusters else 0
                    out += self._set(c, up, first, size, bool(c.clusters) and not c.name.lower().endswith(".frag"))
                    if c.data:
                        put(c.clusters, c.data)
            put(node.clusters, bytes(out))

        serialize(self.root)
        bitmap = bytearray(bitmap_len)
        for c in used:
            bitmap[(c - 2) // 8] |= 1 << ((c - 2) % 8)
        put(bitmap_cl, bytes(bitmap))
        put(up_cl, up_raw)
        fat_at = base + fat_off * SECTOR
        for idx in sorted(fat):  # only the FAT sectors that hold entries are written
            q.write(fat_at + idx * 4, struct.pack("<I", fat[idx]))

        boot = bytearray(SECTOR)
        boot[0:3] = b"\xEB\x76\x90"
        boot[3:11] = b"EXFAT   "
        struct.pack_into("<QQIIIIIIHHBBBBB", boot, 64, PART_START, part_sectors, fat_off, fat_len, heap, count,
                         self.root.clusters[0], self.serial, 0x0100, 0, 9, spc_shift, 1, 0x80,
                         min(100, len(used) * 100 // count))
        boot[510:512] = b"\x55\xAA"
        region = bytearray(boot)
        for _ in range(8):
            ext = bytearray(SECTOR)
            ext[508:512] = b"\x00\x00\x55\xAA"
            region += ext
        region += bytes(2 * SECTOR)
        region += struct.pack("<I", sum32(region, (106, 107, 112))) * (SECTOR // 4)
        q.write(base, region)
        q.write(base + 12 * SECTOR, region)

        # GPT: protective MBR, header + entries at the start, entries + header at the end.
        mbr = bytearray(SECTOR)
        struct.pack_into("<BBBBBBBBII", mbr, 446, 0, 0, 2, 0, 0xEE, 0xFF, 0xFF, 0xFF, 1, min(sectors - 1, 0xFFFFFFFF))
        mbr[510:512] = b"\x55\xAA"
        q.write(0, mbr)
        entry = bytearray(128)
        entry[0:16] = BASIC_DATA
        entry[16:32] = bytes(range(16, 32))
        struct.pack_into("<QQQ", entry, 32, PART_START, last_usable, 0)
        entry[56:56 + 8] = "OXOS".encode("utf-16-le")
        entries = bytes(entry) + bytes(127 * 128)
        ecrc = crc32(entries)

        def header(my, other, entries_lba):
            h = bytearray(92)
            struct.pack_into("<8sIII4xQQQQ16sQIII", h, 0, b"EFI PART", 0x10000, 92, 0, my, other, 34, last_usable,
                             bytes(range(16)), entries_lba, 128, 128, ecrc)
            struct.pack_into("<I", h, 16, crc32(bytes(h)))
            return bytes(h).ljust(SECTOR, b"\0")

        q.write(SECTOR, header(1, sectors - 1, 2))
        q.write(2 * SECTOR, entries)
        q.write((sectors - 33) * SECTOR, entries)
        q.write((sectors - 1) * SECTOR, header(sectors - 1, 1, sectors - 33))
        if reserved:
            q.write(RESERVED_AT * SECTOR, reserved[:0x400 * SECTOR])
        q.save(path)


# --- Reader and checker

class Entry:
    def __init__(self, name, attrs, first, size, valid, nofat, slot, raw):
        self.name, self.attrs, self.first, self.size = name, attrs, first, size
        self.valid, self.nofat, self.slot, self.raw = valid, nofat, slot, raw

    @property
    def is_dir(self):
        return bool(self.attrs & 0x10)


class Exfat:
    def __init__(self, disk, lba=None):
        self.disk = disk
        self.problems = []
        self.lba = lba if lba is not None else self.find_partition()
        b = disk.read(self.lba * SECTOR, SECTOR)
        if b[3:11] != b"EXFAT   ":
            raise ValueError("not an exFAT volume")
        (self.part_off, self.vol_len, self.fat_off, self.fat_len, self.heap, self.count, self.root_cluster,
         self.serial, self.revision, self.flags, bps, spc, self.nfats) = struct.unpack_from("<QQIIIIIIHHBBB", b, 64)
        self.sector = 1 << bps
        self.cs = 1 << (bps + spc)
        region = disk.read(self.lba * SECTOR, 12 * self.sector)
        want = sum32(region[:11 * self.sector], (106, 107, 112))
        if region[11 * self.sector:] != struct.pack("<I", want) * (self.sector // 4):
            self.problems.append("main boot region checksum")
        backup = disk.read((self.lba + 12) * SECTOR, 12 * self.sector)
        if sum32(backup[:11 * self.sector], (106, 107, 112)) != want:
            self.problems.append("backup boot region differs")
        self.up = list(range(0x10000))
        self.bitmap = b""
        self.bitmap_cl = self.up_cl = []
        root = self.dir_bytes(self.root_cluster, None, False)
        for i in range(0, len(root), 32):
            t = root[i]
            if t == 0x81:
                first, length = struct.unpack_from("<IQ", root, i + 20)
                self.bitmap_cl = self.clusters(first, length, False)
                self.bitmap = self.read(first, length, False)
            elif t == 0x82:
                chk = struct.unpack_from("<I", root, i + 4)[0]
                first, length = struct.unpack_from("<IQ", root, i + 20)
                self.up_cl = self.clusters(first, length, False)
                raw = self.read(first, length, False)
                if sum32(raw) != chk:
                    self.problems.append("up-case table checksum")
                self.up = expand_upcase(raw)
            elif t == 0:
                break

    def find_partition(self):
        s0 = self.disk.read(0, SECTOR)
        if s0[510:512] != b"\x55\xAA":
            raise ValueError("no partition table")
        if s0[446 + 4] == 0xEE:
            for lba in (1, self.disk.size // SECTOR - 1):
                h = bytearray(self.disk.read(lba * SECTOR, 92))
                if h[:8] != b"EFI PART":
                    continue
                crc = struct.unpack_from("<I", h, 16)[0]
                struct.pack_into("<I", h, 16, 0)
                if crc32(bytes(h)) != crc:
                    self.problems.append(f"GPT header at {lba}: CRC")
                    continue
                elba, n, esize, ecrc = struct.unpack_from("<QIII", h, 72)
                entries = self.disk.read(elba * SECTOR, n * esize)
                if crc32(entries) != ecrc:
                    self.problems.append(f"GPT entries at {elba}: CRC")
                    continue
                for i in range(n):
                    e = entries[i * esize:(i + 1) * esize]
                    if e[:16] == BASIC_DATA:
                        return struct.unpack_from("<Q", e, 32)[0]
            raise ValueError("no usable GPT basic-data partition")
        for i in range(4):
            t, first = s0[446 + 16 * i + 4], struct.unpack_from("<I", s0, 446 + 16 * i + 8)[0]
            if t == 0x07:
                return first
        raise ValueError("no exFAT MBR partition")

    def coff(self, c):
        return self.lba * SECTOR + self.heap * self.sector + (c - 2) * self.cs

    def fat(self, c):
        return struct.unpack("<I", self.disk.read(self.lba * SECTOR + self.fat_off * self.sector + 4 * c, 4))[0]

    def clusters(self, first, length, nofat):
        if not first:
            return []
        n = (length + self.cs - 1) // self.cs if length is not None else None
        if nofat:
            return list(range(first, first + n))
        out, c = [], first
        while 2 <= c < self.count + 2 and (n is None or len(out) < n):
            if c in out:
                self.problems.append(f"FAT chain loop at cluster {c}")
                break
            out.append(c)
            c = self.fat(c)
        if n is not None and len(out) < n:
            self.problems.append(f"FAT chain from {first} too short ({len(out)} of {n})")
        return out

    def read(self, first, length, nofat):
        return b"".join(self.disk.read(self.coff(c), self.cs) for c in self.clusters(first, length, nofat))[:length]

    def dir_bytes(self, first, length, nofat):
        return b"".join(self.disk.read(self.coff(c), self.cs) for c in self.clusters(first, length, nofat))

    def entries(self, raw, where="/"):
        out, i = [], 0
        while i < len(raw):
            t = raw[i]
            if t == 0:
                break
            if t == 0x85:
                sec = raw[i + 1]
                st = raw[i:i + 32 * (sec + 1)]
                if len(st) < 64 or st[32] != 0xC0:
                    self.problems.append(f"{where}: entry set at slot {i // 32} without a stream entry")
                    i += 32
                    continue
                if sum16(st, (2, 3)) != struct.unpack_from("<H", st, 2)[0]:
                    self.problems.append(f"{where}: entry set at slot {i // 32}: checksum")
                attrs = struct.unpack_from("<H", st, 4)[0]
                flags, nlen, nhash = st[33], st[35], struct.unpack_from("<H", st, 36)[0]
                valid, first, size = struct.unpack_from("<Q4xIQ", st, 40)
                name = b"".join(st[64 + 32 * k + 2:64 + 32 * k + 32] for k in range(sec - 1)).decode(
                    "utf-16-le", "replace")[:nlen]
                if name_hash(name, self.up) != nhash:
                    self.problems.append(f"{where}{name}: name hash")
                if valid > size:
                    self.problems.append(f"{where}{name}: valid data length beyond data length")
                out.append(Entry(name, attrs, first, size, valid, bool(flags & 2), i // 32, st))
                i += 32 * (sec + 1)
            else:
                i += 32
        return out

    def listdir(self, path="/"):
        e = self.lookup(path)
        if e is None or (e != "root" and not e.is_dir):
            return None
        raw = (self.dir_bytes(self.root_cluster, None, False) if e == "root"
               else self.dir_bytes(e.first, e.size, e.nofat))
        return self.entries(raw, path.rstrip("/") + "/")

    def lookup(self, path):
        parts = [p for p in path.replace("\\", "/").split("/") if p]
        cur = "root"
        for p in parts:
            items = self.listdir_entry(cur)
            match = [x for x in items if x.name.upper() == p.upper()]
            if not match:
                return None
            cur = match[0]
        return cur

    def listdir_entry(self, e):
        raw = (self.dir_bytes(self.root_cluster, None, False) if e == "root"
               else self.dir_bytes(e.first, e.size, e.nofat))
        return self.entries(raw)

    def read_file(self, path):
        e = self.lookup(path)
        if e is None or e == "root" or e.is_dir:
            return None
        return self.read(e.first, e.valid, e.nofat).ljust(e.size, b"\0")

    def check(self):
        """Walk the whole tree; returns (problems, files, free clusters)."""
        owner = {}

        def claim(clusters, what):
            for c in clusters:
                if c in owner:
                    self.problems.append(f"cluster {c} used by {owner[c]} and {what}")
                owner[c] = what

        claim(self.bitmap_cl, "bitmap")
        claim(self.up_cl, "up-case table")
        claim(self.clusters(self.root_cluster, None, False), "/")
        files = {}

        def walk(raw, path):
            for e in self.entries(raw, path):
                full = path + e.name
                cl = self.clusters(e.first, e.size, e.nofat)
                if e.is_dir:
                    if e.size % self.cs or e.valid != e.size:
                        self.problems.append(f"{full}: directory length")
                    claim(cl, full)
                    files[full + "/"] = None
                    walk(self.dir_bytes(e.first, e.size, e.nofat), full + "/")
                else:
                    claim(cl, full)
                    files[full] = e.size

        walk(self.dir_bytes(self.root_cluster, None, False), "/")
        marked = {c + 2 for c in range(self.count) if self.bitmap[c // 8] >> (c % 8) & 1}
        if marked != set(owner):
            extra, missing = sorted(marked - set(owner))[:8], sorted(set(owner) - marked)[:8]
            self.problems.append(f"bitmap: marked but unused {extra}, used but not marked {missing}")
        return self.problems, files, self.count - len(marked)


def from_fatx(src, out, size, cluster, extra=()):
    """OUT ending in .raw: a sparse raw disk, with the --add files read while it is written (no size limit from
    memory; qemu-img convert -O qcow2 makes a qcow2 of it)."""
    import fatx
    raw = out.endswith(".raw")
    disk = Qcow2(src)
    b = Builder(size, cluster)

    def copy(part, letter, first=None, prefix=""):
        for name, attr, cl, sz, _ in part.listdir(first):
            dest = f"/{letter}{prefix}/{name}"
            if attr & 0x10:
                b.add(dest, attrs=attr)
                copy(part, letter, cl, f"{prefix}/{name}")
            else:
                b.add(dest, part.read_chain(cl, sz), attrs=attr)

    for letter in ("C", "E"):
        copy(fatx.Fatx(disk, letter), letter)
    for dest, host in extra:  # --add /F/apps/demo=HOST: a host file or folder copied in
        if os.path.isdir(host):
            b.add_tree(dest, host, lazy=raw)
        else:
            b.add(dest, HostFile(host) if raw else open(host, "rb").read())
    b.write(out, disk.read(0, 0x400 * SECTOR), raw=raw)


if __name__ == "__main__":
    a = sys.argv[1:]
    if a[:1] == ["--from-fatx"]:
        pairs = list(zip(a[3::2], a[4::2]))
        opts = dict(pairs)
        extra = [tuple(v.split("=", 1)) for k, v in pairs if k == "--add"]
        from_fatx(a[1], a[2], parse_size(opts.get("--size", "2T")), parse_size(opts.get("--cluster", "128K")), extra)
        print(f"{a[2]}: written")
    elif a[:1] == ["--check"]:
        v = Exfat(Qcow2(a[1]))
        problems, files, free = v.check()
        print(f"{len(files)} files and folders, {free} free clusters of {v.count} ({v.cs // 1024} KB)")
        for p in problems:
            print("PROBLEM:", p)
        sys.exit(1 if problems else 0)
    elif a[:1] == ["--has"]:
        # --has IMG 'F:\\dir\\x.xbe' (or /F/dir/x.xbe): exit 0 if the file is there, 1 if not, 2 if not an exFAT disk
        path = a[2].replace("\\", "/")
        if len(path) > 1 and path[1] == ":":
            path = "/" + path[0].upper() + path[2:]
        try:
            v = Exfat(Qcow2(a[1]))
        except Exception:
            sys.exit(2)
        e = v.lookup(path)
        sys.exit(0 if e is not None and e != "root" and not e.is_dir else 1)
    elif a[:1] == ["--ls"]:
        v = Exfat(Qcow2(a[1]))
        for e in v.listdir(a[2] if len(a) > 2 else "/") or []:
            print(f"{'d' if e.is_dir else '-'} {e.size:>12} {e.name}")
    else:
        print(__doc__)
