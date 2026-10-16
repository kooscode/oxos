// File system cache control (spec/16). The cache is the FATX block cache shared by all FATX volumes (16 pages by
// default, like the reference); XDVDFS/UDF/RAW read straight from their devices.
#include <xk/io.h>

NTSTATUS FatxSetCacheSize(ULONG Pages);
ULONG FatxGetCacheSize();

XBAPI ULONG NTAPI FscGetCacheSize() { return FatxGetCacheSize(); }

XBAPI NTSTATUS NTAPI FscSetCacheSize(ULONG NumberOfCachePages) {
    if (NumberOfCachePages > 0x800)
        return STATUS_INVALID_PARAMETER;
    return FatxSetCacheSize(NumberOfCachePages);
}

XBAPI void NTAPI FscInvalidateIdleBlocks() {} // nothing is cached dirty or idle beyond the volumes' own caches
