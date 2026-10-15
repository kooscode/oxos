"""Minimal read-only qcow2 reader (uncompressed clusters) for checking disk contents in tests."""
import struct


class Qcow2:
    def __init__(self, path):
        self.f = open(path, "rb")
        h = self.f.read(104)
        if h[:4] != b"QFI\xfb":
            raise ValueError("not a qcow2 image")
        self.cluster_bits = struct.unpack(">I", h[20:24])[0]
        self.size = struct.unpack(">Q", h[24:32])[0]
        l1_size = struct.unpack(">I", h[36:40])[0]
        l1_offset = struct.unpack(">Q", h[40:48])[0]
        if struct.unpack(">I", h[4:8])[0] >= 3 and struct.unpack(">Q", h[72:80])[0] & 1:
            raise ValueError("dirty/corrupt bit set")
        self.cluster_size = 1 << self.cluster_bits
        self.l2_entries = self.cluster_size // 8
        self.f.seek(l1_offset)
        self.l1 = struct.unpack(f">{l1_size}Q", self.f.read(8 * l1_size))

    def _cluster(self, index):
        l1 = self.l1[index // self.l2_entries] & 0x00FFFFFFFFFFFE00
        if not l1:
            return None
        self.f.seek(l1 + 8 * (index % self.l2_entries))
        e = struct.unpack(">Q", self.f.read(8))[0]
        if e & (1 << 62):
            raise ValueError("compressed clusters not supported")
        off = e & 0x00FFFFFFFFFFFE00
        if e & 1 and not off:  # zero cluster flag (v3)
            return None
        return off or None

    def read(self, offset, length):
        out = bytearray()
        while length:
            idx, within = divmod(offset, self.cluster_size)
            n = min(length, self.cluster_size - within)
            host = self._cluster(idx)
            if host is None:
                out += bytes(n)
            else:
                self.f.seek(host + within)
                out += self.f.read(n)
            offset += n
            length -= n
        return bytes(out)


if __name__ == "__main__":
    import sys
    q = Qcow2(sys.argv[1])
    print("size", q.size)
    for name, lba in (("E", 0x55F400), ("C", 0x465400), ("X", 0x400), ("Y", 0x177400), ("Z", 0x2EE400)):
        b = q.read(lba * 512, 16)
        print(name, b[:4], b.hex())
    print("sector0", q.read(0, 64).hex())
    print("refurb/config sector 3", q.read(3 * 512, 32).hex())
