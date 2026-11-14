// Test extension in plain C (spec/oxos_internal/34 §9.3): the SDK's header, descriptor macro and helpers work without C++, and the compiler's calls to memcpy,
// memset, memcmp and the 64-bit division helpers reach the kernel's exports (OXOS's own ordinals 402-415). It records the results in its state page for
// tests/boot_test.py.
#include <oxos_sdk.h>

#define SAVED_MAGIC 0x434E4C50UL // "PLNC"

// The state page as boot_test.py reads it (words, in this order).
typedef struct SAVED {
    ULONG Magic;
    ULONG Launches;   // starts since the cold boot
    ULONG Loaded;     // title-loaded events
    ULONG DivisionOk; // 64-bit division and remainder, signed and unsigned, at every start
    ULONG MemoryOk;   // a struct copy, memset and memcmp at every start
    ULONG SettingOk;  // oxos_setting("greeting") was "hello" at every start
} SAVED;

// Large enough that the compiler copies it with a call to memcpy.
typedef struct BLOCK {
    ULONG Words[64];
} BLOCK;

static SAVED *Saved;

static BOOLEAN ExtensionStart(void);

OXOS_DEFINE_EXTENSION(.Name = "plain_c", .Version = "1.0.0", .Start = ExtensionStart);

static void CountLoaded(const OXOS_TITLE_LOADED *Title, void *UserData) {
    Saved->Loaded++;
}

// Divides values the compiler cannot know, so it calls _aulldiv, _aullrem, _alldiv and _allrem.
static ULONG DivisionWorks(void) {
    volatile ULONGLONG dividend = 0x123456789ABCDEF0ULL, divisor = 0x1234567;
    volatile LONGLONG negative = -0x123456789ABCDEFLL, thousand = 1000;
    return dividend / divisor == 0x1000000790ULL && dividend % divisor == 0x400 && negative / thousand == -0x4A90BE587DE6LL && negative % thousand == -0x37F;
}

static ULONG MemoryWorks(void) {
    static BLOCK source, copy;
    for (ULONG i = 0; i < 64; i++)
        source.Words[i] = i * 0x01010101UL;
    copy = source;
    if (memcmp(&copy, &source, sizeof(BLOCK)) != 0)
        return 0;
    memset(&copy, 0xA5, sizeof(BLOCK));
    return copy.Words[63] == 0xA5A5A5A5UL && memcmp(&copy, &source, sizeof(BLOCK)) != 0;
}

static ULONG SameText(const char *A, const char *B) {
    while (A && *A && *A == *B) {
        A++;
        B++;
    }
    return A && *A == *B;
}

static BOOLEAN ExtensionStart(void) {
    Saved = (SAVED *)oxos_state();
    if (!Saved->Launches) { /* the first launch: the page is all zero */
        Saved->Magic = SAVED_MAGIC;
        Saved->DivisionOk = Saved->MemoryOk = Saved->SettingOk = 1;
    }
    Saved->Launches++;
    if (!DivisionWorks())
        Saved->DivisionOk = 0;
    if (!MemoryWorks())
        Saved->MemoryOk = 0;
    if (!SameText(oxos_setting("greeting"), "hello"))
        Saved->SettingOk = 0;
    oxos_log("plain C extension, launch %lu", Saved->Launches);
    return oxos_on_title_loaded(CountLoaded, NULL);
}
