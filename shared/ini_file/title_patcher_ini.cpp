// Title patch file parser (spec/oxos_internal/25 §3).
//
//   ; comment
//   [4D4A000C]            every version of title 4D4A000C
//   [56550039/3]          only certificate version 3 (decimal, or 0x hex)
//   [*]                   every title: its patches are written only where their current (or match) bytes are there
//   name = what it fixes
//   patch = 506D7: E8 D4 64 00 > EB 08   CURRENT_BYTES > NEW_BYTES: when these bytes are at this address of the
//                                        loaded title, write those
//   match = 1C930D: 8B F0 56 E8          when these bytes are at this address, the blind patches below it in the
//                                        section (no CURRENT_BYTES: "patch = ADDR: > NEW_BYTES") are written
#include <ini_file/title_patcher_ini.h>
#include <ini_file/ini_file.h>

namespace oxos::ini_file {

namespace {
bool IsBlank(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

int HexDigit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

struct Cursor {
    const char *p, *end;
    void SkipBlanks() {
        while (p < end && IsBlank(*p))
            p++;
    }
    bool AtEnd() { // the rest is blank or a comment
        SkipBlanks();
        return p == end || *p == ';' || *p == '#';
    }
    // A number: hex (Hex = true, an optional 0x), or decimal unless it starts with 0x. At most 8 hex digits.
    bool Number(uint32_t *v, bool Hex) {
        SkipBlanks();
        if (end - p > 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
            p += 2;
            Hex = true;
        }
        uint32_t value = 0, digits = 0;
        for (; p < end; p++, digits++) {
            int d = HexDigit(*p);
            if (d < 0 || (!Hex && d > 9))
                break;
            if (Hex ? digits == 8 : value > 429496729 || (value == 429496729 && d > 5))
                return false;
            value = Hex ? value << 4 | d : value * 10 + d;
        }
        *v = value;
        return digits > 0;
    }
    // Hex byte pairs, spaces between them optional, up to the end of the line, a comment or '>'. At least one byte, at most INI_PATCH_MAX_BYTES.
    bool HexBytes(uint8_t *Out, uint32_t *Count) {
        uint32_t n = 0;
        for (SkipBlanks(); p < end && *p != '>' && *p != ';' && *p != '#'; SkipBlanks()) {
            int hi = HexDigit(*p), lo = p + 1 < end ? HexDigit(p[1]) : -1;
            if (hi < 0 || lo < 0 || n == INI_PATCH_MAX_BYTES)
                return false;
            Out[n++] = (uint8_t)(hi << 4 | lo);
            p += 2;
        }
        *Count = n;
        return n > 0;
    }
    bool Expect(char c) {
        SkipBlanks();
        if (p == end || *p != c)
            return false;
        p++;
        return true;
    }
};

// Room for a record of `length` bytes: its address, or null (and an error counted) when it does not fit.
uint8_t *Reserve(uint8_t *Out, uint32_t OutSize, uint32_t *used, uint32_t length, INI_PATCH_COUNTS *c) {
    uint32_t size = (sizeof(INI_PATCH_RECORD) + length + 3) & ~3u;
    if (OutSize - *used < size) {
        c->Errors++;
        return nullptr;
    }
    uint8_t *r = Out + *used;
    *used += size;
    return r;
}
} // namespace

uint32_t ParseTitlePatches(const char *Text, uint32_t Length, uint32_t TitleId, uint32_t Version, uint8_t *Out, uint32_t OutSize, INI_PATCH_COUNTS *Counts) {
    INI_PATCH_COUNTS c = {};
    uint32_t used = 0;
    bool active = false;
    uint32_t matchAddress = 0, matchLength = 0; // the current section's `match =` line
    uint8_t match[INI_PATCH_MAX_BYTES];
    INI_READER reader = {Text, Text + Length};
    INI_LINE line;
    while (NextLine(&reader, &line)) {
        if (line.Kind == INI_LINE_KIND::BAD_LINE) {
            c.Errors++;
            continue;
        }
        if (line.Kind != INI_LINE_KIND::KEY) { // [TITLEID], [TITLEID/VERSION] or [*]
            matchLength = 0;
            active = false;
            if (line.Kind == INI_LINE_KIND::SECTION && line.TextLength == 1 && line.Text[0] == '*') {
                active = true; // a launcher that carries the title ID of the game it starts: its code bytes identify it
                c.Sections++;
                continue;
            }
            Cursor cur = {line.Text, line.Text + line.TextLength};
            uint32_t id, version = 0;
            bool ok = line.Kind == INI_LINE_KIND::SECTION && cur.Number(&id, true);
            bool versioned = false;
            if (ok && cur.Expect('/')) {
                versioned = true;
                ok = cur.Number(&version, false);
            }
            if (!(ok && cur.AtEnd())) {
                c.Errors++;
                continue;
            }
            active = id == TitleId && (!versioned || version == Version);
            c.Sections += active;
            continue;
        }
        Cursor cur = {line.Value, line.Value + line.ValueLength};
        if (IsKey(line, "name")) {
            if (!active)
                continue;
            uint32_t n = line.ValueLength;
            if (n > INI_PATCH_MAX_NAME)
                n = INI_PATCH_MAX_NAME;
            if (uint8_t *r = Reserve(Out, OutSize, &used, n, &c)) {
                auto h = (INI_PATCH_RECORD *)r;
                h->Address = 0;
                h->Length = (uint8_t)n;
                h->OriginalLength = 0;
                h->Kind = INI_PATCH_NAME;
                h->Check = 0;
                for (uint32_t i = 0; i < n; i++)
                    r[sizeof(*h) + i] = (uint8_t)line.Value[i];
            }
        } else if (IsKey(line, "match")) {
            // ADDRESS: CURRENT_BYTES: when they are there, the blind patches below it in the section are written
            uint32_t address, n = 0;
            uint8_t bytes[INI_PATCH_MAX_BYTES];
            if (!(cur.Number(&address, true) && address && cur.Expect(':') && cur.HexBytes(bytes, &n) && cur.AtEnd())) {
                c.Errors++;
                continue;
            }
            matchAddress = address;
            matchLength = n;
            for (uint32_t i = 0; i < n; i++)
                match[i] = bytes[i];
        } else if (IsKey(line, "patch")) {
            // ADDRESS: CURRENT_BYTES > NEW_BYTES (hex bytes; spaces between bytes are optional; both sides any length), or a blind patch ADDRESS: > NEW_BYTES,
            // written when the section's `match =` bytes are there
            uint32_t address;
            uint8_t original[INI_PATCH_MAX_BYTES], bytes[INI_PATCH_MAX_BYTES];
            uint32_t on = 0, n = 0;
            bool ok = cur.Number(&address, true) && cur.Expect(':');
            cur.SkipBlanks();
            bool byMatch = ok && cur.p < cur.end && *cur.p == '>';
            ok = ok && (byMatch || cur.HexBytes(original, &on)) && cur.Expect('>') && cur.HexBytes(bytes, &n) && cur.AtEnd();
            if (ok && byMatch && !matchLength)
                ok = false; // a blind patch without a `match =` line above it in this section
            if (!ok) {
                c.Errors++;
                continue;
            }
            if (!active)
                continue;
            const uint8_t *check = byMatch ? match : original;
            if (byMatch)
                on = matchLength;
            if (uint8_t *r = Reserve(Out, OutSize, &used, on + n, &c)) {
                auto h = (INI_PATCH_RECORD *)r;
                h->Address = address;
                h->Length = (uint8_t)n;
                h->OriginalLength = (uint8_t)on;
                h->Kind = INI_PATCH_BYTES;
                h->Check = byMatch ? matchAddress : 0;
                for (uint32_t i = 0; i < on; i++)
                    r[sizeof(*h) + i] = check[i];
                for (uint32_t i = 0; i < n; i++)
                    r[sizeof(*h) + on + i] = bytes[i];
                c.Patches++;
            }
        }
        // Other keys are ignored, so later versions of the format can add some.
    }
    if (Counts)
        *Counts = c;
    return used;
}

} // namespace oxos::ini_file
