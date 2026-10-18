"""Minimal read-only qcow2 reader for checking disk contents in tests: plain, zero and deflate-compressed clusters
(the xemu-dashboard disk image is compressed)."""
import struct, zlib


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
        version = struct.unpack(">I", h[4:8])[0]
        if version >= 3 and struct.unpack(">Q", h[72:80])[0] & 1:
            raise ValueError("dirty/corrupt bit set")
        # Version 3 may name another compression than deflate (header byte 104, when the header is that long).
        if version >= 3 and struct.unpack(">I", h[100:104])[0] > 104:
            self.f.seek(104)
            if self.f.read(1) not in (b"\0", b""):
                raise ValueError("only deflate-compressed clusters are supported")
        self.cluster_size = 1 << self.cluster_bits
        self.l2_entries = self.cluster_size // 8
        self.f.seek(l1_offset)
        self.l1 = struct.unpack(f">{l1_size}Q", self.f.read(8 * l1_size))
        self._cached = (None, None)  # (cluster index, decompressed bytes)

    def _compressed(self, index, e):
        """A compressed cluster's contents: the entry holds the host offset and the number of extra 512-byte
        sectors the compressed data spans; the data is a raw deflate stream."""
        if self._cached[0] == index:
            return self._cached[1]
        bits = 62 - (self.cluster_bits - 8)
        off = e & ((1 << bits) - 1)
        sectors = ((e >> bits) & ((1 << (62 - bits)) - 1)) + 1
        self.f.seek(off)
        data = zlib.decompressobj(-15).decompress(self.f.read(sectors * 512 - (off & 511)), self.cluster_size)
        data = data.ljust(self.cluster_size, b"\0")
        self._cached = (index, data)
        return data

    def _cluster(self, index):
        l1 = self.l1[index // self.l2_entries] & 0x00FFFFFFFFFFFE00
        if not l1:
            return None
        self.f.seek(l1 + 8 * (index % self.l2_entries))
        e = struct.unpack(">Q", self.f.read(8))[0]
        if e & (1 << 62):
            return self._compressed(index, e)
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
            elif isinstance(host, bytes):  # a compressed cluster, already decompressed
                out += host[within:within + n]
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
