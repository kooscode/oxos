#!/usr/bin/env python3
"""LZX compressor (and a small decoder for self-checks) for the 2BL's kernel stream.

Format: Microsoft LZX as used in CAB files, window 2^17, 32 KiB frames (spec/00, spec/01). Every frame is one
*verbatim* block; the stream state (tree lengths, window, repeated offsets) carries over between frames, the bit
stream is realigned to 16 bits at each frame end, and no match crosses a frame end. Matches are found with a
hash chain (greedy plus one step of lazy evaluation). Repeated-offset slots are never emitted.

Bit order: 16-bit little-endian words, bits taken MSB first.
"""
import heapq, struct

FRAME = 0x8000
WINDOW_BITS = 17
WINDOW = 1 << WINDOW_BITS
NUM_CHARS = 256
MIN_MATCH, MAX_MATCH = 2, 257
NUM_PRIMARY_LENGTHS = 7
NUM_SECONDARY_LENGTHS = 249
PRETREE_SIZE = 20

# Position slots: extra bits and base (formatted offset = real offset + 2).
EXTRA_BITS, POSITION_BASE = [], []
_base = 0
for _slot in range(51):
    _extra = 0 if _slot < 4 else min((_slot - 2) // 2, 17)
    EXTRA_BITS.append(_extra)
    POSITION_BASE.append(_base)
    _base += 1 << _extra
NUM_POSITION_SLOTS = {15: 30, 16: 32, 17: 34, 18: 36, 19: 38, 20: 42, 21: 50}[WINDOW_BITS]
MAIN_TREE_SIZE = NUM_CHARS + NUM_POSITION_SLOTS * 8


class BitWriter:
    def __init__(self):
        self.out, self.acc, self.n = bytearray(), 0, 0

    def put(self, value, bits):
        if bits == 0:
            return
        self.acc = (self.acc << bits) | (value & ((1 << bits) - 1))
        self.n += bits
        while self.n >= 16:
            self.n -= 16
            self.out += struct.pack("<H", (self.acc >> self.n) & 0xFFFF)
        self.acc &= (1 << self.n) - 1

    def align16(self):
        if self.n:
            self.put(0, 16 - self.n)


class BitReader:
    def __init__(self, data):
        self.data, self.pos, self.acc, self.n = data, 0, 0, 0

    def get(self, bits):
        while self.n < bits:
            w = struct.unpack_from("<H", self.data, self.pos)[0] if self.pos + 2 <= len(self.data) else 0
            self.pos += 2
            self.acc = (self.acc << 16) | w
            self.n += 16
        self.n -= bits
        v = (self.acc >> self.n) & ((1 << bits) - 1)
        self.acc &= (1 << self.n) - 1
        return v

    def align16(self):
        self.n -= self.n % 16
        self.acc &= (1 << self.n) - 1


# --- Huffman codes

def code_lengths(freqs, limit=16):
    """Huffman code lengths, at most `limit` bits; at least two symbols always get a code."""
    freqs = list(freqs)
    used = [i for i, f in enumerate(freqs) if f]
    for i in range(len(freqs)):
        if len(used) >= 2:
            break
        if not freqs[i]:
            freqs[i] = 1
            used.append(i)
    while True:
        heap = [(f, i, (i,)) for i, f in enumerate(freqs) if f]
        heapq.heapify(heap)
        lengths = [0] * len(freqs)
        tie = len(freqs)
        while len(heap) > 1:
            f1, _, s1 = heapq.heappop(heap)
            f2, _, s2 = heapq.heappop(heap)
            for s in s1 + s2:
                lengths[s] += 1
            heapq.heappush(heap, (f1 + f2, tie, s1 + s2))
            tie += 1
        if max(lengths) <= limit:
            return lengths
        freqs = [(f >> 1) | 1 if f else 0 for f in freqs]


def canonical_codes(lengths):
    codes, code = [0] * len(lengths), 0
    for bits in range(1, 17):
        for sym, n in enumerate(lengths):
            if n == bits:
                codes[sym] = code
                code += 1
        code <<= 1
    return codes


def decode_table(lengths):
    """{(length, code): symbol} for the reader."""
    codes = canonical_codes(lengths)
    return {(n, codes[s]): s for s, n in enumerate(lengths) if n}


def read_symbol(br, table):
    code = 0
    for n in range(1, 17):
        code = (code << 1) | br.get(1)
        if (n, code) in table:
            return table[(n, code)]
    raise ValueError("bad Huffman code")


# --- Tree transmission (pretree + run-length codes, delta against the previous lengths)

def write_lens(bw, prev, new):
    items = []  # (pretree symbol, extra value, extra bits)
    i, n = 0, len(new)
    while i < n:
        run = 1
        while i + run < n and new[i + run] == new[i]:
            run += 1
        if new[i] == 0 and run >= 4:
            take = min(run, 51)
            if take >= 20:
                items.append((18, take - 20, 5))
            else:
                items.append((17, take - 4, 4))
            i += take
            continue
        if run >= 4 and all(prev[j] == prev[i] for j in range(i, i + min(run, 5))):
            take = min(run, 5)
            items.append((19, take - 4, 1))
            items.append(((prev[i] - new[i] + 17) % 17, 0, 0))
            i += take
            continue
        items.append(((prev[i] - new[i] + 17) % 17, 0, 0))
        i += 1
    freqs = [0] * PRETREE_SIZE
    for sym, _, _ in items:
        freqs[sym] += 1
    plens = code_lengths(freqs, 15)
    pcodes = canonical_codes(plens)
    for l in plens:
        bw.put(l, 4)
    for sym, extra, bits in items:
        bw.put(pcodes[sym], plens[sym])
        bw.put(extra, bits)


def read_lens(br, lens, first, last):
    plens = [br.get(4) for _ in range(PRETREE_SIZE)]
    table = decode_table(plens)
    x = first
    while x < last:
        z = read_symbol(br, table)
        if z == 17:
            for _ in range(br.get(4) + 4):
                lens[x] = 0
                x += 1
        elif z == 18:
            for _ in range(br.get(5) + 20):
                lens[x] = 0
                x += 1
        elif z == 19:
            run = br.get(1) + 4
            z = read_symbol(br, table)
            v = (lens[x] - z + 17) % 17
            for _ in range(run):
                lens[x] = v
                x += 1
        else:
            lens[x] = (lens[x] - z + 17) % 17
            x += 1


# --- Match finding

def find_tokens(data, start, end):
    """Tokens for data[start:end]: ints (literals) or (length, distance). Matches stay inside the frame."""
    tokens = []
    head, prev = {}, {}

    def insert(p):
        if p + 3 <= len(data):
            key = data[p:p + 3]
            prev[p] = head.get(key)
            head[key] = p

    # Prime the dictionary with the window before this frame.
    for p in range(max(0, start - (WINDOW - 3)), start):
        insert(p)

    def longest(p):
        best_len, best_dist = 0, 0
        limit = min(MAX_MATCH, end - p)
        if limit < 3:
            return 0, 0
        cand, depth = head.get(data[p:p + 3]), 0
        while cand is not None and depth < 48:
            dist = p - cand
            if dist > WINDOW - 3:
                break
            n = 3
            while n < limit and data[cand + n] == data[p + n]:
                n += 1
            if n > best_len:
                best_len, best_dist = n, dist
                if n == limit:
                    break
            cand, depth = prev.get(cand), depth + 1
        return best_len, best_dist

    p = start
    while p < end:
        n, d = longest(p)
        if n >= 3 and p + 1 < end:
            n2, d2 = longest(p + 1)
            if n2 > n + 1:
                tokens.append(data[p])
                insert(p)
                p += 1
                n, d = n2, d2
        if n >= 3:
            tokens.append((n, d))
            for q in range(p, p + n):
                insert(q)
            p += n
        else:
            tokens.append(data[p])
            insert(p)
            p += 1
    return tokens


def position_slot(formatted):
    lo, hi = 0, NUM_POSITION_SLOTS - 1
    while lo < hi:  # last slot whose base <= formatted
        mid = (lo + hi + 1) // 2
        if POSITION_BASE[mid] <= formatted:
            lo = mid
        else:
            hi = mid - 1
    return lo


# --- Compressor

class Compressor:
    def __init__(self):
        self.main_lens = [0] * MAIN_TREE_SIZE
        self.len_lens = [0] * NUM_SECONDARY_LENGTHS

    def frame(self, data, start, end, first):
        bw = BitWriter()
        if first:
            bw.put(0, 1)  # no Intel E8 translation
        symbols = []  # (main symbol, length footer or None, extra value, extra bits)
        for t in find_tokens(data, start, end):
            if isinstance(t, int):
                symbols.append((t, None, 0, 0))
                continue
            n, d = t
            formatted = d + 2
            slot = position_slot(formatted)
            ml = n - MIN_MATCH
            header = min(ml, NUM_PRIMARY_LENGTHS)
            footer = ml - NUM_PRIMARY_LENGTHS if header == NUM_PRIMARY_LENGTHS else None
            symbols.append((NUM_CHARS + slot * 8 + header, footer, formatted - POSITION_BASE[slot], EXTRA_BITS[slot]))
        main_f, len_f = [0] * MAIN_TREE_SIZE, [0] * NUM_SECONDARY_LENGTHS
        for m, footer, _, _ in symbols:
            main_f[m] += 1
            if footer is not None:
                len_f[footer] += 1
        main_lens, len_lens = code_lengths(main_f), code_lengths(len_f)
        bw.put(1, 3)              # verbatim block
        bw.put(end - start, 24)   # uncompressed size
        write_lens(bw, self.main_lens[:NUM_CHARS], main_lens[:NUM_CHARS])
        write_lens(bw, self.main_lens[NUM_CHARS:], main_lens[NUM_CHARS:])
        write_lens(bw, self.len_lens, len_lens)
        self.main_lens, self.len_lens = main_lens, len_lens
        mc, lc = canonical_codes(main_lens), canonical_codes(len_lens)
        for m, footer, extra, bits in symbols:
            bw.put(mc[m], main_lens[m])
            if footer is not None:
                bw.put(lc[footer], len_lens[footer])
            bw.put(extra, bits)
        bw.align16()
        return bytes(bw.out)


def compress_frames(data):
    """[(compressed frame, uncompressed size)] for consecutive 32 KiB frames."""
    c = Compressor()
    return [(c.frame(data, off, min(off + FRAME, len(data)), off == 0), min(FRAME, len(data) - off))
            for off in range(0, len(data), FRAME)]


# --- Decoder (verbatim and uncompressed blocks; enough to check our own output)

def decompress_frames(frames):
    out = bytearray()
    main_lens, len_lens = [0] * MAIN_TREE_SIZE, [0] * NUM_SECONDARY_LENGTHS
    r = [1, 1, 1]
    block_type = block_left = 0
    main_t = len_t = None
    first = True
    for comp, size in frames:
        br = BitReader(comp)
        if first:
            if br.get(1):
                raise ValueError("E8 translation not supported")
            first = False
        frame_end = len(out) + size
        while len(out) < frame_end:
            if block_left == 0:
                block_type, block_left = br.get(3), br.get(24)
                if block_type == 1:
                    read_lens(br, main_lens, 0, NUM_CHARS)
                    read_lens(br, main_lens, NUM_CHARS, MAIN_TREE_SIZE)
                    read_lens(br, len_lens, 0, NUM_SECONDARY_LENGTHS)
                    main_t, len_t = decode_table(main_lens), decode_table(len_lens)
                else:
                    raise ValueError(f"block type {block_type} not supported")
            run = min(block_left, frame_end - len(out))
            stop = len(out) + run
            while len(out) < stop:
                m = read_symbol(br, main_t)
                if m < NUM_CHARS:
                    out.append(m)
                    continue
                m -= NUM_CHARS
                n = m & 7
                if n == 7:
                    n += read_symbol(br, len_t)
                n += MIN_MATCH
                slot = m >> 3
                if slot < 3:
                    d = r[slot]
                    r[0], r[slot] = r[slot], r[0]
                else:
                    d = POSITION_BASE[slot] - 2 + br.get(EXTRA_BITS[slot])
                    r = [d, r[0], r[1]]
                for _ in range(n):
                    out.append(out[-d])
            block_left -= run
        br.align16()
    return bytes(out)


if __name__ == "__main__":
    import random, sys, time
    samples = [b"", b"a", b"abcabcabcabc" * 5000, random.Random(1).randbytes(70000)]
    if len(sys.argv) > 1:
        samples.append(open(sys.argv[1], "rb").read())
    for s in samples:
        t = time.time()
        frames = compress_frames(s)
        total = sum(len(f) for f, _ in frames)
        ok = decompress_frames(frames) == s
        print(f"{len(s):#x} -> {total:#x} in {time.time() - t:.1f}s: {'ok' if ok else 'MISMATCH'}")
        if not ok:
            sys.exit(1)
