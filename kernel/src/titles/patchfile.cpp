// Title patch file parser (spec/oxos_internal/25 §3).
//
//   ; comment
//   [4D4A000C]            every version of title 4D4A000C
//   [56550039/3]          only certificate version 3 (decimal, or 0x hex)
//   name = what it fixes
//   patch = 506D7: E8 D4 64 00 > EB 08   CURRENT_BYTES > NEW_BYTES: when these bytes are at this address of the
//                                        loaded title, write those
//   match = 1C930D: 8B F0 56 E8          when these bytes are at this address, the blind patches below it in the
//                                        section (no CURRENT_BYTES: "patch = ADDR: > NEW_BYTES") are written
#include <xk/title_patches.h>
#include <xk/ini_file.h>

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
    bool Number(ULONG *v, bool Hex) {
        SkipBlanks();
        if (end - p > 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
            p += 2;
            Hex = true;
        }
        ULONG value = 0, digits = 0;
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
    // Hex byte pairs, spaces between them optional, up to the end of the line, a comment or '>'. At least one byte,
    // at most XE_PATCH_MAX_BYTES.
    bool HexBytes(UCHAR *Out, ULONG *Count) {
        ULONG n = 0;
        for (SkipBlanks(); p < end && *p != '>' && *p != ';' && *p != '#'; SkipBlanks()) {
            int hi = HexDigit(*p), lo = p + 1 < end ? HexDigit(p[1]) : -1;
            if (hi < 0 || lo < 0 || n == XE_PATCH_MAX_BYTES)
                return false;
            Out[n++] = (UCHAR)(hi << 4 | lo);
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
PUCHAR Reserve(PUCHAR Out, ULONG OutSize, ULONG *used, ULONG length, XE_PATCH_COUNTS *c) {
    ULONG size = (sizeof(XE_PATCH_RECORD) + length + 3) & ~3u;
    if (OutSize - *used < size) {
        c->Errors++;
        return nullptr;
    }
    PUCHAR r = Out + *used;
    *used += size;
    return r;
}
} // namespace

ULONG XePatchParse(const char *Text, ULONG Length, ULONG TitleId, ULONG Version, PUCHAR Out, ULONG OutSize,
                   XE_PATCH_COUNTS *Counts) {
    XE_PATCH_COUNTS c = {};
    ULONG used = 0;
    bool active = false;
    ULONG matchAddress = 0, matchLength = 0; // the current section's `match =` line
    UCHAR match[XE_PATCH_MAX_BYTES];
    INI_READER reader = {Text, Text + Length};
    INI_LINE line;
    while (IniNextLine(&reader, &line)) {
        if (line.Kind == INI_LINE_KIND::BAD_LINE) {
            c.Errors++;
            continue;
        }
        if (line.Kind != INI_LINE_KIND::KEY) { // [TITLEID] or [TITLEID/VERSION]
            matchLength = 0;
            active = false;
            Cursor cur = {line.Text, line.Text + line.TextLength};
            ULONG id, version = 0;
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
        if (IniKeyIs(line, "name")) {
            if (!active)
                continue;
            ULONG n = line.ValueLength;
            if (n > XE_PATCH_MAX_NAME)
                n = XE_PATCH_MAX_NAME;
            if (PUCHAR r = Reserve(Out, OutSize, &used, n, &c)) {
                auto h = (XE_PATCH_RECORD *)r;
                h->Address = 0;
                h->Length = (UCHAR)n;
                h->OriginalLength = 0;
                h->Kind = XE_PATCH_NAME;
                h->Check = 0;
                for (ULONG i = 0; i < n; i++)
                    r[sizeof(*h) + i] = (UCHAR)line.Value[i];
            }
        } else if (IniKeyIs(line, "match")) {
            // ADDRESS: CURRENT_BYTES: when they are there, the blind patches below it in the section are written
            ULONG address, n = 0;
            UCHAR bytes[XE_PATCH_MAX_BYTES];
            if (!(cur.Number(&address, true) && address && cur.Expect(':') && cur.HexBytes(bytes, &n) && cur.AtEnd())) {
                c.Errors++;
                continue;
            }
            matchAddress = address;
            matchLength = n;
            for (ULONG i = 0; i < n; i++)
                match[i] = bytes[i];
        } else if (IniKeyIs(line, "patch")) {
            // ADDRESS: CURRENT_BYTES > NEW_BYTES (hex bytes; spaces between bytes are optional; both sides any
            // length), or a blind patch ADDRESS: > NEW_BYTES, written when the section's `match =` bytes are there
            ULONG address;
            UCHAR original[XE_PATCH_MAX_BYTES], bytes[XE_PATCH_MAX_BYTES];
            ULONG on = 0, n = 0;
            bool ok = cur.Number(&address, true) && cur.Expect(':');
            cur.SkipBlanks();
            bool byMatch = ok && cur.p < cur.end && *cur.p == '>';
            ok = ok && (byMatch || cur.HexBytes(original, &on)) && cur.Expect('>') && cur.HexBytes(bytes, &n) &&
                 cur.AtEnd();
            if (ok && byMatch && !matchLength)
                ok = false; // a blind patch without a `match =` line above it in this section
            if (!ok) {
                c.Errors++;
                continue;
            }
            if (!active)
                continue;
            const UCHAR *check = byMatch ? match : original;
            if (byMatch)
                on = matchLength;
            if (PUCHAR r = Reserve(Out, OutSize, &used, on + n, &c)) {
                auto h = (XE_PATCH_RECORD *)r;
                h->Address = address;
                h->Length = (UCHAR)n;
                h->OriginalLength = (UCHAR)on;
                h->Kind = XE_PATCH_BYTES;
                h->Check = byMatch ? matchAddress : 0;
                for (ULONG i = 0; i < on; i++)
                    r[sizeof(*h) + i] = check[i];
                for (ULONG i = 0; i < n; i++)
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
