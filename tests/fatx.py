"""Read-only FATX reader over a Qcow2 image, for checking the kernel's file system results in tests."""
import struct

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


if __name__ == "__main__":
    import sys
    from qcow2 import Qcow2
    d = Qcow2(sys.argv[1])
    for letter in "CEXYZ":
        f = Fatx(d, letter)
        print(letter, "cluster", f.cluster, "clusters", f.clusters, "entry", f.esize, "root", f.root)
        for e in f.listdir()[:12]:
            print("   ", e)
