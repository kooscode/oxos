// Boot-time bump allocator, used until the memory manager exists (milestone 2c).
// The 2BL maps physical 0-256 MB at 0x80000000; the window below sits above the kernel image and
// below the 2BL image (which KiSystemStartup wipes). Memory handed out here is never freed.
#include <xk/ke.h>

static constexpr ULONG EARLY_POOL_START = 0x80100000;
static constexpr ULONG EARLY_POOL_END = 0x80400000;
static ULONG MmEarlyNext = EARLY_POOL_START;

PVOID MmEarlyAllocate(SIZE_T Size, SIZE_T Alignment) {
    ULONG p = (MmEarlyNext + Alignment - 1) & ~(Alignment - 1);
    if (p + Size > EARLY_POOL_END || p + Size < p)
        return nullptr;
    MmEarlyNext = p + Size;
    return (PVOID)p;
}
