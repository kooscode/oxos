"""Minimal qcow2 reader for checking disk contents in tests (plain, zero and deflate-compressed clusters; the
xemu-dashboard disk image is compressed), with copy-with-changes writing: write() keeps changes in memory, save()
writes a new image. The source image is never modified.

save() writes a fresh qcow2 version 2 image with the same cluster size and virtual size: header, L1 table,
reference-count table and blocks (every used cluster counted once), L2 tables and the data clusters, uncompressed;
clusters that are entirely zero are left out. grow() raises the virtual size first, so save() makes a bigger disk:
the added space is unallocated and costs nothing on the host (`qcow2.py --grow SRC DST SIZE` from the shell)."""
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
        self.changed = {}  # guest cluster index -> bytearray, the changes made with write()

    @classmethod
    def blank(cls, size, cluster_bits=16):
        """An empty disk of size bytes, in memory: write() into it, then save()."""
        q = cls.__new__(cls)
        q.f, q.cluster_bits, q.size = None, cluster_bits, size
        q.cluster_size = 1 << cluster_bits
        q.l2_entries = q.cluster_size // 8
        q.l1 = [0] * ((size + q.cluster_size * q.l2_entries - 1) // (q.cluster_size * q.l2_entries))
        q._cached, q.changed = (None, None), {}
        return q

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

    def _source_cluster(self, idx):
        """Guest cluster idx as stored in the source image."""
        host = self._cluster(idx)
        if host is None:
            return bytes(self.cluster_size)
        if isinstance(host, bytes):
            return host
        self.f.seek(host)
        return self.f.read(self.cluster_size).ljust(self.cluster_size, b"\0")

    def write(self, offset, data):
        """Change guest bytes (kept in memory until save())."""
        data = memoryview(data)
        while len(data):
            idx, within = divmod(offset, self.cluster_size)
            n = min(len(data), self.cluster_size - within)
            if idx not in self.changed:
                self.changed[idx] = bytearray(self._source_cluster(idx))
            self.changed[idx][within:within + n] = data[:n]
            offset += n
            data = data[n:]

    def _used_clusters(self):
        """Guest cluster indices that may hold data: allocated in the source, or changed."""
        used = set(self.changed)
        for i1, l1 in enumerate(self.l1):
            table = l1 & 0x00FFFFFFFFFFFE00
            if not table:
                continue
            self.f.seek(table)
            entries = struct.unpack(f">{self.l2_entries}Q", self.f.read(8 * self.l2_entries))
            for i2, e in enumerate(entries):
                if e & ~1:  # an allocated or compressed cluster (not unallocated or a v3 zero cluster)
                    used.add(i1 * self.l2_entries + i2)
        return used

    def save(self, path):
        """Write the image, with the changes, as a new qcow2 file at path."""
        cs, l2n = self.cluster_size, self.l2_entries
        data = {}
        for idx in sorted(self._used_clusters()):
            c = bytes(self.changed[idx]) if idx in self.changed else self._source_cluster(idx)
            if any(c):
                data[idx] = c
        l1_size = (self.size + cs * l2n - 1) // (cs * l2n)
        l2_tables = sorted({idx // l2n for idx in data})
        l1_clusters = (8 * l1_size + cs - 1) // cs
        rc_per_block = cs // 2  # 16-bit reference counts (version 2)
        rt_clusters = rb_count = 1
        while True:  # the reference-count structures count themselves
            total = 1 + l1_clusters + rt_clusters + rb_count + len(l2_tables) + len(data)
            need_rb = (total + rc_per_block - 1) // rc_per_block
            need_rt = (8 * need_rb + cs - 1) // cs
            if (need_rb, need_rt) == (rb_count, rt_clusters):
                break
            rb_count, rt_clusters = need_rb, need_rt
        # Layout: header, L1, reference-count table, reference-count blocks, L2 tables, data.
        l1_at, rt_at = 1, 1 + l1_clusters
        rb_at = rt_at + rt_clusters
        l2_at = rb_at + rb_count
        data_at = l2_at + len(l2_tables)
        copied = 1 << 63  # reference count exactly 1
        l1 = [0] * l1_size
        l2 = {t: [0] * l2n for t in l2_tables}
        for n, t in enumerate(l2_tables):
            l1[t] = ((l2_at + n) * cs) | copied
        for n, idx in enumerate(sorted(data)):
            l2[idx // l2n][idx % l2n] = ((data_at + n) * cs) | copied
        header = struct.pack(">4sIQIIQIIQQIIQ", b"QFI\xfb", 2, 0, 0, self.cluster_bits, self.size, 0, l1_size,
                             l1_at * cs, rt_at * cs, rt_clusters, 0, 0)
        refcounts = bytearray(rb_count * cs)
        for c in range(total):
            struct.pack_into(">H", refcounts, 2 * c, 1)
        with open(path + ".part", "wb") as f:
            f.write(header.ljust(cs, b"\0"))
            f.write(struct.pack(f">{l1_size}Q", *l1).ljust(l1_clusters * cs, b"\0"))
            f.write(struct.pack(f">{rb_count}Q", *((rb_at + i) * cs for i in range(rb_count))).ljust(rt_clusters * cs, b"\0"))
            f.write(refcounts)
            for t in l2_tables:
                f.write(struct.pack(f">{l2n}Q", *l2[t]))
            for idx in sorted(data):
                f.write(data[idx])
        import os
        os.replace(path + ".part", path)

    def grow(self, size):
        """Make the virtual disk size bytes long (only larger; a multiple of 512) for the next save()."""
        if size < self.size or size % 512:
            raise ValueError("can only grow, to a multiple of 512 bytes")
        self.size = size

    def read(self, offset, length):
        out = bytearray()
        while length:
            idx, within = divmod(offset, self.cluster_size)
            n = min(length, self.cluster_size - within)
            if idx in self.changed:
                out += self.changed[idx][within:within + n]
                offset += n
                length -= n
                continue
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
    if len(sys.argv) == 5 and sys.argv[1] == "--grow":  # qcow2.py --grow SRC DST SIZE (bytes, or with K/M/G/T)
        units = {"K": 1 << 10, "M": 1 << 20, "G": 1 << 30, "T": 1 << 40}
        n = sys.argv[4]
        size = int(n[:-1]) * units[n[-1].upper()] if n[-1].upper() in units else int(n)
        q = Qcow2(sys.argv[2])
        q.grow(size)
        q.save(sys.argv[3])
        print(f"{sys.argv[3]}: {size} bytes ({size // 512} sectors)")
        sys.exit(0)
    q = Qcow2(sys.argv[1])
    print("size", q.size)
    for name, lba in (("E", 0x55F400), ("C", 0x465400), ("X", 0x400), ("Y", 0x177400), ("Z", 0x2EE400)):
        b = q.read(lba * 512, 16)
        print(name, b[:4], b.hex())
    print("sector0", q.read(0, 64).hex())
    print("refurb/config sector 3", q.read(3 * 512, 32).hex())
