// Runtime exports: interlocked operations, S-lists, memory/integer helpers, ANSI/Unicode strings and conversions,
// port buffers (spec/stock/13). Semantics follow the documented NT runtime; Xbox strings are ANSI, Unicode is UTF-16
// with a Latin-1 multibyte mapping.
#include <xk/arch.h>
#include <xk/ob.h>
#include <xk/rtl.h>

// --- Interlocked (fastcall, uniprocessor but locked anyway)

XBAPI LONG FASTCALL InterlockedIncrement(volatile LONG *Addend) { return __atomic_add_fetch(Addend, 1, __ATOMIC_SEQ_CST); }
XBAPI LONG FASTCALL InterlockedDecrement(volatile LONG *Addend) { return __atomic_sub_fetch(Addend, 1, __ATOMIC_SEQ_CST); }
XBAPI LONG FASTCALL InterlockedExchange(volatile LONG *Target, LONG Value) {
    return __atomic_exchange_n(Target, Value, __ATOMIC_SEQ_CST);
}
XBAPI LONG FASTCALL InterlockedExchangeAdd(volatile LONG *Addend, LONG Value) {
    return __atomic_fetch_add(Addend, Value, __ATOMIC_SEQ_CST);
}
XBAPI LONG FASTCALL InterlockedCompareExchange(volatile LONG *Destination, LONG Exchange, LONG Comparand) {
    __atomic_compare_exchange_n(Destination, &Comparand, Exchange, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return Comparand;
}

// 8-byte S-list header: {Next, Depth:16, Sequence:16}.
typedef struct _SINGLE_LIST_ENTRY {
    struct _SINGLE_LIST_ENTRY *Next;
} SINGLE_LIST_ENTRY, *PSINGLE_LIST_ENTRY;
typedef union _SLIST_HEADER {
    ULONGLONG Alignment;
    struct {
        SINGLE_LIST_ENTRY Next;
        USHORT Depth, Sequence;
    };
} SLIST_HEADER, *PSLIST_HEADER;

static bool XkCas64(volatile ULONGLONG *p, ULONGLONG *expected, ULONGLONG value) {
    return __atomic_compare_exchange_n(p, expected, value, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

XBAPI PSINGLE_LIST_ENTRY FASTCALL InterlockedPushEntrySList(PSLIST_HEADER ListHead, PSINGLE_LIST_ENTRY ListEntry) {
    SLIST_HEADER old, now;
    old.Alignment = ListHead->Alignment;
    do {
        ListEntry->Next = old.Next.Next;
        now.Next.Next = ListEntry;
        now.Depth = (USHORT)(old.Depth + 1);
        now.Sequence = (USHORT)(old.Sequence + 1);
    } while (!XkCas64(&ListHead->Alignment, &old.Alignment, now.Alignment));
    return old.Next.Next;
}

XBAPI PSINGLE_LIST_ENTRY FASTCALL InterlockedPopEntrySList(PSLIST_HEADER ListHead) {
    SLIST_HEADER old, now;
    old.Alignment = ListHead->Alignment;
    do {
        if (!old.Next.Next)
            return nullptr;
        now.Next.Next = old.Next.Next->Next;
        now.Depth = (USHORT)(old.Depth - 1);
        now.Sequence = old.Sequence;
    } while (!XkCas64(&ListHead->Alignment, &old.Alignment, now.Alignment));
    return old.Next.Next;
}

XBAPI PSINGLE_LIST_ENTRY FASTCALL InterlockedFlushSList(PSLIST_HEADER ListHead) {
    SLIST_HEADER old, now;
    old.Alignment = ListHead->Alignment;
    do {
        now.Alignment = 0;
        now.Sequence = old.Sequence;
    } while (old.Next.Next && !XkCas64(&ListHead->Alignment, &old.Alignment, now.Alignment));
    return old.Next.Next;
}

XBAPI LONGLONG FASTCALL ExInterlockedCompareExchange64(volatile LONGLONG *Destination, PLONGLONG Exchange,
                                                       PLONGLONG Comparand) {
    LONGLONG expected = *Comparand;
    __atomic_compare_exchange_n(Destination, &expected, *Exchange, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
}

XBAPI LARGE_INTEGER NTAPI ExInterlockedAddLargeInteger(PLARGE_INTEGER Addend, LARGE_INTEGER Increment,
                                                       PVOID /*Lock*/) {
    LARGE_INTEGER old;
    ULONG flags = save_flags_cli();
    old = *Addend;
    Addend->QuadPart += Increment.QuadPart;
    restore_flags(flags);
    return old;
}

XBAPI void FASTCALL ExInterlockedAddLargeStatistic(PLARGE_INTEGER Addend, ULONG Increment) {
    ULONG flags = save_flags_cli();
    Addend->QuadPart += Increment;
    restore_flags(flags);
}

XBAPI PLIST_ENTRY FASTCALL ExfInterlockedInsertHeadList(PLIST_ENTRY ListHead, PLIST_ENTRY ListEntry) {
    ULONG flags = save_flags_cli();
    PLIST_ENTRY first = ListHead->Flink;
    InsertHeadList(ListHead, ListEntry);
    restore_flags(flags);
    return first == ListHead ? nullptr : first;
}

XBAPI PLIST_ENTRY FASTCALL ExfInterlockedInsertTailList(PLIST_ENTRY ListHead, PLIST_ENTRY ListEntry) {
    ULONG flags = save_flags_cli();
    PLIST_ENTRY last = ListHead->Blink;
    InsertTailList(ListHead, ListEntry);
    restore_flags(flags);
    return last == ListHead ? nullptr : last;
}

XBAPI PLIST_ENTRY FASTCALL ExfInterlockedRemoveHeadList(PLIST_ENTRY ListHead) {
    ULONG flags = save_flags_cli();
    PLIST_ENTRY e = IsListEmpty(ListHead) ? nullptr : RemoveHeadList(ListHead);
    restore_flags(flags);
    return e;
}

// --- Byte order, memory, extended arithmetic

XBAPI ULONG FASTCALL RtlUlongByteSwap(ULONG Source) { return __builtin_bswap32(Source); }
XBAPI USHORT FASTCALL RtlUshortByteSwap(USHORT Source) { return __builtin_bswap16(Source); }

XBAPI SIZE_T NTAPI RtlCompareMemory(const VOID *Source1, const VOID *Source2, SIZE_T Length) {
    auto a = (const UCHAR *)Source1, b = (const UCHAR *)Source2;
    SIZE_T i = 0;
    while (i < Length && a[i] == b[i])
        i++;
    return i;
}

XBAPI SIZE_T NTAPI RtlCompareMemoryUlong(PVOID Source, SIZE_T Length, ULONG Pattern) {
    auto p = (const ULONG *)Source;
    SIZE_T i = 0;
    while (i + 4 <= Length && p[i / 4] == Pattern)
        i += 4;
    return i;
}

XBAPI void NTAPI RtlFillMemoryUlong(PVOID Destination, SIZE_T Length, ULONG Pattern) {
    auto p = (PULONG)Destination;
    for (SIZE_T i = 0; i < Length / 4; i++)
        p[i] = Pattern;
}

XBAPI LARGE_INTEGER NTAPI RtlExtendedIntegerMultiply(LARGE_INTEGER Multiplicand, LONG Multiplier) {
    LARGE_INTEGER r;
    r.QuadPart = Multiplicand.QuadPart * Multiplier;
    return r;
}

XBAPI LARGE_INTEGER NTAPI RtlExtendedLargeIntegerDivide(LARGE_INTEGER Dividend, ULONG Divisor, PULONG Remainder) {
    LARGE_INTEGER r;
    ULONGLONG n = (ULONGLONG)Dividend.QuadPart;
    r.QuadPart = (LONGLONG)(n / Divisor);
    if (Remainder)
        *Remainder = (ULONG)(n % Divisor);
    return r;
}

// (Dividend * MagicDivisor) >> (64 + ShiftCount), keeping the sign of the dividend.
XBAPI LARGE_INTEGER NTAPI RtlExtendedMagicDivide(LARGE_INTEGER Dividend, LARGE_INTEGER MagicDivisor, CCHAR ShiftCount) {
    bool negative = Dividend.QuadPart < 0;
    ULONGLONG a = negative ? 0 - (ULONGLONG)Dividend.QuadPart : (ULONGLONG)Dividend.QuadPart;
    ULONGLONG b = (ULONGLONG)MagicDivisor.QuadPart;
    ULONGLONG al = (ULONG)a, ah = a >> 32, bl = (ULONG)b, bh = b >> 32;
    ULONGLONG lo = al * bl, m1 = ah * bl, m2 = al * bh, hi = ah * bh;
    ULONGLONG mid = (lo >> 32) + (ULONG)m1 + (ULONG)m2;
    ULONGLONG high = hi + (m1 >> 32) + (m2 >> 32) + (mid >> 32);
    high >>= ShiftCount;
    LARGE_INTEGER r;
    r.QuadPart = negative ? -(LONGLONG)high : (LONGLONG)high;
    return r;
}

// --- Generic access mapping and port buffers

struct GENERIC_MAPPING {
    ULONG GenericRead, GenericWrite, GenericExecute, GenericAll;
};
XBAPI void NTAPI RtlMapGenericMask(PULONG AccessMask, const GENERIC_MAPPING *GenericMapping) {
    ULONG m = *AccessMask;
    if (m & 0x80000000)
        m |= GenericMapping->GenericRead;
    if (m & 0x40000000)
        m |= GenericMapping->GenericWrite;
    if (m & 0x20000000)
        m |= GenericMapping->GenericExecute;
    if (m & 0x10000000)
        m |= GenericMapping->GenericAll;
    *AccessMask = m & 0x0FFFFFFF;
}

XBAPI void NTAPI READ_PORT_BUFFER_UCHAR(PUCHAR Port, PUCHAR Buffer, ULONG Count) {
    for (ULONG i = 0; i < Count; i++)
        Buffer[i] = inb((USHORT)(ULONG)Port);
}
XBAPI void NTAPI READ_PORT_BUFFER_USHORT(PUSHORT Port, PUSHORT Buffer, ULONG Count) {
    for (ULONG i = 0; i < Count; i++)
        Buffer[i] = inw((USHORT)(ULONG)Port);
}
XBAPI void NTAPI READ_PORT_BUFFER_ULONG(PULONG Port, PULONG Buffer, ULONG Count) {
    for (ULONG i = 0; i < Count; i++)
        Buffer[i] = inl((USHORT)(ULONG)Port);
}
XBAPI void NTAPI WRITE_PORT_BUFFER_UCHAR(PUCHAR Port, PUCHAR Buffer, ULONG Count) {
    for (ULONG i = 0; i < Count; i++)
        outb((USHORT)(ULONG)Port, Buffer[i]);
}
XBAPI void NTAPI WRITE_PORT_BUFFER_USHORT(PUSHORT Port, PUSHORT Buffer, ULONG Count) {
    for (ULONG i = 0; i < Count; i++)
        outw((USHORT)(ULONG)Port, Buffer[i]);
}
XBAPI void NTAPI WRITE_PORT_BUFFER_ULONG(PULONG Port, PULONG Buffer, ULONG Count) {
    for (ULONG i = 0; i < Count; i++)
        outl((USHORT)(ULONG)Port, Buffer[i]);
}

// --- Strings

constexpr NTSTATUS STATUS_BUFFER_TOO_SMALL_ = (NTSTATUS)0xC0000023, STATUS_INVALID_PARAMETER_ = (NTSTATUS)0xC000000D,
                   STATUS_NO_MEMORY_ = (NTSTATUS)0xC0000017, STATUS_BUFFER_OVERFLOW_ = (NTSTATUS)0x80000005;

XBAPI NTSTATUS NTAPI RtlAppendStringToString(PSTRING Destination, const STRING *Source) {
    if (!Source->Length)
        return STATUS_SUCCESS;
    if (Destination->Length + Source->Length > Destination->MaximumLength)
        return STATUS_BUFFER_TOO_SMALL_;
    RtlMoveMemory(Destination->Buffer + Destination->Length, Source->Buffer, Source->Length);
    Destination->Length = (USHORT)(Destination->Length + Source->Length);
    return STATUS_SUCCESS;
}

XBAPI NTSTATUS NTAPI RtlAppendUnicodeStringToString(PUNICODE_STRING Destination, const UNICODE_STRING *Source) {
    if (!Source->Length)
        return STATUS_SUCCESS;
    if (Destination->Length + Source->Length > Destination->MaximumLength)
        return STATUS_BUFFER_TOO_SMALL_;
    RtlMoveMemory((PUCHAR)Destination->Buffer + Destination->Length, Source->Buffer, Source->Length);
    Destination->Length = (USHORT)(Destination->Length + Source->Length);
    if (Destination->Length + 2 <= Destination->MaximumLength)
        Destination->Buffer[Destination->Length / 2] = 0;
    return STATUS_SUCCESS;
}

XBAPI NTSTATUS NTAPI RtlAppendUnicodeToString(PUNICODE_STRING Destination, PCWSTR Source) {
    if (!Source)
        return STATUS_SUCCESS;
    UNICODE_STRING s;
    RtlInitUnicodeString(&s, Source);
    return RtlAppendUnicodeStringToString(Destination, &s);
}

XBAPI void NTAPI RtlCopyUnicodeString(PUNICODE_STRING Destination, const UNICODE_STRING *Source) {
    if (!Source) {
        Destination->Length = 0;
        return;
    }
    USHORT n = Source->Length < Destination->MaximumLength ? Source->Length : Destination->MaximumLength;
    RtlMoveMemory(Destination->Buffer, Source->Buffer, n);
    Destination->Length = n;
    if (n + 2 <= Destination->MaximumLength)
        Destination->Buffer[n / 2] = 0;
}

XBAPI BOOLEAN NTAPI RtlCreateUnicodeString(PUNICODE_STRING DestinationString, PCWSTR SourceString) {
    ULONG n = 0;
    while (SourceString[n])
        n++;
    ULONG bytes = (n + 1) * 2;
    if (bytes > 0xFFFE)
        return FALSE;
    auto buf = (PWSTR)ExAllocatePoolWithTag(bytes, 'grtS');
    if (!buf)
        return FALSE;
    RtlMoveMemory(buf, SourceString, bytes);
    DestinationString->Buffer = buf;
    DestinationString->Length = (USHORT)(bytes - 2);
    DestinationString->MaximumLength = (USHORT)bytes;
    return TRUE;
}

// Latin-1 case mapping (0xDF and 0xFF have no single-character upper case in Latin-1).
XBAPI WCHAR NTAPI RtlUpcaseUnicodeChar(WCHAR SourceCharacter) {
    WCHAR c = SourceCharacter;
    if ((c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7))
        return (WCHAR)(c - 0x20);
    return c;
}

XBAPI WCHAR NTAPI RtlDowncaseUnicodeChar(WCHAR SourceCharacter) {
    WCHAR c = SourceCharacter;
    if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7))
        return (WCHAR)(c + 0x20);
    return c;
}

static NTSTATUS RtlpCaseUnicodeString(PUNICODE_STRING Destination, const UNICODE_STRING *Source, BOOLEAN Allocate,
                                      WCHAR (*Map)(WCHAR)) {
    if (Allocate) {
        Destination->Buffer = (PWSTR)ExAllocatePoolWithTag(Source->Length ? Source->Length : 2, 'grtS');
        if (!Destination->Buffer)
            return STATUS_NO_MEMORY_;
        Destination->MaximumLength = Source->Length;
    } else if (Source->Length > Destination->MaximumLength) {
        return STATUS_BUFFER_TOO_SMALL_;
    }
    for (ULONG i = 0; i < Source->Length / 2u; i++)
        Destination->Buffer[i] = Map(Source->Buffer[i]);
    Destination->Length = Source->Length;
    return STATUS_SUCCESS;
}

static WCHAR RtlpUp(WCHAR c) { return RtlUpcaseUnicodeChar(c); }
static WCHAR RtlpDown(WCHAR c) { return RtlDowncaseUnicodeChar(c); }

XBAPI NTSTATUS NTAPI RtlUpcaseUnicodeString(PUNICODE_STRING DestinationString, const UNICODE_STRING *SourceString,
                                            BOOLEAN AllocateDestinationString) {
    return RtlpCaseUnicodeString(DestinationString, SourceString, AllocateDestinationString, RtlpUp);
}

XBAPI NTSTATUS NTAPI RtlDowncaseUnicodeString(PUNICODE_STRING DestinationString, const UNICODE_STRING *SourceString,
                                              BOOLEAN AllocateDestinationString) {
    return RtlpCaseUnicodeString(DestinationString, SourceString, AllocateDestinationString, RtlpDown);
}

// Multibyte <-> Unicode: Latin-1 (characters above 0xFF become '?').
XBAPI NTSTATUS NTAPI RtlMultiByteToUnicodeN(PWSTR UnicodeString, ULONG MaxBytesInUnicodeString,
                                            PULONG BytesInUnicodeString, const CHAR *MultiByteString,
                                            ULONG BytesInMultiByteString) {
    ULONG n = MaxBytesInUnicodeString / 2 < BytesInMultiByteString ? MaxBytesInUnicodeString / 2 : BytesInMultiByteString;
    for (ULONG i = 0; i < n; i++)
        UnicodeString[i] = (UCHAR)MultiByteString[i];
    if (BytesInUnicodeString)
        *BytesInUnicodeString = n * 2;
    return STATUS_SUCCESS;
}

XBAPI NTSTATUS NTAPI RtlMultiByteToUnicodeSize(PULONG BytesInUnicodeString, const CHAR *, ULONG BytesInMultiByteString) {
    *BytesInUnicodeString = BytesInMultiByteString * 2;
    return STATUS_SUCCESS;
}

static NTSTATUS RtlpUnicodeToMultiByte(PCHAR MultiByteString, ULONG MaxBytes, PULONG BytesInMultiByteString,
                                       const WCHAR *UnicodeString, ULONG BytesInUnicodeString, bool upper) {
    ULONG n = BytesInUnicodeString / 2 < MaxBytes ? BytesInUnicodeString / 2 : MaxBytes;
    for (ULONG i = 0; i < n; i++) {
        WCHAR c = upper ? RtlUpcaseUnicodeChar(UnicodeString[i]) : UnicodeString[i];
        MultiByteString[i] = c > 0xFF ? '?' : (CHAR)c;
    }
    if (BytesInMultiByteString)
        *BytesInMultiByteString = n;
    return STATUS_SUCCESS;
}

XBAPI NTSTATUS NTAPI RtlUnicodeToMultiByteN(PCHAR MultiByteString, ULONG MaxBytesInMultiByteString,
                                            PULONG BytesInMultiByteString, const WCHAR *UnicodeString,
                                            ULONG BytesInUnicodeString) {
    return RtlpUnicodeToMultiByte(MultiByteString, MaxBytesInMultiByteString, BytesInMultiByteString, UnicodeString,
                                  BytesInUnicodeString, false);
}

XBAPI NTSTATUS NTAPI RtlUpcaseUnicodeToMultiByteN(PCHAR MultiByteString, ULONG MaxBytesInMultiByteString,
                                                  PULONG BytesInMultiByteString, const WCHAR *UnicodeString,
                                                  ULONG BytesInUnicodeString) {
    return RtlpUnicodeToMultiByte(MultiByteString, MaxBytesInMultiByteString, BytesInMultiByteString, UnicodeString,
                                  BytesInUnicodeString, true);
}

XBAPI NTSTATUS NTAPI RtlUnicodeToMultiByteSize(PULONG BytesInMultiByteString, const WCHAR *,
                                               ULONG BytesInUnicodeString) {
    *BytesInMultiByteString = BytesInUnicodeString / 2;
    return STATUS_SUCCESS;
}

// --- Integer <-> text

static int RtlpDigit(int c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z')
        return c - 'A' + 10;
    return 99;
}

template <typename CharT>
static NTSTATUS RtlpTextToInteger(const CharT *p, ULONG n, ULONG Base, PULONG Value) {
    ULONG i = 0;
    while (i < n && p[i] <= ' ' && p[i])
        i++;
    bool negative = false;
    if (i < n && (p[i] == '-' || p[i] == '+'))
        negative = p[i++] == '-';
    if (!Base) {
        Base = 10;
        if (i + 1 < n && p[i] == '0') {
            if (p[i + 1] == 'x' || p[i + 1] == 'X')
                Base = 16, i += 2;
            else if (p[i + 1] == 'o' || p[i + 1] == 'O')
                Base = 8, i += 2;
            else if (p[i + 1] == 'b' || p[i + 1] == 'B')
                Base = 2, i += 2;
        }
    } else if (Base != 2 && Base != 8 && Base != 10 && Base != 16) {
        return STATUS_INVALID_PARAMETER_;
    }
    ULONG v = 0;
    for (; i < n && p[i]; i++) {
        ULONG d = (ULONG)RtlpDigit(p[i]);
        if (d >= Base)
            break;
        v = v * Base + d;
    }
    *Value = negative ? 0 - v : v;
    return STATUS_SUCCESS;
}

XBAPI NTSTATUS NTAPI RtlCharToInteger(PCSZ String, ULONG Base, PULONG Value) {
    return RtlpTextToInteger(String, 0xFFFFFFFF, Base, Value);
}

XBAPI NTSTATUS NTAPI RtlUnicodeStringToInteger(const UNICODE_STRING *String, ULONG Base, PULONG Value) {
    return RtlpTextToInteger(String->Buffer, String->Length / 2u, Base, Value);
}

static ULONG RtlpFormatUnsigned(ULONG Value, ULONG Base, CHAR *out) {
    CHAR tmp[33];
    ULONG n = 0;
    do {
        ULONG d = Value % Base;
        tmp[n++] = (CHAR)(d < 10 ? '0' + d : 'A' + d - 10);
        Value /= Base;
    } while (Value);
    for (ULONG i = 0; i < n; i++)
        out[i] = tmp[n - 1 - i];
    return n;
}

XBAPI NTSTATUS NTAPI RtlIntegerToChar(ULONG Value, ULONG Base, LONG OutputLength, PCHAR String) {
    if (!Base)
        Base = 10;
    if (Base != 2 && Base != 8 && Base != 10 && Base != 16)
        return STATUS_INVALID_PARAMETER_;
    CHAR digits[33];
    ULONG n = RtlpFormatUnsigned(Value, Base, digits);
    if ((LONG)n > OutputLength)
        return STATUS_BUFFER_OVERFLOW_;
    RtlCopyMemory(String, digits, n);
    if ((LONG)n < OutputLength)
        String[n] = 0;
    return STATUS_SUCCESS;
}

XBAPI NTSTATUS NTAPI RtlIntegerToUnicodeString(ULONG Value, ULONG Base, PUNICODE_STRING String) {
    if (!Base)
        Base = 10;
    if (Base != 2 && Base != 8 && Base != 10 && Base != 16)
        return STATUS_INVALID_PARAMETER_;
    CHAR digits[33];
    ULONG n = RtlpFormatUnsigned(Value, Base, digits);
    if ((n + 1) * 2 > String->MaximumLength)
        return STATUS_BUFFER_OVERFLOW_;
    for (ULONG i = 0; i < n; i++)
        String->Buffer[i] = (UCHAR)digits[i];
    String->Buffer[n] = 0;
    String->Length = (USHORT)(n * 2);
    return STATUS_SUCCESS;
}
