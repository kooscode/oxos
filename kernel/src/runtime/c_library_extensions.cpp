// The kernel's C library (c_library.h): the functions the kernel itself does not use. They are not in the kernel: tools/gen_oxos_sdk.py copies this file
// into the SDK (sdk/src/oxos_c_library.cpp), so each extension links the ones it calls, and the resident kernel stays as small as stock (spec/oxos_internal/34
// §9.1). The host unit tests build it with the rest.
#include <runtime/c_library.h>

extern "C" {

// --- Characters

int XK_C(tolower)(int Character) {
    return Character >= 'A' && Character <= 'Z' ? Character + ('a' - 'A') : Character;
}

int XK_C(isspace)(int Character) {
    return Character == ' ' || (Character >= '\t' && Character <= '\r');
}

int XK_C(isalpha)(int Character) {
    return (Character | 0x20) >= 'a' && (Character | 0x20) <= 'z';
}

int XK_C(isalnum)(int Character) {
    return XK_C(isalpha)(Character) || XK_C(isdigit)(Character);
}

// --- Strings

char *XK_C(strchr)(const char *Text, int Character) {
    for (;; Text++) {
        if (*Text == (char)Character)
            return (char *)Text;
        if (!*Text)
            return nullptr;
    }
}

char *XK_C(strrchr)(const char *Text, int Character) {
    const char *last = nullptr;
    for (;; Text++) {
        if (*Text == (char)Character)
            last = Text;
        if (!*Text)
            return (char *)last;
    }
}

char *XK_C(strstr)(const char *Text, const char *Part) {
    size_t n = XK_C(strlen)(Part);
    for (; *Text || !n; Text++) {
        if (!XK_C(strncmp)(Text, Part, n))
            return (char *)Text;
    }
    return nullptr;
}

size_t XK_C(strlcat)(char *Buffer, const char *Text, size_t Size) {
    size_t used = XK_C(strnlen)(Buffer, Size);
    if (used == Size)
        return Size + XK_C(strlen)(Text);
    return used + XK_C(strlcpy)(Buffer + used, Text, Size - used);
}

// --- Numbers

// Reads Text as a number in Base (0: from its prefix); *Negative when it starts with '-'. *End after the last digit, or Text when there is none.
static unsigned long ReadNumber(const char *Text, char **End, int Base, bool *Negative, bool *Overflow) {
    const char *p = Text;
    while (XK_C(isspace)((unsigned char)*p))
        p++;
    *Negative = *p == '-';
    p += *p == '-' || *p == '+';
    if ((Base | 16) == 16 && p[0] == '0' && (p[1] | 0x20) == 'x' && XK_C(isalnum)((unsigned char)p[2])) {
        p += 2;
        Base = 16;
    }
    if (!Base)
        Base = *p == '0' ? 8 : 10;
    unsigned long value = 0;
    const char *first = p;
    *Overflow = false;
    for (;; p++) {
        unsigned digit = (unsigned)(*p - '0') < 10 ? (unsigned)(*p - '0') : (unsigned)((*p | 0x20) - 'a') < 26 ? (unsigned)((*p | 0x20) - 'a' + 10) : 99;
        if (digit >= (unsigned)Base)
            break;
        unsigned long next = value * (unsigned long)Base + digit;
        *Overflow |= next / (unsigned long)Base != value;
        value = next;
    }
    if (End)
        *End = (char *)(p == first ? Text : p);
    return value;
}

unsigned long XK_C(strtoul)(const char *Text, char **End, int Base) {
    bool negative, overflow;
    unsigned long value = ReadNumber(Text, End, Base, &negative, &overflow);
    if (overflow)
        return ~0UL;
    return negative ? 0UL - value : value;
}

long XK_C(strtol)(const char *Text, char **End, int Base) {
    bool negative, overflow;
    unsigned long value = ReadNumber(Text, End, Base, &negative, &overflow);
    constexpr unsigned long LONG_LIMIT = ~0UL >> 1;
    if (overflow || value > LONG_LIMIT + negative)
        value = LONG_LIMIT + negative;
    return negative ? (long)(0UL - value) : (long)value;
}

int XK_C(atoi)(const char *Text) {
    return (int)XK_C(strtol)(Text, nullptr, 10);
}

} // extern "C"
