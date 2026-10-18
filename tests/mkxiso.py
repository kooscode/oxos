"""Build a small XDVDFS (XISO) test image: volume descriptor at sector 32, binary-tree directories."""
import os, struct

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"


def pattern(n, seed):
    return bytes(((i * 7 + seed) ^ (i >> 9)) & 0xFF for i in range(n))


# name -> bytes (file) or dict (directory)
TREE = {
    "default.xbe": b"XBEH" + pattern(5000, 1),
    "AAA.txt": b"first entry\n",
    "zzz.dat": pattern(100, 3),
    "media": {"a.txt": b"hello dvd", "big.bin": pattern(300 * 1024 + 77, 5), "Sub": {"deep.txt": b"deep"}},
}


def build(path, default_xbe=None, relaunch=False):
    tree = dict(TREE)
    if default_xbe is not None:
        tree["default.xbe"] = default_xbe
        tree["media"] = dict(tree["media"], **{"second.xbe": default_xbe})
    if relaunch:  # the test title relaunches itself as media\\second.xbe
        tree["relaunch.txt"] = b"quick reboot"
    return write_image(path, tree)


def tree_from_dir(path):
    """name -> bytes or dict for every file under path (e.g. an nxdk app's bin/ folder)."""
    tree = {}
    for name in os.listdir(path):
        full = os.path.join(path, name)
        tree[name] = tree_from_dir(full) if os.path.isdir(full) else open(full, "rb").read()
    return tree


def write_image(path, tree):
    """An XISO of tree (name -> bytes or dict) at path."""
    sectors = {}  # sector -> bytes
    next_sector = [33]

    def alloc(nbytes):
        s = next_sector[0]
        next_sector[0] += max(1, (nbytes + SECTOR - 1) // SECTOR)
        return s

    def write_dir(d):
        names = sorted(d, key=lambda n: n.upper().encode())
        # place children first so their sectors are known
        info = {}
        for n in names:
            v = d[n]
            if isinstance(v, dict):
                s, size = write_dir(v)
                info[n] = (s, size, 0x10)
            else:
                s = alloc(len(v))
                sectors[s] = v
                info[n] = (s, len(v), 0x00)
        # balanced tree: middle element is the root of each range; lay entries out in pre-order
        order = []

        def pre(lo, hi):
            if lo >= hi:
                return None
            mid = (lo + hi) // 2
            node = {"name": names[mid]}
            order.append(node)
            node["left"] = pre(lo, mid)
            node["right"] = pre(mid + 1, hi)
            return node

        pre(0, len(names))
        offs, pos = {}, 0
        for node in order:
            size = (14 + len(node["name"]) + 3) & ~3
            if pos // SECTOR != (pos + size - 1) // SECTOR:
                pos = (pos + SECTOR - 1) & ~(SECTOR - 1)
            offs[id(node)] = pos
            pos += size
        table = bytearray(b"\xff" * ((pos + SECTOR - 1) & ~(SECTOR - 1) or SECTOR))
        for node in order:
            o = offs[id(node)]
            s, size, attr = info[node["name"]]
            left = offs[id(node["left"])] // 4 if node["left"] else 0
            right = offs[id(node["right"])] // 4 if node["right"] else 0
            name = node["name"].encode()
            entry = struct.pack("<HHIIBB", left, right, s, size, attr, len(name)) + name
            entry += b"\xff" * (((len(entry) + 3) & ~3) - len(entry))
            table[o:o + len(entry)] = entry
        s = alloc(len(table))
        sectors[s] = bytes(table)
        return s, len(table)

    root_sector, root_size = write_dir(tree)
    total = next_sector[0] + 16
    img = bytearray(total * SECTOR)
    vd = bytearray(SECTOR)
    vd[0:20] = MAGIC
    vd[20:28] = struct.pack("<II", root_sector, root_size)
    vd[28:36] = struct.pack("<Q", 0x01C0000000000000)
    vd[0x7EC:0x800] = MAGIC
    img[32 * SECTOR:33 * SECTOR] = vd
    for s, data in sectors.items():
        img[s * SECTOR:s * SECTOR + len(data)] = data
    open(path, "wb").write(img)
    return img


def checksum(data):
    s = 0
    for b in data:
        s = (s * 31 + b) & 0xFFFFFFFF
    return s


if __name__ == "__main__":
    import sys
    build(sys.argv[1])
