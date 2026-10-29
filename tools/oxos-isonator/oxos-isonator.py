#!/usr/bin/env python3
"""oxos-isonator: Xbox disc images (XISO) and per-game launchers for OXOS and Cerbios.

For every game it makes a folder a dashboard (XBMC4Gamers, EvoX, the OXOS title launcher) lists as a game:

    Halo 2/
        Halo 2.iso       the disc image (XISO; or Halo 2.1.iso + Halo 2.2.iso with --split)
        default.xbe      the launcher: OXOS's ISO launcher, carrying the game's title ID, name, region, version and
                         title image, so the dashboard shows the game itself

Starting default.xbe mounts the image in its folder as the console's disc and starts the game, through the virtual
disc device that Cerbios and OXOS share (spec/oxos-internal/16-virtual-discs.md, spec/cerbios/07).

Sources:
  - an extracted game folder (default.xbe at its top): an XISO is built from it, in OUT/<name>/;
  - a disc image (.iso, XISO or full dump; a split one by its .1.iso part): only the launcher is made, next to it;
  - a folder of images, or of game folders: each one in turn.

Images are one file by default: exFAT (OXOS) has no file size limit. --split cuts an image over 4 GB into two parts
for a FATX disk (Cerbios, stock partitions), named NAME.1.iso and NAME.2.iso.

Python 3.8+, standard library only. The launcher template is tools/iso-launcher/bin/default.xbe
(make -C kernel iso-launcher), or --launcher.
"""
import argparse
import hashlib
import os
import shutil
import struct
import sys
import time

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
VD_SECTOR = 32                    # the XDVDFS volume descriptor
FULL_DUMP_OFFSET = 0x18300000     # the game partition of a full disc dump
FULL_DUMP_MIN = 0x18310800        # a full dump's first part is larger than this (spec/cerbios/07 §2.1)
PART_MAX = 0xFFF00000             # one part of a split image: under FATX's 4 GB file limit, whole sectors
ATTR_DIR, ATTR_FILE = 0x10, 0x80  # as on retail discs
CHUNK = 8 << 20
LAUNCHER_DEFAULT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "iso-launcher", "bin", "default.xbe")
IMAGE_EXTS = (".iso", ".cci")


def fail(msg):
    raise SystemExit(f"oxos-isonator: {msg}")


def log(msg):
    print(msg, flush=True)


# --- Reading images ------------------------------------------------------------------------------------------------

def image_parts(path):
    """The part files of an image given by any part: [x.iso], [x.1.iso, x.2.iso]."""
    stem, ext = os.path.splitext(path)
    if stem.endswith((".1", ".2")):
        base = stem[:-2]
        parts = [f"{base}.1{ext}", f"{base}.2{ext}"]
        return [p for p in parts if os.path.exists(p)]
    return [path]


def image_name(path):
    """The game's name from an image path: 'Halo 2' for Halo 2.iso and Halo 2.1.iso."""
    stem = os.path.splitext(os.path.basename(path))[0]
    return stem[:-2] if stem.endswith((".1", ".2")) else stem


class Image:
    """An XISO or full disc dump of one or two parts, read as one; the XDVDFS volume in it."""

    def __init__(self, path):
        self.paths = image_parts(path)
        if path.lower().endswith(".cci"):
            fail(f"{path}: CCI images are read by the consoles, not by this tool (tools/mkcci makes them)")
        self.files = [open(p, "rb") for p in self.paths]
        self.sizes = [os.path.getsize(p) for p in self.paths]
        self.size = sum(self.sizes)
        self.base = None
        for base in (0, FULL_DUMP_OFFSET):
            if base and self.sizes[0] <= FULL_DUMP_MIN:
                continue
            vd = self.read(base + VD_SECTOR * SECTOR, SECTOR)
            if vd[:20] == MAGIC and (base == 0 or vd[-20:] == MAGIC):
                self.base = base
                self.root = struct.unpack_from("<II", vd, 20)
                break
        if self.base is None:
            fail(f"{path}: not an Xbox disc image (no XDVDFS volume at sector 32, nor in a full dump's game partition)")

    def close(self):
        for f in self.files:
            f.close()

    def read(self, offset, length):
        out = bytearray()
        for f, size in zip(self.files, self.sizes):
            if length and offset < size:
                f.seek(offset)
                piece = f.read(min(length, size - offset))
                out += piece
                length -= len(piece)
                offset = 0
            else:
                offset -= size
        return bytes(out)

    def read_at(self, sector, offset, length):
        return self.read(self.base + sector * SECTOR + offset, length)

    def entries(self, sector, size):
        """(name, sector, size, attributes) of a directory, in name order."""
        if not size:
            return []
        table = self.read_at(sector, 0, size)
        out, stack, seen = [], [0], set()
        while stack:
            off = stack.pop()
            if off in seen or off + 14 > len(table):
                continue
            seen.add(off)
            left, right, sec, sz, attr, n = struct.unpack_from("<HHIIBB", table, off)
            if left == 0xFFFF:  # padding: an empty directory
                continue
            out.append((table[off + 14:off + 14 + n].decode("latin-1"), sec, sz, attr))
            stack += [o * 4 for o in (left, right) if o]
        return sorted(out, key=lambda e: e[0].upper().encode("latin-1"))

    def walk(self, sector=None, size=None, prefix=""):
        """(path, sector, size, attributes) of every file and folder, folders before their contents."""
        if sector is None:
            sector, size = self.root
        for name, sec, sz, attr in self.entries(sector, size):
            path = f"{prefix}{name}"
            yield path, sec, sz, attr
            if attr & ATTR_DIR:
                yield from self.walk(sec, sz, path + "/")

    def find(self, path):
        sector, size = self.root
        found = None
        for part in path.replace("\\", "/").split("/"):
            match = [e for e in self.entries(sector, size) if e[0].upper() == part.upper()]
            if not match:
                return None
            found = match[0]
            sector, size = found[1], found[2]
        return found

    def read_file(self, path):
        e = self.find(path)
        if e is None or e[3] & ATTR_DIR:
            fail(f"{self.paths[0]}: no {path} in the image")
        return self.read_at(e[1], 0, e[2])


# --- XBE ------------------------------------------------------------------------------------------------------------

class Xbe:
    """The parts of an XBE this tool reads and changes (layout in spec/stock/05)."""

    def __init__(self, data, what):
        if data[:4] != b"XBEH":
            fail(f"{what}: not an XBE")
        self.data = bytearray(data)
        self.what = what
        self.base, self.header_size, self.image_size = struct.unpack_from("<III", data, 0x104)
        self.cert = struct.unpack_from("<I", data, 0x118)[0] - self.base
        self.count, sec_va = struct.unpack_from("<II", data, 0x11C)
        self.sections = []
        for i in range(self.count):
            at = sec_va - self.base + i * 0x38
            flags, va, vsize, raw, rsize, name_va = struct.unpack_from("<IIIIII", data, at)
            end = data.index(b"\0", name_va - self.base)
            name = data[name_va - self.base:end].decode("latin-1")
            self.sections.append({"name": name, "flags": flags, "va": va, "vsize": vsize, "raw": raw,
                                  "rsize": rsize, "header": at})

    def section(self, name):
        for s in self.sections:
            if s["name"] == name:
                return s
        return None

    def section_data(self, name):
        s = self.section(name)
        return None if s is None else bytes(self.data[s["raw"]:s["raw"] + s["rsize"]])

    @property
    def title_id(self):
        return struct.unpack_from("<I", self.data, self.cert + 8)[0]

    @property
    def title_name(self):
        raw = bytes(self.data[self.cert + 0x0C:self.cert + 0x0C + 80])
        return raw.decode("utf-16-le", "replace").split("\0")[0]

    @property
    def region(self):
        return struct.unpack_from("<I", self.data, self.cert + 0xA0)[0]

    @property
    def version(self):
        return struct.unpack_from("<I", self.data, self.cert + 0xAC)[0]

    def add_section(self, name, flags, payload):
        """Append a section (not loaded at start-up) with its header in the header area's free space."""
        d = self.data
        first_raw = min(s["raw"] for s in self.sections)
        first_va = min(s["va"] for s in self.sections) - self.base
        # New header area tail: the section headers (moved, plus one), the new name, two shared-page counters.
        at = (self.header_size + 3) & ~3
        table = at
        names = table + (self.count + 1) * 0x38
        counters = (names + len(name) + 1 + 3) & ~3
        end = counters + 4
        if end > min(first_raw, first_va):
            fail(f"{self.what}: no room in the XBE header for another section")
        old_table = struct.unpack_from("<I", d, 0x120)[0] - self.base
        d[table:table + self.count * 0x38] = d[old_table:old_table + self.count * 0x38]
        d[names:names + len(name) + 1] = name.encode("latin-1") + b"\0"
        d[counters:counters + 4] = bytes(4)
        va = max(s["va"] + s["vsize"] for s in self.sections)
        va = (va + 0xFFF) & ~0xFFF
        raw = (len(d) + 0xFFF) & ~0xFFF
        digest = hashlib.sha1(struct.pack("<I", len(payload)) + payload).digest()
        entry = struct.pack("<IIIIIIIII", flags, va, len(payload), raw, len(payload), self.base + names,
                            self.base + counters, self.base + counters, self.base + counters + 2) + digest
        d[table + self.count * 0x38:table + (self.count + 1) * 0x38] = entry
        d += bytes(raw - len(d)) + payload
        d += bytes(((len(d) + 0xFFF) & ~0xFFF) - len(d))
        self.count += 1
        self.header_size = end
        self.image_size = max(self.image_size, va + len(payload) - self.base)
        struct.pack_into("<III", d, 0x104, self.base, self.header_size, self.image_size)
        struct.pack_into("<II", d, 0x11C, self.count, self.base + table)


# Certificate fields copied from the game: title ID, title name, alternate title IDs, region, ratings, disc number,
# version. The launcher keeps its own media types (it runs from the hard disk) and keys.
CERT_COPY = ((0x08, 4), (0x0C, 80), (0x5C, 64), (0xA0, 4), (0xA4, 4), (0xA8, 4), (0xAC, 4))
TITLE_IMAGE = "$$XTIMAGE"


def make_launcher(template, game, title=None):
    """The launcher XBE for a game: the template with the game's identity and title image."""
    out = Xbe(bytes(template.data), template.what)
    for off, n in CERT_COPY:
        out.data[out.cert + off:out.cert + off + n] = game.data[game.cert + off:game.cert + off + n]
    if title:
        name = title.encode("utf-16-le")[:78]
        out.data[out.cert + 0x0C:out.cert + 0x0C + 80] = name + bytes(80 - len(name))
    image = game.section_data(TITLE_IMAGE)
    if image and not out.section(TITLE_IMAGE):
        out.add_section(TITLE_IMAGE, game.section(TITLE_IMAGE)["flags"] & ~0x4, image)  # bit 2: preload
    return bytes(out.data)


LAUNCHER_MARK = b"OXOS ISO launcher"  # in every ISO launcher build (its heading), so older launchers are updated too


def is_launcher(data):
    """True when data is an OXOS ISO launcher (made by this tool, from any launcher build)."""
    return data[:4] == b"XBEH" and LAUNCHER_MARK in data


def describe(game):
    regions = "/".join(n for bit, n in ((1, "NA"), (2, "JP"), (4, "PAL")) if game.region & bit) or "-"
    return f"{game.title_name} (title ID {game.title_id:08X}, {regions}, version {game.version:08X})"


# --- Writing XISO images ---------------------------------------------------------------------------------------------

class Dir:
    def __init__(self, name):
        self.name, self.children = name, []


class File:
    def __init__(self, name, size, chunks):
        self.name, self.size, self.chunks = name, size, chunks  # chunks(): the data, in pieces


def tree_from_folder(path, name=""):
    d = Dir(name)
    for entry in sorted(os.scandir(path), key=lambda e: e.name):
        if entry.is_dir(follow_symlinks=True):
            d.children.append(tree_from_folder(entry.path, entry.name))
        elif entry.is_file(follow_symlinks=True):
            size = entry.stat().st_size

            def chunks(p=entry.path):
                with open(p, "rb") as f:
                    while True:
                        b = f.read(CHUNK)
                        if not b:
                            return
                        yield b
            d.children.append(File(entry.name, size, chunks))
    return d


def name_key(name):
    return name.upper().encode("latin-1")


def check_tree(d, where=""):
    seen = {}
    for c in d.children:
        try:
            raw = c.name.encode("latin-1")
        except UnicodeEncodeError:
            fail(f"{where}/{c.name}: XDVDFS names are 8-bit characters")
        if not 0 < len(raw) < 256:
            fail(f"{where}/{c.name}: name too long for XDVDFS")
        if name_key(c.name) in seen:
            fail(f"{where}/{c.name}: two names that differ only in case ({seen[name_key(c.name)]})")
        seen[name_key(c.name)] = c.name
        if isinstance(c, Dir):
            check_tree(c, f"{where}/{c.name}")
        elif c.size > 0xFFFFFFFF:
            fail(f"{where}/{c.name}: files of 4 GB or more do not fit XDVDFS")


def dir_table(d, sector_of):
    """A directory's table: a balanced binary tree of entries in name order, laid out in pre-order; an entry never
    crosses a sector; padding is 0xFF. sector_of(child) -> (sector, size) once known (None: sizes only)."""
    names = sorted(d.children, key=lambda c: name_key(c.name))
    order = []

    def pre(lo, hi):
        if lo >= hi:
            return None
        mid = (lo + hi) // 2
        node = [names[mid], None, None]
        order.append(node)
        node[1], node[2] = pre(lo, mid), pre(mid + 1, hi)
        return node

    pre(0, len(names))
    offs, pos = {}, 0
    for node in order:
        size = (14 + len(node[0].name.encode("latin-1")) + 3) & ~3
        if pos // SECTOR != (pos + size - 1) // SECTOR:
            pos = (pos + SECTOR - 1) & ~(SECTOR - 1)
        offs[id(node)] = pos
        pos += size
    length = ((pos + SECTOR - 1) & ~(SECTOR - 1)) or SECTOR
    if sector_of is None:
        return length
    table = bytearray(b"\xff" * length)
    for node in order:
        child, left, right = node
        sec, size = sector_of(child)
        raw = child.name.encode("latin-1")
        entry = struct.pack("<HHIIBB", offs[id(left)] // 4 if left else 0, offs[id(right)] // 4 if right else 0,
                            sec, size, ATTR_DIR if isinstance(child, Dir) else ATTR_FILE, len(raw)) + raw
        entry += b"\xff" * (((len(entry) + 3) & ~3) - len(entry))
        o = offs[id(node)]
        table[o:o + len(entry)] = entry
    assert len(table) == length
    return bytes(table)


class PartWriter:
    """Writes an image sequentially into one file, or two parts of at most PART_MAX bytes."""

    def __init__(self, paths, split):
        self.paths, self.split = paths, split
        self.index, self.pos, self.f = 0, 0, open(paths[0], "wb")

    def write(self, data):
        mv = memoryview(data)
        while mv:
            room = PART_MAX - self.pos if self.split and self.index == 0 else len(mv)
            if room == 0:
                self.f.close()
                self.index, self.pos = 1, 0
                self.f = open(self.paths[1], "wb")
                continue
            n = min(room, len(mv))
            self.f.write(mv[:n])
            self.pos += n
            mv = mv[n:]

    def close(self):
        self.f.close()


def write_xiso(root, out_base, split):
    """Write the tree as an XISO: out_base + '.iso', or '.1.iso' and '.2.iso' when split and over PART_MAX.
    Returns the paths written."""
    check_tree(root)
    # Sectors: the directory tables first (breadth first), then the files in the same order.
    dirs, files, queue = [], [], [root]
    while queue:
        d = queue.pop(0)
        dirs.append(d)
        for c in sorted(d.children, key=lambda c: name_key(c.name)):
            (queue if isinstance(c, Dir) else files).append(c)
    place, nxt = {}, VD_SECTOR + 1
    for d in dirs:
        length = dir_table(d, None)
        place[id(d)] = (nxt, length)
        nxt += length // SECTOR
    for f in files:
        place[id(f)] = (nxt, f.size)
        nxt += max(1, (f.size + SECTOR - 1) // SECTOR)
    total = nxt * SECTOR
    split = split and total > PART_MAX
    paths = [f"{out_base}.1.iso", f"{out_base}.2.iso"] if split else [f"{out_base}.iso"]
    for p in paths:
        if os.path.exists(p):
            fail(f"{p} exists already")

    vd = bytearray(SECTOR)
    vd[0:20] = MAGIC
    seconds = int(time.time()) + 11644473600  # FILETIME: 100 ns since 1601
    vd[20:36] = struct.pack("<IIQ", *place[id(root)], seconds * 10_000_000)
    vd[SECTOR - 20:] = MAGIC
    w = PartWriter([p + ".part" for p in paths], split)
    done, last = 0, time.time()
    try:
        w.write(bytes(VD_SECTOR * SECTOR))
        w.write(vd)
        for d in dirs:
            w.write(dir_table(d, lambda c: place[id(c)]))
        for f in files:
            written = 0
            for piece in f.chunks():
                w.write(piece)
                written += len(piece)
                done += len(piece)
                if time.time() - last > 5:
                    log(f"  {done / total * 100:5.1f} %")
                    last = time.time()
            if written != f.size:
                fail(f"{f.name}: size changed while reading ({written} of {f.size} bytes)")
            w.write(bytes(max(SECTOR, (f.size + SECTOR - 1) & ~(SECTOR - 1)) - f.size))
        w.close()
    except BaseException:
        w.close()
        for p in paths:
            if os.path.exists(p + ".part"):
                os.remove(p + ".part")
        raise
    for p in paths:
        os.replace(p + ".part", p)
    return paths


def verify_xiso(path, root):
    """Read the written image back: every file and folder of the source tree, same sizes and contents."""
    img = Image(path)
    try:
        def check(d, sector, size, where):
            got = {name_key(n): (n, s, z, a) for n, s, z, a in img.entries(sector, size)}
            if len(got) != len(d.children):
                fail(f"verify {where}/: {len(got)} entries, expected {len(d.children)}")
            for c in d.children:
                e = got.get(name_key(c.name))
                if e is None:
                    fail(f"verify: {where}/{c.name} missing")
                if isinstance(c, Dir):
                    check(c, e[1], e[2], f"{where}/{c.name}")
                else:
                    h1, h2 = hashlib.sha1(), hashlib.sha1()
                    for piece in c.chunks():
                        h1.update(piece)
                    off = 0
                    while off < e[2]:
                        n = min(CHUNK, e[2] - off)
                        h2.update(img.read_at(e[1], off, n))
                        off += n
                    if e[2] != c.size or h1.digest() != h2.digest():
                        fail(f"verify: {where}/{c.name} differs from the source")
        check(root, *img.root, "")
    finally:
        img.close()


def split_image(path):
    """Cut a whole image over PART_MAX into NAME.1.iso + NAME.2.iso in place: the tail is copied into part 2, then
    the file is shortened and renamed to part 1 (no second copy of the whole image)."""
    size = os.path.getsize(path)
    if size <= PART_MAX:
        return [path]
    if size > 2 * PART_MAX:
        fail(f"{path}: too large for two parts")
    base = os.path.splitext(path)[0]
    p1, p2 = f"{base}.1.iso", f"{base}.2.iso"
    for p in (p1, p2):
        if os.path.exists(p):
            fail(f"{p} exists already")
    with open(path, "rb") as src, open(p2 + ".part", "wb") as dst:
        src.seek(PART_MAX)
        shutil.copyfileobj(src, dst, CHUNK)
    if os.path.getsize(p2 + ".part") != size - PART_MAX:
        os.remove(p2 + ".part")
        fail(f"{path}: copying the second part failed")
    os.replace(p2 + ".part", p2)
    with open(path, "r+b") as f:
        f.truncate(PART_MAX)
    os.replace(path, p1)
    return [p1, p2]


# --- Commands ---------------------------------------------------------------------------------------------------------

def load_template(path):
    if not os.path.isfile(path):
        fail(f"launcher template {path} not found (build it: make -C kernel iso-launcher, or pass --launcher)")
    t = Xbe(open(path, "rb").read(), path)
    if not is_launcher(bytes(t.data)):
        fail(f"{path}: not an OXOS ISO launcher")
    if t.section(TITLE_IMAGE):
        fail(f"{path}: the template has a title image already; use the plain ISO launcher")
    return t


def write_launcher(folder, game, template, force, title=None):
    out = os.path.join(folder, "default.xbe")
    if os.path.exists(out) and not force and not is_launcher(open(out, "rb").read()):
        fail(f"{out} exists and is not a launcher made by this tool (--force replaces it)")
    data = make_launcher(template, game, title)
    with open(out + ".part", "wb") as f:
        f.write(data)
    os.replace(out + ".part", out)
    return out


def images_in(folder):
    """The images in a folder, one per game: whole images and first parts."""
    out = []
    for name in sorted(os.listdir(folder)):
        p = os.path.join(folder, name)
        stem, ext = os.path.splitext(name)
        if os.path.isfile(p) and ext.lower() == ".iso" and not stem.endswith(".2"):
            out.append(p)
    return out


def do_image(path, a, template):
    """An existing image: optionally into its own folder and split; then its launcher."""
    path = os.path.abspath(path)
    folder = os.path.dirname(path)
    name = image_name(path)
    others = [p for p in images_in(folder) if image_name(p) != name]
    if others or any(n.lower().endswith(".cci") for n in os.listdir(folder)):
        if not a.own_folder:
            fail(f"{folder} holds more than one game; the launcher starts the image in its own folder, so each game "
                 f"needs a folder of its own (--own-folder moves each image into one)")
    if a.own_folder and (others or os.path.basename(folder) != name):
        dest = os.path.join(folder, name)
        os.makedirs(dest, exist_ok=True)
        for p in image_parts(path):
            target = os.path.join(dest, os.path.basename(p))
            if os.path.exists(target):
                fail(f"{target} exists already")
            os.replace(p, target)
        log(f"{name}: moved into {dest}{os.sep}")
        folder, path = dest, os.path.join(dest, os.path.basename(path))
    if a.split and len(image_parts(path)) == 1:
        parts = split_image(path)
        if len(parts) == 2:
            log(f"{name}: split into {os.path.basename(parts[0])} + {os.path.basename(parts[1])}")
            path = parts[0]
    img = Image(path)
    try:
        game = Xbe(img.read_file("default.xbe"), f"{path}: default.xbe")
        kind = "full dump" if img.base else "XISO"
    finally:
        img.close()
    out = write_launcher(folder, game, template, a.force, a.title)
    log(f"{name}: {kind}, {describe(game)}\n  launcher {out}")


def do_folder(src, a, template):
    """An extracted game: an XISO of it in OUT/<name>/, and its launcher."""
    src = os.path.abspath(src)
    name = a.name or os.path.basename(src.rstrip(os.sep))
    dest = os.path.join(os.path.abspath(a.output), name)
    if os.path.commonpath([dest, src]) == src or os.path.commonpath([dest, src]) == dest:
        fail(f"the output folder {dest} must not be the source folder, inside it, or around it (-o picks another)")
    game = Xbe(open(os.path.join(src, "default.xbe"), "rb").read(), os.path.join(src, "default.xbe"))
    os.makedirs(dest, exist_ok=True)
    log(f"{name}: building the image from {src}")
    tree = tree_from_folder(src)
    paths = write_xiso(tree, os.path.join(dest, name), a.split)
    if a.verify:
        log(f"{name}: verifying")
        verify_xiso(paths[0], tree)
    out = write_launcher(dest, game, template, a.force, a.title)
    size = sum(os.path.getsize(p) for p in paths)
    log(f"{name}: {describe(game)}\n  image {', '.join(paths)} ({size / 2**30:.2f} GB)\n  launcher {out}")


def do_info(path):
    img = Image(path)
    try:
        game = Xbe(img.read_file("default.xbe"), "default.xbe")
        files = sum(1 for e in img.walk() if not e[3] & ATTR_DIR)
        used = max((e[1] * SECTOR + e[2] for e in img.walk()), default=0)
        print(f"{path}: {'full dump' if img.base else 'XISO'} in {len(img.paths)} part(s), {img.size / 2**30:.2f} GB, "
              f"{files} files, data ends at {used / 2**30:.2f} GB\n  {describe(game)}\n"
              f"  title image: {'yes' if game.section(TITLE_IMAGE) else 'no'}")
    finally:
        img.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog="\n\n".join(__doc__.split("\n\n")[1:]))
    ap.add_argument("sources", nargs="+", help="game folders, images, or folders of either")
    ap.add_argument("-o", "--output", default=".", help="where games built from folders go (default: here)")
    ap.add_argument("--split", action="store_true", help="cut images over 4 GB in two (for FATX disks)")
    ap.add_argument("--own-folder", action="store_true", help="move each image into a folder named after it")
    ap.add_argument("--name", help="the game folder's and image's name (one folder source; default: its name)")
    ap.add_argument("--title", help="the title name the launcher shows to dashboards (default: the game's)")
    ap.add_argument("--verify", action="store_true", help="read a built image back and compare every file")
    ap.add_argument("--force", action="store_true", help="replace a default.xbe that is not one of our launchers")
    ap.add_argument("--launcher", default=LAUNCHER_DEFAULT, help="the ISO launcher XBE to use as the template")
    ap.add_argument("--info", action="store_true", help="only print what each image holds")
    a = ap.parse_args()

    if a.info:
        for s in a.sources:
            for p in ([s] if os.path.isfile(s) else images_in(s)):
                do_info(p)
        return
    template = load_template(a.launcher)
    for s in a.sources:
        if os.path.isfile(s):
            do_image(s, a, template)
        elif os.path.isdir(s):
            imgs = images_in(s)
            if imgs:
                for p in imgs:
                    do_image(p, a, template)
            elif os.path.isfile(os.path.join(s, "default.xbe")):
                do_folder(s, a, template)
            else:
                subs = [e.path for e in sorted(os.scandir(s), key=lambda e: e.name) if e.is_dir()]
                done = 0
                for sub in subs:
                    if images_in(sub):
                        for p in images_in(sub):
                            do_image(p, a, template)
                        done += 1
                if not done:
                    fail(f"{s}: no images, no default.xbe, and no game folders in it")
        else:
            fail(f"{s}: not found")


if __name__ == "__main__":
    main()
