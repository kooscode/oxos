// The C library of OXOS's own code and of kernel extensions: the standard functions, under their standard names. The Xbox kernel's Rtl* exports are for
// titles only (tools/check_c_library_use.py keeps them out of OXOS's own code).
//
// The kernel has the functions it uses itself (memory.cpp, c_library.cpp, sprintf.cpp) and exports them to extensions as they are, at OXOS's own ordinals
// 402-405 and 417-428 (spec/oxos_internal/34 §9.1). The others (c_library_extensions.cpp) are not in the kernel, so its resident size stays as stock's: the
// SDK's library has them (tools/gen_oxos_sdk.py copies the file), and an extension links the ones it calls.
#pragma once

#ifdef XK_HOST_TEST // host unit tests: the C library's own; the kernel's versions are built as Xk_strlen, ... and tested by those names
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#define XK_C(name) Xk_##name
#else
#define XK_C(name) name
typedef __SIZE_TYPE__ size_t;
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Memory (memory.cpp): the compiler also calls memcpy and memset for copies and zeroing.
void *XK_C(memcpy)(void *Destination, const void *Source, size_t Count);
void *XK_C(memmove)(void *Destination, const void *Source, size_t Count);
void *XK_C(memset)(void *Destination, int Value, size_t Count);
int XK_C(memcmp)(const void *A, const void *B, size_t Count);
void *XK_C(memchr)(const void *Memory, int Value, size_t Count); // the first, or null

// Strings (c_library.cpp, c_library_extensions.cpp). Compares by bytes; "case" means ASCII letters.
size_t XK_C(strlen)(const char *Text);
size_t XK_C(strnlen)(const char *Text, size_t Max);
int XK_C(strcmp)(const char *A, const char *B);
int XK_C(strncmp)(const char *A, const char *B, size_t Count);
int XK_C(strcasecmp)(const char *A, const char *B); // any case
int XK_C(strncasecmp)(const char *A, const char *B, size_t Count);
char *XK_C(strchr)(const char *Text, int Character);    // the first, or null
char *XK_C(strrchr)(const char *Text, int Character);   // the last, or null
char *XK_C(strstr)(const char *Text, const char *Part); // the first, or null
// Copies with the buffer's whole size: the result always ends in 0, and the length of the text it was meant to hold is returned (>= Size: cut short).
size_t XK_C(strlcpy)(char *Buffer, const char *Text, size_t Size);
size_t XK_C(strlcat)(char *Buffer, const char *Text, size_t Size);

// Numbers (c_library_extensions.cpp): base 0 reads 0x... as hexadecimal and 0... as octal; *End (when given) is set after the last digit read.
long XK_C(strtol)(const char *Text, char **End, int Base);
unsigned long XK_C(strtoul)(const char *Text, char **End, int Base);
int XK_C(atoi)(const char *Text);

// Characters (c_library.cpp, c_library_extensions.cpp): ASCII.
int XK_C(tolower)(int Character);
int XK_C(toupper)(int Character);
int XK_C(isdigit)(int Character);
int XK_C(isspace)(int Character);
int XK_C(isalpha)(int Character);
int XK_C(isalnum)(int Character);

// Formatting (sprintf.cpp), as C99: the output always ends in 0 (when Size > 0); returns the length the whole text has, so >= Size when cut short.
int XK_C(snprintf)(char *Buffer, size_t Size, const char *Format, ...);
int XK_C(vsnprintf)(char *Buffer, size_t Size, const char *Format, __builtin_va_list Args);

#ifdef __cplusplus
}
#endif
