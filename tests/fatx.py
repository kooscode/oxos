"""FATX reader over a Qcow2 image, for checking the kernel's file system results in tests, and a small writer
(FatxWriter: create directories, write files, copy a host folder) for preparing disk images; the changes go
through Qcow2.write() and are saved as a new image with Qcow2.save() (spec/stock/04b for the format).

usage: fatx.py IMAGE                                   list each partition's root
       fatx.py IMAGE --add DRIVE:\\PATH=HOST_PATH ... --out NEW_IMAGE
                                                       copy host files or folders into a copy of IMAGE"""
import os, struct, time

PARTITIONS = {"E": (0x55F400, 0x1312D6000), "C": (0x465400, 0x1F400000), "X": (0x400, 0x2EE00000),
              "Y": (0x177400, 0x2EE00000), "Z": (0x2EE400, 0x2EE00000)}


class Fatx:
    def __init__(self, disk, letter):
        lba, self.length = PARTITIONS[letter]
        self.disk, self.base = disk, lba * 512
        sig, self.serial, spc, self.root = struct.unpack("<4sIII", disk.read(self.base, 16))
        assert sig == b"FATX"
        self.cluster = spc * 512
        entries = self.length // self.cluster + 1
        self.esize = 2 if entries < 0xFFF0 else 4
        fat_size = (entries * self.esize + 4095) & ~4095
        self.fat = self.base + 0x1000
        self.data = self.fat + fat_size
        self.clusters = (self.length - 0x1000 - fat_size) // self.cluster

    def next(self, c):
        raw = self.disk.read(self.fat + c * self.esize, self.esize)
        v = struct.unpack("<H" if self.esize == 2 else "<I", raw)[0]
        if self.esize == 2 and v >= 0xFFF0:
            v |= 0xFFFF0000
        return v

    def chain(self, c):
        out = []
        while 1 <= c <= self.clusters and len(out) <= self.clusters:
            out.append(c)
            c = self.next(c)
        return out

    def read_chain(self, first, size=None):
        data = b"".join(self.disk.read(self.data + (c - 1) * self.cluster, self.cluster) for c in self.chain(first))
        return data if size is None else data[:size]

    def listdir(self, first=None):
        raw = self.read_chain(self.root if first is None else first)
        out = []
        for i in range(0, len(raw), 64):
            n, attr = raw[i], raw[i + 1]
            if n in (0, 0xFF):
                break
            if n == 0xE5 or n > 42:
                continue
            name = raw[i + 2:i + 2 + n].decode("latin-1")
            first, size, ct, wt, at = struct.unpack("<IIIII", raw[i + 44:i + 64])
            out.append((name, attr, first, size, wt))
        return out

    def lookup(self, path):
        cur, entry = None, None
        for part in [p for p in path.split("\\") if p]:
            match = [e for e in self.listdir(cur) if e[0].lower() == part.lower()]
            if not match:
                return None
            entry = match[0]
            cur = entry[2]
        return entry

    def read_file(self, path):
        e = self.lookup(path)
        return None if e is None else self.read_chain(e[2], e[3])


BAD_NAME_CHARS = set('"*/:<>?\\|')


def fatx_time(t=None):
    """A FATX timestamp: date in the high word (day, month << 5, (year - 2000) << 9), time in the low word."""
    lt = time.localtime(t)
    date = lt.tm_mday | lt.tm_mon << 5 | max(0, lt.tm_year - 2000) << 9
    return date << 16 | lt.tm_sec // 2 | lt.tm_min << 5 | lt.tm_hour << 11


class FatxWriter(Fatx):
    """Fatx with directory and file creation. The disk must support write() (Qcow2)."""

    def __init__(self, disk, letter):
        super().__init__(disk, letter)
        self.fat_bytes = bytearray(disk.read(self.fat, (self.clusters + 1) * self.esize))
        self.free_hint = 1

    def next(self, c):  # from the in-memory FAT, which holds this writer's changes
        v = struct.unpack_from("<H" if self.esize == 2 else "<I", self.fat_bytes, c * self.esize)[0]
        if self.esize == 2 and v >= 0xFFF0:
            v |= 0xFFFF0000
        return v

    def _set(self, c, v):
        if self.esize == 2:
            v &= 0xFFFF
        packed = struct.pack("<H" if self.esize == 2 else "<I", v)
        self.fat_bytes[c * self.esize:(c + 1) * self.esize] = packed
        self.disk.write(self.fat + c * self.esize, packed)

    def _allocate(self, count):
        """A new chain of count clusters (not necessarily contiguous); returns its clusters."""
        out, c = [], self.free_hint
        while len(out) < count:
            if c > self.clusters:
                raise OSError("FATX volume full")
            if self.next(c) == 0:
                out.append(c)
            c += 1
        self.free_hint = c
        for a, b in zip(out, out[1:] + [0xFFFFFFFF]):
            self._set(a, b)
        return out

    def _write_clusters(self, clusters, data):
        for n, c in enumerate(clusters):
            piece = data[n * self.cluster:(n + 1) * self.cluster]
            self.disk.write(self.data + (c - 1) * self.cluster, piece.ljust(self.cluster, b"\0"))

    def _dir_cluster(self):
        c = self._allocate(1)
        self._write_clusters(c, b"\xff" * self.cluster)  # an empty directory: all end-of-directory markers
        return c[0]

    def _add_entry(self, dir_first, name, attr, first, size):
        if not 1 <= len(name) <= 42 or set(name) & BAD_NAME_CHARS or name in (".", "..") or \
                any(not 0x20 <= ord(ch) <= 0x7E for ch in name):
            raise ValueError(f"invalid FATX name {name!r}")
        stamp = fatx_time()
        entry = struct.pack("<BB42sIIIII", len(name), attr, name.encode().ljust(42, b"\xff"), first, size,
                            stamp, stamp, stamp)
        chain = self.chain(self.root if dir_first is None else dir_first)
        per = self.cluster // 64
        for n, c in enumerate(chain):
            raw = self.disk.read(self.data + (c - 1) * self.cluster, self.cluster)
            for i in range(per):
                if raw[i * 64] in (0x00, 0xFF, 0xE5):  # end of directory or a deleted entry: reuse the slot
                    self.disk.write(self.data + (c - 1) * self.cluster + i * 64, entry)
                    return
        if len(chain) * per >= 4096:
            raise OSError("FATX directory full (4096 entries)")
        new = self._dir_cluster()  # grow the directory by one cluster
        self._set(chain[-1], new)
        self.disk.write(self.data + (new - 1) * self.cluster, entry)

    def _parent(self, path):
        parts = [p for p in path.split("\\") if p]
        parent = None
        for part in parts[:-1]:
            match = [e for e in self.listdir(parent) if e[0].lower() == part.lower()]
            if not match or not match[0][1] & 0x10:
                raise FileNotFoundError(f"no directory {part!r} in {path!r}")
            parent = match[0][2]
        return parent, parts[-1]

    def mkdir(self, path):
        """Create a directory (its parent must exist); returns its first cluster. An existing one is reused."""
        parent, name = self._parent(path)
        match = [e for e in self.listdir(parent) if e[0].lower() == name.lower()]
        if match:
            if not match[0][1] & 0x10:
                raise FileExistsError(f"{path!r} exists and is a file")
            return match[0][2]
        first = self._dir_cluster()
        self._add_entry(parent, name, 0x10, first, 0)
        return first

    def makedirs(self, path):
        parts = [p for p in path.split("\\") if p]
        for n in range(1, len(parts) + 1):
            self.mkdir("\\".join(parts[:n]))

    def write_file(self, path, data):
        """Create a file (its parent must exist, the name must be new)."""
        parent, name = self._parent(path)
        if any(e[0].lower() == name.lower() for e in self.listdir(parent)):
            raise FileExistsError(f"{path!r} exists")
        clusters = self._allocate((len(data) + self.cluster - 1) // self.cluster) if data else []
        self._write_clusters(clusters, data)
        self._add_entry(parent, name, 0x20, clusters[0] if clusters else 0, len(data))

    def copy_in(self, host, path):
        """Copy a host file, or a host folder recursively, to path."""
        if os.path.isdir(host):
            self.makedirs(path)
            for name in sorted(os.listdir(host)):
                self.copy_in(os.path.join(host, name), path + "\\" + name)
        else:
            self.write_file(path, open(host, "rb").read())


if __name__ == "__main__":
    import sys
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # qcow2.py, also under python3 -I
    from qcow2 import Qcow2
    d = Qcow2(sys.argv[1])
    if "--add" in sys.argv:
        args = sys.argv[2:]
        out = args[args.index("--out") + 1]
        adds = [a for i, a in enumerate(args) if i and args[i - 1] == "--add"]
        writers = {}
        for spec in adds:
            target, host = spec.split("=", 1)
            letter, path = target[0].upper(), target[2:].lstrip("\\")
            w = writers.setdefault(letter, FatxWriter(d, letter))
            w.copy_in(host, path)
            print(f"{host} -> {letter}:\\{path}")
        d.save(out)
        print(f"saved {out}")
        sys.exit(0)
    for letter in "CEXYZ":
        f = Fatx(d, letter)
        print(letter, "cluster", f.cluster, "clusters", f.clusters, "entry", f.esize, "root", f.root)
        for e in f.listdir()[:12]:
            print("   ", e)
