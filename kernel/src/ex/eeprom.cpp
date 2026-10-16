// EEPROM (SMBus 0xA8): settings access and the encrypted security section (spec/12).
#include <xk/hal.h>
#include <xk/ke.h>
#include <xk/rtl.h>

XBAPI NTSTATUS NTAPI HalReadSMBusValue(UCHAR SlaveAddress, UCHAR CommandCode, BOOLEAN ReadWordValue, PULONG DataValue);
XBAPI NTSTATUS NTAPI HalWriteSMBusValue(UCHAR SlaveAddress, UCHAR CommandCode, BOOLEAN WriteWordValue, ULONG DataValue);
XBAPI void NTAPI XcHMAC(const UCHAR *, ULONG, const UCHAR *, ULONG, const UCHAR *, ULONG, PUCHAR);
XBAPI void NTAPI XcRC4Key(PUCHAR, ULONG, const UCHAR *);
XBAPI void NTAPI XcRC4Crypt(PUCHAR, ULONG, PUCHAR);
XBAPI void NTAPI XcSHAInit(PUCHAR);
XBAPI void NTAPI XcSHAUpdate(PUCHAR, const UCHAR *, ULONG);
XBAPI void NTAPI XcSHAFinal(PUCHAR, PUCHAR);
extern ULONG KiEepromKeyStates[8][10], KiEepromKeyStateCount;

constexpr UCHAR EEPROM_ADDRESS = 0xA8;
constexpr ULONG REG_BINARY = 3, REG_DWORD = 4;
constexpr NTSTATUS STATUS_OBJECT_NAME_NOT_FOUND_ = (NTSTATUS)0xC0000034, STATUS_BUFFER_TOO_SMALL_ = (NTSTATUS)0xC0000023,
                   STATUS_INVALID_PARAMETER_ = (NTSTATUS)0xC000000D, STATUS_DEVICE_DATA_ERROR_ = (NTSTATUS)0xC000009C;

XBAPI UCHAR XboxHDKey[16];
XK_PERSIST UCHAR XboxHDKey[16];
extern ULONG XeConsoleGameRegion;
XK_PERSIST volatile ULONG ExpEepromSecurityValid; // 1 when the security section decrypted and authenticated

// Setting descriptors {type, index, size, offset-in-section}; layout of the EEPROM format.
struct EEPROM_SETTING {
    UCHAR Type, Index, Size, Offset;
};
static const EEPROM_SETTING ExpFactorySettings[] = { // section at 0x30, 0x30 bytes, index 0x100 + n
    {3, 0x00, 0x0C, 0x04}, {3, 0x01, 0x06, 0x10}, {3, 0x02, 0x10, 0x18}, {4, 0x03, 0x04, 0x28}, {3, 0xFF, 0x30, 0x00}};
static const EEPROM_SETTING ExpUserSettings[] = { // section at 0x60, 0x60 bytes, index < 0x100
    {4, 0x00, 0x04, 0x04}, {3, 0x01, 0x04, 0x08}, {3, 0x02, 0x04, 0x18}, {4, 0x03, 0x04, 0x28}, {3, 0x04, 0x04, 0x0C},
    {3, 0x05, 0x04, 0x1C}, {4, 0x06, 0x04, 0x2C}, {4, 0x07, 0x04, 0x30}, {4, 0x08, 0x04, 0x34}, {4, 0x09, 0x04, 0x38},
    {4, 0x0A, 0x04, 0x3C}, {4, 0x0B, 0x04, 0x40}, {4, 0x0C, 0x04, 0x44}, {4, 0x0D, 0x04, 0x48}, {4, 0x0E, 0x04, 0x4C},
    {4, 0x0F, 0x04, 0x50}, {4, 0x10, 0x04, 0x54}, {4, 0x11, 0x04, 0x58}, {4, 0x12, 0x04, 0x5C}, {3, 0xFF, 0x60, 0x00}};

static UCHAR ExpEeprom[256]; // shadow copy
static bool ExpEepromValid;
static KEVENT ExpEepromLock;
static bool ExpLockReady;

static void ExpLock() {
    if (!ExpLockReady) {
        KeInitializeEvent(&ExpEepromLock, SynchronizationEvent, TRUE);
        ExpLockReady = true;
    }
    KeWaitForSingleObject(&ExpEepromLock, Executive, KernelMode, FALSE, nullptr);
}
static void ExpUnlock() { KeSetEvent(&ExpEepromLock, 0, FALSE); }

static NTSTATUS ExpLoadEeprom() {
    if (ExpEepromValid)
        return STATUS_SUCCESS;
    for (ULONG i = 0; i < 256; i++) {
        ULONG v;
        NTSTATUS status = HalReadSMBusValue(EEPROM_ADDRESS, (UCHAR)i, FALSE, &v);
        if (!NT_SUCCESS(status))
            return status;
        ExpEeprom[i] = (UCHAR)v;
    }
    ExpEepromValid = true;
    return STATUS_SUCCESS;
}

// 32-bit sum with end-around carry; a section is valid when it sums to 0xFFFFFFFF.
static ULONG ExpChecksum(const UCHAR *p, ULONG n) {
    ULONG sum = 0, carry = 0;
    for (ULONG i = 0; i + 4 <= n; i += 4) {
        ULONG v = *(const ULONG *)(p + i);
        ULONG s = sum + v;
        carry += s < sum;
        sum = s;
    }
    ULONG s = sum + carry;
    return s + (s < sum);
}

static NTSTATUS ExpWrite(ULONG offset, ULONG length, const UCHAR *data) {
    for (ULONG i = 0; i < length; i += 2) {
        ULONG word = data[i] | ((ULONG)data[i + 1] << 8);
        if (ExpEepromValid && *(USHORT *)(ExpEeprom + offset + i) == word)
            continue;
        NTSTATUS status = HalWriteSMBusValue(EEPROM_ADDRESS, (UCHAR)(offset + i), TRUE, word);
        if (!NT_SUCCESS(status)) {
            ExpEepromValid = false;
            return status;
        }
        *(USHORT *)(ExpEeprom + offset + i) = (USHORT)word;
        KeStallExecutionProcessor(5000); // EEPROM write cycle
    }
    return STATUS_SUCCESS;
}

static const EEPROM_SETTING *ExpFind(ULONG Index, ULONG *sectionOffset, ULONG *sectionSize) {
    const EEPROM_SETTING *table;
    ULONG count;
    if (Index < 0x100) {
        table = ExpUserSettings, count = 20, *sectionOffset = 0x60, *sectionSize = 0x60;
    } else if (Index < 0x200) {
        table = ExpFactorySettings, count = 5, *sectionOffset = 0x30, *sectionSize = 0x30;
        Index -= 0x100;
    } else {
        return nullptr;
    }
    for (ULONG i = 0; i < count; i++)
        if (table[i].Index == Index)
            return &table[i];
    return nullptr;
}

XBAPI NTSTATUS NTAPI ExQueryNonVolatileSetting(ULONG ValueIndex, PULONG Type, PVOID Value, ULONG ValueLength,
                                               PULONG ResultLength) {
    if (ValueIndex == 0x104) { // game region (from the security section)
        if (ResultLength)
            *ResultLength = 4;
        if (ValueLength < 4)
            return STATUS_BUFFER_TOO_SMALL_;
        *Type = REG_DWORD;
        *(PULONG)Value = XeConsoleGameRegion;
        return STATUS_SUCCESS;
    }
    if (ValueIndex == 0xFFFF || ValueIndex == 0xFFFE) { // the whole EEPROM / its encrypted section
        ULONG n = ValueIndex == 0xFFFF ? 0x100 : 0x30;
        if (ResultLength)
            *ResultLength = n;
        if (ValueLength < n)
            return STATUS_BUFFER_TOO_SMALL_;
        *Type = REG_BINARY;
        ExpLock();
        NTSTATUS status = ExpLoadEeprom();
        if (NT_SUCCESS(status))
            RtlCopyMemory(Value, ExpEeprom, n);
        ExpUnlock();
        return status;
    }
    ULONG secOffset, secSize;
    const EEPROM_SETTING *s = ExpFind(ValueIndex, &secOffset, &secSize);
    if (!s)
        return STATUS_OBJECT_NAME_NOT_FOUND_;
    ExpLock();
    NTSTATUS status = ExpLoadEeprom();
    if (NT_SUCCESS(status) && ExpChecksum(ExpEeprom + secOffset, secSize) != 0xFFFFFFFF)
        status = STATUS_DEVICE_DATA_ERROR_;
    if (NT_SUCCESS(status)) {
        if (ValueLength < s->Size) {
            status = STATUS_BUFFER_TOO_SMALL_;
        } else {
            *Type = s->Type;
            if (ResultLength)
                *ResultLength = s->Size;
            RtlZeroMemory(Value, ValueLength);
            RtlCopyMemory(Value, ExpEeprom + secOffset + s->Offset, s->Size);
        }
    }
    ExpUnlock();
    return status;
}

XBAPI NTSTATUS NTAPI ExSaveNonVolatileSetting(ULONG ValueIndex, ULONG Type, PVOID Value, ULONG ValueLength) {
    ExpLock();
    NTSTATUS status;
    if (ValueIndex == 0xFFFF) {
        status = ValueLength == 0x100 ? ExpLoadEeprom() : STATUS_INVALID_PARAMETER_;
        if (NT_SUCCESS(status))
            status = ExpWrite(0, 0x100, (const UCHAR *)Value);
        ExpUnlock();
        return status;
    }
    ULONG secOffset, secSize;
    const EEPROM_SETTING *s = ValueIndex < 0x100 ? ExpFind(ValueIndex, &secOffset, &secSize) : nullptr;
    if (!s) {
        ExpUnlock();
        return STATUS_OBJECT_NAME_NOT_FOUND_;
    }
    if (ValueLength > s->Size) {
        ExpUnlock();
        return STATUS_INVALID_PARAMETER_;
    }
    UCHAR section[0x60];
    status = ExpLoadEeprom();
    if (NT_SUCCESS(status)) {
        RtlCopyMemory(section, ExpEeprom + secOffset, secSize);
        if (ExpChecksum(section, secSize) != 0xFFFFFFFF)
            RtlZeroMemory(section, secSize); // a corrupt user section starts over
        RtlZeroMemory(section + s->Offset, s->Size);
        RtlCopyMemory(section + s->Offset, Value, ValueLength);
        *(ULONG *)section = 0;
        *(ULONG *)section = ~ExpChecksum(section, secSize);
        status = ExpWrite(secOffset, secSize, section);
    }
    ExpUnlock();
    return status;
}

// Security section (0x00-0x2F): {HMAC-SHA1[20], confounder[8], HD key[16], game region[4]}, RC4-encrypted with
// HMAC(EEPROM key, digest) and authenticated by HMAC(EEPROM key, plaintext).
// HMAC-SHA1 from precomputed states (the SHA-1 state after the key ^ ipad block, and after the key ^ opad block):
// how the open image's key block describes the EEPROM keys (spec/01 §5).
static void ExpHmacFromStates(const ULONG *states, const UCHAR *data, ULONG length, PUCHAR digest) {
    struct {
        ULONG State[5];
        ULONG CountLow, CountHigh;
        UCHAR Buffer[64];
    } ctx; // the XcSHA context layout
    UCHAR inner[20];
    XcSHAInit((PUCHAR)&ctx);
    RtlCopyMemory(ctx.State, states, 20);
    ctx.CountLow = 64;
    XcSHAUpdate((PUCHAR)&ctx, data, length);
    XcSHAFinal((PUCHAR)&ctx, inner);
    XcSHAInit((PUCHAR)&ctx);
    RtlCopyMemory(ctx.State, states + 5, 20);
    ctx.CountLow = 64;
    XcSHAUpdate((PUCHAR)&ctx, inner, 20);
    XcSHAFinal((PUCHAR)&ctx, digest);
}

// One candidate EEPROM key (states == nullptr: XboxEEPROMKey). True when the section authenticates.
static bool ExpDecryptSecurity(const ULONG *states, PUCHAR plain) {
    UCHAR rc4Key[20], check[20], rc4[258];
    if (states)
        ExpHmacFromStates(states, ExpEeprom, 20, rc4Key);
    else
        XcHMAC(XboxEEPROMKey, 16, ExpEeprom, 20, nullptr, 0, rc4Key);
    RtlCopyMemory(plain, ExpEeprom + 20, 28);
    XcRC4Key(rc4, 20, rc4Key);
    XcRC4Crypt(rc4, 28, plain);
    if (states)
        ExpHmacFromStates(states, plain, 28, check);
    else
        XcHMAC(XboxEEPROMKey, 16, plain, 28, nullptr, 0, check);
    bool ok = true;
    for (int i = 0; i < 20; i++)
        ok &= check[i] == ExpEeprom[i];
    return ok;
}

volatile ULONG ExpEepromKeySource; // diagnostics: 0 = none worked, 1 = EEPROM key, 2 + n = key state n

void ExInitEeprom() {
    ExpLock();
    NTSTATUS status = ExpLoadEeprom();
    ExpUnlock();
    if (!NT_SUCCESS(status))
        return;
    UCHAR plain[28];
    bool ok = ExpDecryptSecurity(nullptr, plain);
    ExpEepromKeySource = ok ? 1 : 0;
    for (ULONG i = 0; !ok && i < KiEepromKeyStateCount; i++)
        if ((ok = ExpDecryptSecurity(KiEepromKeyStates[i], plain)))
            ExpEepromKeySource = 2 + i;
    if (!ok)
        return; // unknown EEPROM revision or key: keep the defaults
    RtlCopyMemory(XboxHDKey, plain + 8, 16);
    XeConsoleGameRegion = *(ULONG *)(plain + 24);
    ExpEepromSecurityValid = 1;
}

// Refurbishment record in sector 3 of the disk: {'RBRF', first-boot time (8), power cycle count}.
#include <xk/io.h>
XBAPI NTSTATUS NTAPI ExReadWriteRefurbInfo(PULONG RefurbInfo, ULONG ValueLength, BOOLEAN DoWrite) {
    if (ValueLength != 16)
        return STATUS_INVALID_PARAMETER_;
    OBJECT_STRING name;
    RtlInitAnsiString(&name, "\\Device\\Harddisk0\\partition0");
    OBJECT_ATTRIBUTES attrs = {nullptr, &name, OBJ_CASE_INSENSITIVE};
    IO_STATUS_BLOCK iosb;
    HANDLE h;
    NTSTATUS status = NtOpenFile(&h, (DoWrite ? GENERIC_WRITE : 0) | GENERIC_READ | SYNCHRONIZE, &attrs, &iosb,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_SYNCHRONOUS_IO_ALERT);
    if (!NT_SUCCESS(status))
        return status;
    auto sector = (PULONG)ExAllocatePoolWithTag(512, 'frxE');
    if (!sector) {
        NtClose(h);
        return (NTSTATUS)0xC000009A;
    }
    RtlZeroMemory(sector, 512);
    LARGE_INTEGER offset;
    offset.QuadPart = 0x600;
    if (!DoWrite) {
        status = NtReadFile(h, nullptr, nullptr, nullptr, &iosb, sector, 512, &offset);
        if (NT_SUCCESS(status)) {
            bool valid = sector[0] == 0x52465242;
            for (int i = 0; i < 4; i++)
                RefurbInfo[i] = valid ? sector[i] : 0;
        }
    } else {
        sector[0] = 0x52465242;
        for (int i = 1; i < 4; i++)
            sector[i] = RefurbInfo[i];
        status = NtWriteFile(h, nullptr, nullptr, nullptr, &iosb, sector, 512, &offset);
    }
    ExFreePool(sector);
    NtClose(h);
    return status;
}
