// mkcci: convert an Xbox disc image (.iso, XISO or full dump) to CCI, the LZ4-compressed format OXOS and Cerbios
// mount (spec/cerbios/07 §4, spec/oxos-internal/16-virtual-discs.md).
//
//   mkcci IN.iso OUT [--split] [--no-verify]
//
// OUT names the image without its extension. One part is written as OUT.cci. When the CCI would not fit in one FATX
// file (4 GB), or with --split, the image is written as two parts, OUT.1.cci and OUT.2.cci. --split cuts at the middle
// sector. Each part is a whole CCI file for its own run of sectors, and part 1 holds the first ones. Afterwards every
// part is read back, decoded and compared with the input, unless --no-verify is given.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t BLOCK = 0x800;
constexpr std::uint64_t PART_LIMIT = 0xFFFFFFFFull; // a FATX file, and the reach of index entries (offset / 4, 30 bits)

// --- LZ4 block format, greedy: sequences of (token, literals, offset, match); the last 5 bytes are literals and no
// match starts in the last 12.
std::size_t Lz4Encode(const std::uint8_t *in, std::size_t n, std::uint8_t *out) {
    std::array<std::int32_t, 4096> table;
    table.fill(-1);
    std::size_t o = 0, anchor = 0, i = 0;
    auto lengths = [&](std::size_t v) {
        for (; v >= 255; v -= 255)
            out[o++] = 255;
        out[o++] = std::uint8_t(v);
    };
    auto hash = [&](std::size_t at) {
        std::uint32_t v;
        std::memcpy(&v, in + at, 4);
        return (v * 2654435761u) >> 20;
    };
    const std::size_t limit = n > 12 ? n - 12 : 0;
    while (i < limit) {
        std::uint32_t h = hash(i);
        std::int32_t j = table[h];
        table[h] = std::int32_t(i);
        if (j < 0 || std::memcmp(in + j, in + i, 4)) {
            i++;
            continue;
        }
        std::size_t m = 4;
        while (i + m < n - 5 && in[j + m] == in[i + m])
            m++;
        std::size_t lit = i - anchor;
        out[o++] = std::uint8_t((std::min<std::size_t>(lit, 15) << 4) | std::min<std::size_t>(m - 4, 15));
        if (lit >= 15)
            lengths(lit - 15);
        std::memcpy(out + o, in + anchor, lit);
        o += lit;
        std::size_t distance = i - std::size_t(j);
        out[o++] = std::uint8_t(distance);
        out[o++] = std::uint8_t(distance >> 8);
        if (m - 4 >= 15)
            lengths(m - 4 - 15);
        i += m;
        anchor = i;
    }
    std::size_t lit = n - anchor;
    out[o++] = std::uint8_t(std::min<std::size_t>(lit, 15) << 4);
    if (lit >= 15)
        lengths(lit - 15);
    std::memcpy(out + o, in + anchor, lit);
    return o + lit;
}

// The kernel's decoder, for the check.
bool Lz4Decode(const std::uint8_t *src, std::size_t len, std::uint8_t *dst, std::size_t cap) {
    std::size_t ip = 0, op = 0;
    while (ip < len) {
        std::uint8_t token = src[ip++];
        std::size_t n = token >> 4;
        if (n == 15)
            for (std::uint8_t b = 255; b == 255;) {
                if (ip >= len)
                    return false;
                n += b = src[ip++];
            }
        if (n > len - ip || n > cap - op)
            return false;
        std::memcpy(dst + op, src + ip, n);
        ip += n, op += n;
        if (ip == len)
            break;
        if (len - ip < 2)
            return false;
        std::size_t distance = src[ip] | std::size_t(src[ip + 1]) << 8;
        ip += 2;
        n = (token & 15) + 4;
        if ((token & 15) == 15)
            for (std::uint8_t b = 255; b == 255;) {
                if (ip >= len)
                    return false;
                n += b = src[ip++];
            }
        if (!distance || distance > op || n > cap - op)
            return false;
        for (std::size_t k = 0; k < n; k++, op++)
            dst[op] = dst[op - distance];
    }
    return op == cap;
}

struct File {
    std::FILE *f = nullptr;
    explicit File(const std::string &path, const char *mode) : f(std::fopen(path.c_str(), mode)) {}
    ~File() {
        if (f)
            std::fclose(f);
    }
};

std::uint64_t Size(std::FILE *f) {
    std::fseek(f, 0, SEEK_END);
    long long n = ftello(f);
    std::fseek(f, 0, SEEK_SET);
    return std::uint64_t(n);
}

// The header: magic, header size, image size, index offset, block size, version 1, index alignment 2 (offset / 4).
void Header(std::uint8_t *h, std::uint64_t image, std::uint64_t index) {
    std::memset(h, 0, 32);
    std::memcpy(h, "CCIM", 4);
    std::uint32_t v32 = 0x20;
    std::memcpy(h + 4, &v32, 4);
    std::memcpy(h + 8, &image, 8);
    std::memcpy(h + 16, &index, 8);
    v32 = BLOCK;
    std::memcpy(h + 24, &v32, 4);
    h[28] = 1;
    h[29] = 2;
}

// One block as it is stored: [pad][LZ4][pad bytes] (a multiple of 4, under 2048 bytes) or the 2048 raw bytes.
std::size_t Store(const std::uint8_t *raw, std::uint8_t *out, bool *packed) {
    static std::uint8_t lz[BLOCK + BLOCK / 255 + 16];
    std::size_t n = Lz4Encode(raw, BLOCK, lz);
    std::size_t pad = (4 - (1 + n) % 4) % 4;
    if (1 + n + pad < BLOCK) {
        out[0] = std::uint8_t(pad);
        std::memcpy(out + 1, lz, n);
        std::memset(out + 1 + n, 0, pad);
        *packed = true;
        return 1 + n + pad;
    }
    std::memcpy(out, raw, BLOCK);
    *packed = false;
    return BLOCK;
}

// Writes sectors [first, first + count) of the input as one CCI file. Stops early (returns the sectors written) when
// the next block would take the file past PART_LIMIT.
std::uint64_t WritePart(std::FILE *in, std::uint64_t first, std::uint64_t count, const std::string &path) {
    File out(path, "wb");
    if (!out.f) {
        std::fprintf(stderr, "mkcci: cannot write %s\n", path.c_str());
        return 0;
    }
    std::uint8_t header[32] = {};
    std::fwrite(header, 1, 32, out.f); // written again at the end
    std::vector<std::uint32_t> index;
    index.reserve(count + 1);
    std::uint64_t at = 32;
    std::vector<std::uint8_t> raw(BLOCK * 512);
    std::uint8_t stored[BLOCK];
    fseeko(in, (long long)(first * BLOCK), SEEK_SET);
    std::uint64_t done = 0;
    std::uint64_t compressed = 0;
    while (done < count) {
        std::size_t want = std::size_t(std::min<std::uint64_t>(512, count - done));
        std::memset(raw.data(), 0, want * BLOCK);
        if (std::fread(raw.data(), 1, want * BLOCK, in) < want * BLOCK) {
        } // a short last sector is padded with zeros
        std::size_t k = 0;
        for (; k < want; k++) {
            bool packed;
            std::size_t n = Store(raw.data() + k * BLOCK, stored, &packed);
            // Room for this block, the index so far, its next entry and the end entry.
            if (at + n + (index.size() + 2) * 4 > PART_LIMIT)
                break;
            index.push_back(std::uint32_t(at >> 2) | (packed ? 0x80000000u : 0));
            std::fwrite(stored, 1, n, out.f);
            at += n;
            compressed += packed;
        }
        done += k;
        if (k < want)
            break;
        if (done % (512 * 64) == 0)
            std::fprintf(stderr, "\r  %s: %llu / %llu MB", path.c_str(), (unsigned long long)(done >> 9),
                         (unsigned long long)(count >> 9));
    }
    index.push_back(std::uint32_t(at >> 2));
    std::fwrite(index.data(), 4, index.size(), out.f);
    Header(header, done * BLOCK, at);
    std::fseek(out.f, 0, SEEK_SET);
    std::fwrite(header, 1, 32, out.f);
    std::fprintf(stderr, "\r  %s: %llu sectors (%llu compressed), %llu MB\n", path.c_str(), (unsigned long long)done,
                 (unsigned long long)compressed, (unsigned long long)((at + index.size() * 4) >> 20));
    return done;
}

// Decodes every block of a part and compares it with sectors [first, ...) of the input.
bool Verify(std::FILE *in, std::uint64_t first, const std::string &path, std::uint64_t *sectors) {
    File part(path, "rb");
    std::uint8_t h[32];
    if (!part.f || std::fread(h, 1, 32, part.f) != 32 || std::memcmp(h, "CCIM", 4)) {
        std::fprintf(stderr, "mkcci: %s: not a CCI file\n", path.c_str());
        return false;
    }
    std::uint64_t image, index;
    std::memcpy(&image, h + 8, 8);
    std::memcpy(&index, h + 16, 8);
    std::uint64_t blocks = image / BLOCK;
    std::vector<std::uint32_t> entries(blocks + 1);
    fseeko(part.f, (long long)index, SEEK_SET);
    if (std::fread(entries.data(), 4, entries.size(), part.f) != entries.size())
        return false;
    fseeko(part.f, 32, SEEK_SET);
    fseeko(in, (long long)(first * BLOCK), SEEK_SET);
    std::uint8_t stored[2 * BLOCK], got[BLOCK], want[BLOCK];
    for (std::uint64_t b = 0; b < blocks; b++) {
        std::uint64_t at = std::uint64_t(entries[b] << 2), next = std::uint64_t(entries[b + 1] << 2);
        std::size_t n = std::size_t(next - at);
        bool ok = next > at && n <= 2 * BLOCK && ftello(part.f) == (long long)at &&
                  std::fread(stored, 1, n, part.f) == n;
        if (ok && n == BLOCK)
            std::memcpy(got, stored, BLOCK);
        else if (ok)
            ok = stored[0] + 1u <= n && Lz4Decode(stored + 1, n - 1 - stored[0], got, BLOCK);
        std::memset(want, 0, BLOCK);
        if (std::fread(want, 1, BLOCK, in) < BLOCK) {
        } // a short last sector: zeros
        if (!ok || std::memcmp(got, want, BLOCK)) {
            std::fprintf(stderr, "mkcci: %s: block %llu differs from the input\n", path.c_str(),
                         (unsigned long long)b);
            return false;
        }
    }
    *sectors = blocks;
    std::fprintf(stderr, "  %s: checked, %llu sectors match\n", path.c_str(), (unsigned long long)blocks);
    return true;
}

} // namespace

int main(int argc, char **argv) {
    std::vector<std::string> names;
    bool split = false, verify = true, halves = false;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--split"))
            split = halves = true;
        else if (!std::strcmp(argv[i], "--no-verify"))
            verify = false;
        else
            names.emplace_back(argv[i]);
    }
    if (names.size() != 2) {
        std::fprintf(stderr, "usage: mkcci IN.iso OUT [--split] [--no-verify]  (writes OUT.cci or OUT.1.cci + OUT.2.cci)\n");
        return 2;
    }
    File in(names[0], "rb");
    if (!in.f) {
        std::fprintf(stderr, "mkcci: cannot read %s\n", names[0].c_str());
        return 1;
    }
    std::uint64_t sectors = (Size(in.f) + BLOCK - 1) / BLOCK;
    std::string out = names[1];
    if (out.size() > 4 && out.compare(out.size() - 4, 4, ".cci") == 0)
        out.resize(out.size() - 4);

    std::vector<std::string> parts;
    std::uint64_t first = 0;
    if (!split) {
        std::uint64_t n = WritePart(in.f, 0, sectors, out + ".cci");
        if (n == sectors) {
            parts.push_back(out + ".cci");
        } else { // too big for one file: the rest goes in a second part
            std::remove((out + ".cci").c_str());
            split = true;
        }
    }
    if (split) {
        std::uint64_t half = halves ? sectors / 2 : sectors; // else part 1 takes all it can
        std::uint64_t n = WritePart(in.f, 0, half, out + ".1.cci");
        if (n != half && n < sectors / 2) {
            std::fprintf(stderr, "mkcci: the first part filled up\n");
            return 1;
        }
        first = n;
        if (WritePart(in.f, first, sectors - first, out + ".2.cci") != sectors - first) {
            std::fprintf(stderr, "mkcci: the image does not fit in two parts\n");
            return 1;
        }
        parts = {out + ".1.cci", out + ".2.cci"};
    }
    if (verify) {
        std::uint64_t at = 0, n = 0;
        for (const auto &p : parts) {
            if (!Verify(in.f, at, p, &n))
                return 1;
            at += n;
        }
        if (at != sectors) {
            std::fprintf(stderr, "mkcci: the parts hold %llu of %llu sectors\n", (unsigned long long)at,
                         (unsigned long long)sectors);
            return 1;
        }
    }
    return 0;
}
