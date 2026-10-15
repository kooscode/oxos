// File system cache control (spec/16). Our file systems keep small write-through caches of their own, so the
// global cache size is bookkeeping only.
#include <xk/io.h>

static ULONG FscCachePages = 16;

XBAPI ULONG NTAPI FscGetCacheSize() { return FscCachePages; }

XBAPI NTSTATUS NTAPI FscSetCacheSize(ULONG NumberOfCachePages) {
    if (NumberOfCachePages > 0x800)
        return STATUS_INVALID_PARAMETER;
    FscCachePages = NumberOfCachePages;
    return STATUS_SUCCESS;
}

XBAPI void NTAPI FscInvalidateIdleBlocks() {} // nothing is cached dirty or idle beyond the volumes' own caches
