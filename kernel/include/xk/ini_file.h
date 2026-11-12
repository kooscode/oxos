// The kernel's ini files in C:\OXOS: the line reader that title_patches.ini (spec/oxos_internal/25 §3.1) and
// launch_redirect.ini (spec/oxos_internal/33 §1) share. It makes no kernel calls, so the host unit tests run it
// unchanged (tests/unit/test_patches.cpp, test_launch_redirect.cpp).
//
//   ; a comment (also # ...)
//   [section]   ; a comment may follow a section header
//   key = value
#pragma once
#include <xk/types.h>

// What a line holds: a section header, a key and its value, or something that cannot be read: a broken section header
// (it still ends the section before it) or any other line.
enum class INI_LINE_KIND { SECTION, KEY, BAD_SECTION, BAD_LINE };

struct INI_LINE {
    INI_LINE_KIND Kind;
    const char *Text;   // SECTION: what is between the brackets; KEY: the key
    ULONG TextLength;
    const char *Value;  // KEY: everything after '=' (a comment there included), without the blanks around it
    ULONG ValueLength;
};

// The text still to read.
struct INI_READER {
    const char *Next;
    const char *End;
};

// The next line that is not blank or a comment, in *Line; false at the end of the text. Lines end in LF or CR LF.
// A section header must close its bracket, with only blanks or a comment after it; a key is one word followed by '='.
// Lines that cannot be read come back as BAD_SECTION or BAD_LINE, for the caller to count.
bool IniNextLine(INI_READER *Reader, INI_LINE *Line);

// True when the KEY line's key is Word (lower case), in any case.
bool IniKeyIs(const INI_LINE &Line, const char *Word);
