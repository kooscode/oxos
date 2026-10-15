// RtlSprintf family: MS C runtime formatting (_snprintf semantics: -1 when the output was truncated).
#include <xk/ob.h>

typedef __builtin_va_list va_list;
#define va_start(v, l) __builtin_va_start(v, l)
#define va_end(v) __builtin_va_end(v)
#define va_arg(v, t) __builtin_va_arg(v, t)

struct RTLP_OUT {
    PCHAR Buffer;
    ULONG Size, Used;
    void Put(CHAR c) {
        if (Used < Size)
            Buffer[Used] = c;
        Used++;
    }
};

static void RtlpEmitNumber(RTLP_OUT &o, ULONGLONG v, bool negative, ULONG base, bool upper, int width, int precision,
                           bool left, bool zero, char sign, bool alt) {
    CHAR digits[66];
    int n = 0;
    const char *set = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v || precision != 0)
        do {
            digits[n++] = set[v % base];
            v /= base;
        } while (v);
    while (n < precision)
        digits[n++] = '0';
    CHAR prefix[3];
    int p = 0;
    if (negative)
        prefix[p++] = '-';
    else if (sign)
        prefix[p++] = sign;
    if (alt && base == 16 && n)
        prefix[p++] = '0', prefix[p++] = upper ? 'X' : 'x';
    else if (alt && base == 8 && (!n || digits[n - 1] != '0'))
        digits[n++] = '0';
    int pad = width - n - p;
    if (zero && !left && precision < 0) {
        for (int i = 0; i < p; i++)
            o.Put(prefix[i]);
        for (; pad > 0; pad--)
            o.Put('0');
    } else {
        if (!left)
            for (; pad > 0; pad--)
                o.Put(' ');
        for (int i = 0; i < p; i++)
            o.Put(prefix[i]);
    }
    while (n)
        o.Put(digits[--n]);
    for (; pad > 0; pad--)
        o.Put(' ');
}

static int RtlpFormat(RTLP_OUT &o, const char *f, va_list ap) {
    for (; *f; f++) {
        if (*f != '%') {
            o.Put(*f);
            continue;
        }
        f++;
        bool left = false, zero = false, alt = false;
        char sign = 0;
        for (;; f++) {
            if (*f == '-')
                left = true;
            else if (*f == '0')
                zero = true;
            else if (*f == '+')
                sign = '+';
            else if (*f == ' ' && !sign)
                sign = ' ';
            else if (*f == '#')
                alt = true;
            else
                break;
        }
        int width = 0, precision = -1;
        if (*f == '*') {
            width = va_arg(ap, int);
            if (width < 0)
                left = true, width = -width;
            f++;
        } else {
            while (*f >= '0' && *f <= '9')
                width = width * 10 + (*f++ - '0');
        }
        if (*f == '.') {
            f++;
            precision = 0;
            if (*f == '*') {
                precision = va_arg(ap, int);
                f++;
            } else {
                while (*f >= '0' && *f <= '9')
                    precision = precision * 10 + (*f++ - '0');
            }
        }
        int size = 0; // 0 int, 1 long, 2 64-bit, -1 short, 'w' wide
        bool wide = false;
        if (*f == 'h')
            size = -1, f++;
        else if (*f == 'l') {
            size = 1, f++;
            if (*f == 'l')
                size = 2, f++;
        } else if (*f == 'L')
            size = 2, f++;
        else if (*f == 'w')
            wide = true, f++;
        else if (f[0] == 'I' && f[1] == '6' && f[2] == '4')
            size = 2, f += 3;
        else if (f[0] == 'I' && f[1] == '3' && f[2] == '2')
            size = 1, f += 3;
        if (!*f)
            break;
        switch (*f) {
        case 'd':
        case 'i': {
            LONGLONG v = size == 2 ? va_arg(ap, LONGLONG) : (LONGLONG)va_arg(ap, int);
            if (size == -1)
                v = (short)v;
            RtlpEmitNumber(o, v < 0 ? 0 - (ULONGLONG)v : (ULONGLONG)v, v < 0, 10, false, width, precision, left, zero,
                           sign, false);
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o': {
            ULONGLONG v = size == 2 ? va_arg(ap, ULONGLONG) : (ULONGLONG)va_arg(ap, unsigned);
            if (size == -1)
                v = (USHORT)v;
            ULONG base = *f == 'u' ? 10 : *f == 'o' ? 8 : 16;
            RtlpEmitNumber(o, v, false, base, *f == 'X', width, precision, left, zero, 0, alt);
            break;
        }
        case 'p':
            RtlpEmitNumber(o, (ULONG)va_arg(ap, void *), false, 16, true, 8, 8, left, false, 0, false);
            break;
        case 'c':
        case 'C': {
            int c = va_arg(ap, int);
            bool w = (*f == 'C') != (size == -1) || wide || size == 1;
            if (!left)
                for (int i = 1; i < width; i++)
                    o.Put(' ');
            o.Put((CHAR)(w && (c & 0xFFFF) > 0xFF ? '?' : c));
            if (left)
                for (int i = 1; i < width; i++)
                    o.Put(' ');
            break;
        }
        case 's':
        case 'S':
        case 'Z': {
            const void *s = va_arg(ap, const void *);
            bool w = (*f == 'S') != (size == -1) || wide || size == 1;
            ULONG n = 0;
            if (*f == 'Z') { // counted string (ANSI_STRING, or UNICODE_STRING with w/l)
                auto str = (const STRING *)s;
                s = str ? (const void *)str->Buffer : nullptr;
                n = str ? (w ? str->Length / 2u : str->Length) : 0;
            } else if (!s) {
                s = w ? (const void *)L"(null)" : "(null)";
            }
            if (*f != 'Z' || !s) {
                if (!s)
                    s = "(null)", w = false;
                n = 0;
                if (w)
                    while (((const WCHAR *)s)[n])
                        n++;
                else
                    while (((const char *)s)[n])
                        n++;
            }
            if (precision >= 0 && (ULONG)precision < n)
                n = precision;
            int pad = width - (int)n;
            if (!left)
                for (; pad > 0; pad--)
                    o.Put(' ');
            for (ULONG i = 0; i < n; i++) {
                ULONG c = w ? ((const WCHAR *)s)[i] : (UCHAR)((const char *)s)[i];
                o.Put((CHAR)(c > 0xFF ? '?' : c));
            }
            for (; pad > 0; pad--)
                o.Put(' ');
            break;
        }
        case 'n':
            *va_arg(ap, int *) = (int)o.Used;
            break;
        default:
            o.Put(*f);
        }
    }
    return (int)o.Used;
}

XBAPI int __cdecl RtlVsnprintf(char *Buffer, SIZE_T Size, const char *Format, va_list Args) {
    RTLP_OUT o = {Buffer, Size, 0};
    int n = RtlpFormat(o, Format, Args);
    if ((ULONG)n < Size) {
        Buffer[n] = 0;
        return n;
    }
    return -1; // truncated, no terminator (as _vsnprintf)
}

XBAPI int __cdecl RtlVsprintf(char *Buffer, const char *Format, va_list Args) {
    RTLP_OUT o = {Buffer, 0x7FFFFFFF, 0};
    int n = RtlpFormat(o, Format, Args);
    Buffer[n] = 0;
    return n;
}

XBAPI int __cdecl RtlSnprintf(char *Buffer, SIZE_T Size, const char *Format, ...) {
    va_list ap;
    va_start(ap, Format);
    int n = RtlVsnprintf(Buffer, Size, Format, ap);
    va_end(ap);
    return n;
}

XBAPI int __cdecl RtlSprintf(char *Buffer, const char *Format, ...) {
    va_list ap;
    va_start(ap, Format);
    int n = RtlVsprintf(Buffer, Format, ap);
    va_end(ap);
    return n;
}
