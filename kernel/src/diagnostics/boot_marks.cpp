// Start-up marks and the start-up watchdog (spec/oxos_internal/03 §8): CMOS records that name where a start-up that froze stopped, for the next boot to report.
// Off once the title starts, or once a last-words record owns the CMOS bytes (last_words.cpp).
#include <diagnostics/diagnostics.h>
#include <arch/arch.h>
#include <hardware/chipset/chipset.h>
#include <hardware/chipset/serial.h>

namespace xk::diagnostics {

volatile BOOLEAN BootMarksOn;
static ULONG BootMarkCount, PrevMark, PrevMarkCount;
[[maybe_unused]] static ULONG PrevMarkCaller;
static bool PrevMarkFound, PrevStallFound;
static ULONG PrevStallEip, PrevStallIrql, PrevStallSeconds;
extern "C" volatile ULONG LastMarkTick;
volatile ULONG LastMarkTick;         // WatchdogTicks at the last mark (the clock interrupt's start-up watchdog)
static volatile ULONG WatchdogTicks; // clock interrupts the watchdog has seen

void BootMark(const char *Format) {
    if (!BootMarksOn)
        return;
    ULONG flags = save_flags_cli(); // the index / data pairs must not interleave with another CMOS access
    if (BootMarkCount++ == 0)
        hardware::chipset::WriteCmos(0x70, 'P');
    for (int i = 0; i < 4; i++)
        hardware::chipset::WriteCmos((UCHAR)(0x71 + i), (UCHAR)((ULONG)Format >> (8 * i)));
    hardware::chipset::WriteCmos(0x75, (UCHAR)BootMarkCount);
    hardware::chipset::WriteCmos(0x76, (UCHAR)(BootMarkCount >> 8));
    if (LastMarkTick + 3000 <= WatchdogTicks)
        hardware::chipset::WriteCmos(0x77, 0); // a stall record of an earlier pause is stale now
    LastMarkTick = WatchdogTicks;
    restore_flags(flags);
}

// Start-up watchdog (clock interrupt, all builds): no mark for 3 s while the marks are on means start-up is stuck. Each tick from then on leaves the
// interrupted EIP and IRQL in CMOS ('W' 0x77, EIP 0x78-0x7B, IRQL 0x7C, seconds since the last mark 0x7D): a CPU that idles shows the idle loop, one that spins
// shows where. No record at all means the clock interrupt stopped too.
void BootMarkWatchdog(ULONG Eip, UCHAR Irql) {
    ULONG now = WatchdogTicks + 1;
    WatchdogTicks = now;
    if (!BootMarksOn || now - LastMarkTick < 3000)
        return;
    hardware::chipset::WriteCmos(0x77, 'W');
    for (int i = 0; i < 4; i++)
        hardware::chipset::WriteCmos((UCHAR)(0x78 + i), (UCHAR)(Eip >> (8 * i)));
    hardware::chipset::WriteCmos(0x7C, Irql);
    ULONG s = (now - LastMarkTick) / 1000;
    hardware::chipset::WriteCmos(0x7D, (UCHAR)(s > 255 ? 255 : s));
}

// The title is starting: the marks stop, and the record says start-up finished ('T').
void BootMarksDone() {
    BootMarksOn = FALSE;
    hardware::chipset::WriteCmos(0x70, 'T');
}

void TakePreviousBootMarks() {
    // The previous boot's start-up record, before this boot's first mark overwrites it.
    if (hardware::chipset::ReadCmos(0x70) == 'P') {
        PrevMarkFound = true;
        for (int i = 0; i < 4; i++)
            PrevMark |= (ULONG)hardware::chipset::ReadCmos((UCHAR)(0x71 + i)) << (8 * i);
        PrevMarkCount = hardware::chipset::ReadCmos(0x75) | hardware::chipset::ReadCmos(0x76) << 8;
        if (hardware::chipset::ReadCmos(0x77) == 'W') {
            PrevStallFound = true;
            for (int i = 0; i < 4; i++)
                PrevStallEip |= (ULONG)hardware::chipset::ReadCmos((UCHAR)(0x78 + i)) << (8 * i);
            PrevStallIrql = hardware::chipset::ReadCmos(0x7C);
            PrevStallSeconds = hardware::chipset::ReadCmos(0x7D);
        }
        hardware::chipset::WriteCmos(0x77, 0);
    }
    if (hardware::chipset::ReadCmos(0x70) != 'X' && hardware::chipset::ReadCmos(0x70) != 'B')
        BootMarksOn = TRUE; // else the fault record stays until ReportLastWords has shown it
}

void ReportPreviousBootMarks() {
    if (PrevMarkFound) {
        XK_SERIAL("*** previous boot stopped in start-up: last mark %08lx (mark %lu; the address of a log line's " "format string in that boot's image)\n",
                  PrevMark, PrevMarkCount);
    }
    if (PrevMarkFound) {
        XK_SERIAL("*** previous boot's last mark was set from a function called at %08lx (if that boot recorded it)\n", PrevMarkCaller);
    }
    if (PrevStallFound) {
        XK_SERIAL("*** previous boot's start-up watchdog: %lu s after that mark the clock interrupt still ran, last " "interrupted eip %08lx at IRQL %lu\n",
                  PrevStallSeconds, PrevStallEip, PrevStallIrql);
    } else if (PrevMarkFound) {
        XK_SERIAL("*** previous boot's start-up watchdog: no record (the clock interrupt stopped, or start-up " "had not stalled for 3 s)\n");
    }
}

} // namespace xk::diagnostics
