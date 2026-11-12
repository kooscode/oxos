// The kernel's ini files: the line reader (include/xk/ini_file.h).
#include <xk/ini_file.h>

namespace {

bool IsBlank(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

bool IsComment(char c) {
    return c == ';' || c == '#';
}

// The text between From and To without the blanks around it.
void Trim(const char *&From, const char *&To) {
    while (From < To && IsBlank(*From))
        From++;
    while (To > From && IsBlank(To[-1]))
        To--;
}

// [name] with only blanks or a comment after the ']'.
void ReadSection(const char *From, const char *To, INI_LINE *Line) {
    const char *close = From + 1;
    while (close < To && *close != ']')
        close++;
    Line->Text = From + 1;
    Line->TextLength = static_cast<ULONG>(close - Line->Text);
    Line->Kind = INI_LINE_KIND::BAD_SECTION;
    if (close == To)
        return;
    const char *rest = close + 1;
    Trim(rest, To);
    if (rest == To || IsComment(*rest))
        Line->Kind = INI_LINE_KIND::SECTION;
}

// key = value: the key is one word.
void ReadKey(const char *From, const char *To, INI_LINE *Line) {
    const char *key = From;
    while (From < To && *From != '=' && !IsBlank(*From))
        From++;
    Line->Text = key;
    Line->TextLength = static_cast<ULONG>(From - key);
    while (From < To && IsBlank(*From))
        From++;
    if (From == To || *From != '=' || !Line->TextLength) {
        Line->Kind = INI_LINE_KIND::BAD_LINE;
        return;
    }
    const char *value = From + 1;
    Trim(value, To);
    Line->Kind = INI_LINE_KIND::KEY;
    Line->Value = value;
    Line->ValueLength = static_cast<ULONG>(To - value);
}

} // namespace

bool IniNextLine(INI_READER *Reader, INI_LINE *Line) {
    while (Reader->Next < Reader->End) {
        const char *from = Reader->Next;
        const char *to = from;
        while (to < Reader->End && *to != '\n')
            to++;
        Reader->Next = to + 1;
        Trim(from, to);
        if (from == to || IsComment(*from))
            continue;
        if (*from == '[')
            ReadSection(from, to, Line);
        else
            ReadKey(from, to, Line);
        return true;
    }
    return false;
}

bool IniKeyIs(const INI_LINE &Line, const char *Word) {
    ULONG i = 0;
    for (; i < Line.TextLength && Word[i]; i++) {
        if ((Line.Text[i] | 0x20) != Word[i])
            return false;
    }
    return i == Line.TextLength && !Word[i];
}
