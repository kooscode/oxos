// Last words: a record in CMOS that survives a power cycle (a serial build prints it at the next boot). CMOS bytes 0x70-0x7F (free; 0x10-0x6F and 0x80-0xFF
// hold the clock signature): 'X', vector, EIP, MCG_STATUS low, bank 1 status low and high; or 'B' and a bug check.
#include <diagnostics/diagnostics.h>
#include <arch/arch.h>
#include <hardware/chipset/chipset.h>

namespace xk::diagnostics {

static void CmosPut32(UCHAR at, ULONG v) {
    for (int i = 0; i < 4; i++)
        hardware::chipset::WriteCmos((UCHAR)(at + i), (UCHAR)(v >> (8 * i)));
}

[[maybe_unused]] static ULONG CmosGet32(UCHAR at) {
    ULONG v = 0;
    for (int i = 0; i < 4; i++)
        v |= (ULONG)hardware::chipset::ReadCmos((UCHAR)(at + i)) << (8 * i);
    return v;
}

void SaveExceptionLastWords(ULONG Vector, ULONG Eip) {
    BootMarksOn = FALSE;
    hardware::chipset::WriteCmos(0x70, 'X');
    hardware::chipset::WriteCmos(0x71, (UCHAR)Vector);
    CmosPut32(0x72, Eip);
    if (MachineCheckBanks) {
        CmosPut32(0x76, (ULONG)rdmsr(0x17A));
        ULONGLONG st = rdmsr(0x405);
        CmosPut32(0x7A, (ULONG)st);
        hardware::chipset::WriteCmos(0x7E, (UCHAR)(st >> 56));
    }
}

// Bug checks: 'B', the code (0x71), the first parameter (0x75) and the caller (0x79); for 0x1E the access violation's address (0x75) and the faulting eip
// (0x79).
void SaveBugCheckLastWords(ULONG Code, ULONG P1, ULONG Caller) {
    BootMarksOn = FALSE;
    hardware::chipset::WriteCmos(0x70, 'B');
    CmosPut32(0x71, Code);
    CmosPut32(0x75, P1);
    CmosPut32(0x79, Caller);
}

void ReportLastWords() {
    if (hardware::chipset::ReadCmos(0x70) == 'B') {
        if (CmosGet32(0x71) == 0x1E) {
            XK_SERIAL("*** previous boot ended in bug check 0x1e: unhandled exception at eip %08lx (an access " "violation's address: %08lx)\n",
                      CmosGet32(0x79), CmosGet32(0x75));
        } else {
            XK_SERIAL("*** previous boot ended in bug check %#lx (%#lx), called from %08lx\n", CmosGet32(0x71), CmosGet32(0x75), CmosGet32(0x79));
        }
        hardware::chipset::WriteCmos(0x70, 0);
        BootMarksOn = TRUE;
        return;
    }
    if (hardware::chipset::ReadCmos(0x70) != 'X')
        return;
    XK_SERIAL("*** previous boot ended in exception %u at eip %08lx; MCG_STATUS %08lx, bank 1 status %02x..%08lx\n", hardware::chipset::ReadCmos(0x71),
              CmosGet32(0x72), CmosGet32(0x76), hardware::chipset::ReadCmos(0x7E), CmosGet32(0x7A));
    hardware::chipset::WriteCmos(0x70, 0);
    BootMarksOn = TRUE;
}

} // namespace xk::diagnostics
