// The C library of the code in shared/: the standard functions under their standard names, from whichever C library the program is built with.
//   - The kernel (and the host unit tests, which build the kernel's sources), built with OXOS_KERNEL: the kernel's own (kernel/src/runtime/c_library.h).
//   - Apps (nxdk) and host tools: the real one. nxdk's lacks strlcpy, strncasecmp and strcasecmp, so they are defined here for nxdk builds.
#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(OXOS_KERNEL)
#include <runtime/c_library.h>
#else
#include <ctype.h>
#include <string.h>
#if defined(NXDK)
// Copies Text into Buffer (Size bytes), cut to fit and always terminated; the length of Text.
inline size_t strlcpy(char *Buffer, const char *Text, size_t Size) {
    size_t length = strlen(Text);
    if (Size) {
        size_t copied = length < Size - 1 ? length : Size - 1;
        memcpy(Buffer, Text, copied);
        Buffer[copied] = 0;
    }
    return length;
}

// Compares at most Count characters, ASCII letters in any case.
inline int strncasecmp(const char *A, const char *B, size_t Count) {
    for (size_t i = 0; i < Count; i++) {
        int a = tolower(static_cast<unsigned char>(A[i]));
        int b = tolower(static_cast<unsigned char>(B[i]));
        if (a != b || !a) {
            return a - b;
        }
    }
    return 0;
}

// Compares two strings, ASCII letters in any case.
inline int strcasecmp(const char *A, const char *B) {
    return strncasecmp(A, B, static_cast<size_t>(-1));
}
#else
#include <strings.h>
#endif
#endif
