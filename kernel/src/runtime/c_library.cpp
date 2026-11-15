// The kernel's C library (c_library.h): the functions the kernel uses itself, exported to kernel extensions as they are. The others are in
// c_library_extensions.cpp.
#include <runtime/c_library.h>

extern "C" {

// --- Characters

int XK_C(toupper)(int Character) {
    return Character >= 'a' && Character <= 'z' ? Character - ('a' - 'A') : Character;
}

int XK_C(isdigit)(int Character) {
    return Character >= '0' && Character <= '9';
}

// --- Strings

size_t XK_C(strlen)(const char *Text) {
    size_t n = 0;
    while (Text[n])
        n++;
    return n;
}

size_t XK_C(strnlen)(const char *Text, size_t Max) {
    size_t n = 0;
    while (n < Max && Text[n])
        n++;
    return n;
}

int XK_C(strncmp)(const char *A, const char *B, size_t Count) {
    for (size_t i = 0; i < Count; i++) {
        unsigned char a = A[i], b = B[i];
        if (a != b || !a)
            return a - b;
    }
    return 0;
}

int XK_C(strcmp)(const char *A, const char *B) {
    return XK_C(strncmp)(A, B, (size_t)-1);
}

int XK_C(strncasecmp)(const char *A, const char *B, size_t Count) {
    for (size_t i = 0; i < Count; i++) {
        unsigned a = (unsigned char)A[i], b = (unsigned char)B[i];
        if (a - 'a' < 26u) // as toupper, without a call per character
            a -= 'a' - 'A';
        if (b - 'a' < 26u)
            b -= 'a' - 'A';
        if (a != b || !a)
            return (int)a - (int)b;
    }
    return 0;
}

int XK_C(strcasecmp)(const char *A, const char *B) {
    return XK_C(strncasecmp)(A, B, (size_t)-1);
}

size_t XK_C(strlcpy)(char *Buffer, const char *Text, size_t Size) {
    size_t n = XK_C(strlen)(Text);
    if (Size) {
        size_t copy = n < Size ? n : Size - 1;
        XK_C(memcpy)(Buffer, Text, copy);
        Buffer[copy] = 0;
    }
    return n;
}

} // extern "C"
