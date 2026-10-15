// Counted strings (ANSI OBJECT_STRING / STRING and UNICODE_STRING), as used by Ob and Io.
#include <xk/ob.h>
#include <xk/rtl.h>

XBAPI CHAR NTAPI RtlUpperChar(CHAR Character) {
    return (Character >= 'a' && Character <= 'z') ? (CHAR)(Character - 'a' + 'A') : Character;
}

XBAPI CHAR NTAPI RtlLowerChar(CHAR Character) {
    return (Character >= 'A' && Character <= 'Z') ? (CHAR)(Character - 'A' + 'a') : Character;
}

XBAPI void NTAPI RtlInitAnsiString(PANSI_STRING DestinationString, PCSZ SourceString) {
    USHORT len = 0;
    if (SourceString)
        while (SourceString[len])
            len++;
    DestinationString->Buffer = (PCHAR)SourceString;
    DestinationString->Length = len;
    DestinationString->MaximumLength = SourceString ? (USHORT)(len + 1) : 0;
}

XBAPI void NTAPI RtlInitUnicodeString(PUNICODE_STRING DestinationString, PCWSTR SourceString) {
    USHORT len = 0;
    if (SourceString)
        while (SourceString[len])
            len++;
    DestinationString->Buffer = (PWSTR)SourceString;
    DestinationString->Length = (USHORT)(len * sizeof(WCHAR));
    DestinationString->MaximumLength = SourceString ? (USHORT)((len + 1) * sizeof(WCHAR)) : 0;
}

XBAPI LONG NTAPI RtlCompareString(const ANSI_STRING *String1, const ANSI_STRING *String2, BOOLEAN CaseInSensitive) {
    USHORT n = String1->Length < String2->Length ? String1->Length : String2->Length;
    for (USHORT i = 0; i < n; i++) {
        CHAR a = String1->Buffer[i], b = String2->Buffer[i];
        if (CaseInSensitive) {
            a = RtlUpperChar(a);
            b = RtlUpperChar(b);
        }
        if (a != b)
            return (LONG)(UCHAR)a - (LONG)(UCHAR)b;
    }
    return (LONG)String1->Length - (LONG)String2->Length;
}

XBAPI BOOLEAN NTAPI RtlEqualString(const ANSI_STRING *String1, const ANSI_STRING *String2, BOOLEAN CaseInSensitive) {
    return String1->Length == String2->Length && RtlCompareString(String1, String2, CaseInSensitive) == 0;
}

static WCHAR UpcaseW(WCHAR c) { return (c >= 'a' && c <= 'z') ? (WCHAR)(c - 'a' + 'A') : c; }

XBAPI LONG NTAPI RtlCompareUnicodeString(const UNICODE_STRING *String1, const UNICODE_STRING *String2,
                                         BOOLEAN CaseInSensitive) {
    USHORT n1 = String1->Length / 2, n2 = String2->Length / 2, n = n1 < n2 ? n1 : n2;
    for (USHORT i = 0; i < n; i++) {
        WCHAR a = String1->Buffer[i], b = String2->Buffer[i];
        if (CaseInSensitive) {
            a = UpcaseW(a);
            b = UpcaseW(b);
        }
        if (a != b)
            return (LONG)a - (LONG)b;
    }
    return (LONG)n1 - (LONG)n2;
}

XBAPI BOOLEAN NTAPI RtlEqualUnicodeString(const UNICODE_STRING *String1, const UNICODE_STRING *String2,
                                          BOOLEAN CaseInSensitive) {
    return String1->Length == String2->Length && RtlCompareUnicodeString(String1, String2, CaseInSensitive) == 0;
}

XBAPI void NTAPI RtlCopyString(PANSI_STRING DestinationString, const ANSI_STRING *SourceString) {
    if (!SourceString) {
        DestinationString->Length = 0;
        return;
    }
    USHORT n = SourceString->Length < DestinationString->MaximumLength ? SourceString->Length
                                                                       : DestinationString->MaximumLength;
    RtlCopyMemory(DestinationString->Buffer, SourceString->Buffer, n);
    DestinationString->Length = n;
}

XBAPI void NTAPI RtlUpperString(PANSI_STRING DestinationString, const ANSI_STRING *SourceString) {
    USHORT n = SourceString->Length < DestinationString->MaximumLength ? SourceString->Length
                                                                       : DestinationString->MaximumLength;
    for (USHORT i = 0; i < n; i++)
        DestinationString->Buffer[i] = RtlUpperChar(SourceString->Buffer[i]);
    DestinationString->Length = n;
}

XBAPI NTSTATUS NTAPI RtlAnsiStringToUnicodeString(PUNICODE_STRING DestinationString, const ANSI_STRING *SourceString,
                                                  BOOLEAN AllocateDestinationString) {
    ULONG bytes = (ULONG)SourceString->Length * 2;
    if (bytes > 0xFFFE)
        return (NTSTATUS)0xC000000D;
    if (AllocateDestinationString) {
        DestinationString->Buffer = (PWSTR)ExAllocatePoolWithTag(bytes + 2, 'grtS');
        if (!DestinationString->Buffer)
            return (NTSTATUS)0xC0000017;
        DestinationString->MaximumLength = (USHORT)(bytes + 2);
    } else if (bytes > DestinationString->MaximumLength) {
        return (NTSTATUS)0x80000005; // STATUS_BUFFER_OVERFLOW
    }
    for (USHORT i = 0; i < SourceString->Length; i++)
        DestinationString->Buffer[i] = (UCHAR)SourceString->Buffer[i];
    DestinationString->Length = (USHORT)bytes;
    if (bytes + 2 <= DestinationString->MaximumLength)
        DestinationString->Buffer[bytes / 2] = 0;
    return STATUS_SUCCESS;
}

XBAPI NTSTATUS NTAPI RtlUnicodeStringToAnsiString(PANSI_STRING DestinationString, const UNICODE_STRING *SourceString,
                                                  BOOLEAN AllocateDestinationString) {
    ULONG chars = SourceString->Length / 2;
    if (AllocateDestinationString) {
        DestinationString->Buffer = (PCHAR)ExAllocatePoolWithTag(chars + 1, 'grtS');
        if (!DestinationString->Buffer)
            return (NTSTATUS)0xC0000017;
        DestinationString->MaximumLength = (USHORT)(chars + 1);
    } else if (chars > DestinationString->MaximumLength) {
        return (NTSTATUS)0x80000005;
    }
    for (ULONG i = 0; i < chars; i++) {
        WCHAR c = SourceString->Buffer[i];
        DestinationString->Buffer[i] = c < 0x100 ? (CHAR)c : '?';
    }
    DestinationString->Length = (USHORT)chars;
    if (chars < DestinationString->MaximumLength)
        DestinationString->Buffer[chars] = 0;
    return STATUS_SUCCESS;
}

XBAPI void NTAPI RtlFreeAnsiString(PANSI_STRING AnsiString) {
    if (AnsiString->Buffer)
        ExFreePool(AnsiString->Buffer);
    AnsiString->Buffer = nullptr;
    AnsiString->Length = AnsiString->MaximumLength = 0;
}

XBAPI void NTAPI RtlFreeUnicodeString(PUNICODE_STRING UnicodeString) {
    if (UnicodeString->Buffer)
        ExFreePool(UnicodeString->Buffer);
    UnicodeString->Buffer = nullptr;
    UnicodeString->Length = UnicodeString->MaximumLength = 0;
}
