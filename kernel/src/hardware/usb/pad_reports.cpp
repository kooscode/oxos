// The pads' state while titles run (spec/oxos_internal/22 §9, spec/oxos_internal/34 §7.3). A title's own USB code (whatever it was built with) sets up its
// pads' interrupt transfers on the two OHCI host controllers; each finished transfer goes on the controller's done list, and the controller raises its
// interrupt. The kernel's interrupt dispatch calls ReadPadReports before the title's interrupt routine: it walks the done list and decodes the pads' input
// reports. It only reads: nothing is written to the controller or to the list.
#include <hardware/usb/pad_reports.h>
#include <hardware/chipset/chipset.h>
#include <diagnostics/diagnostics.h>
#include <memory/memory.h>
#include <arch/arch.h>

namespace xk::hardware::usb {

namespace {

// The OHCI registers (OHCI 1.0a §7): HcInterruptStatus and HcHCCA; in the HCCA, HccaDoneHead.
constexpr ULONG OHCI_INTERRUPT_STATUS = 0x0C, OHCI_HCCA = 0x18, HCCA_DONE_HEAD = 0x84;
constexpr ULONG OHCI_WRITEBACK_DONE_HEAD = 0x02;
constexpr ULONG DONE_LIST_LIMIT = 32;         // descriptors followed at one interrupt
constexpr ULONG RAM_LIMIT = 0x08000000;       // physical addresses read: inside 128 MB
constexpr ULONG LAST_RESORT = OXOS_PAD_LEFT_TRIGGER | OXOS_PAD_RIGHT_TRIGGER | OXOS_PAD_BACK | OXOS_PAD_BLACK; // spec/oxos_internal/48 §5
constexpr ULONG LAST_RESORT_MS = 3000;

// How long the last-resort combination has been held. A time with a flag of its own: KeTickCount | 1 (to keep it from 0) could be a tick ahead and wrap.
struct HELD_COMBINATION {
    bool Held;   // held in the last report
    ULONG Since; // KeTickCount of the first report that had it
};

// One pad, known by the physical address of its report buffer. Some titles' USB code (nxdk's, 2026-11-23) fills a new buffer for every report: their one
// pad then moves from slot to slot, so slots tell pads apart only for titles that keep their buffers (the XDK's).
struct PAD_SLOT {
    ULONG Buffer;           // the report's physical address; 0 = a free slot
    ULONG LastReport;       // KeTickCount of its last report
    HELD_COMBINATION Combo; // the last-resort combination on this slot's reports
    OXOS_PAD_STATE State;   // its last report
};

PAD_SLOT PadSlots[PAD_SLOTS];  // the pads seen since this boot
volatile LONG PadChanges;      // a bit for each slot whose buttons changed since TakePadChanges
// The buttons of the last report, any slot: a slot's first report counts as a change only when it differs from them. Kept across quick reboots, so buttons
// still held when the next title starts are no change (an in-game shortcut held into the title it started does not fire again there).
XK_PERSIST ULONG LastButtons;
HELD_COMBINATION AnyCombo;     // the last-resort combination over every report, any slot: titles that move one pad from slot to slot
ULONG ReadPage;                // the page the physical reads go through (StartPadReports), 0 before

// Copies Length bytes at physical address Physical through ReadPage: titles keep their USB buffers in their own memory, which the kernel's 0x80000000 view
// does not map. Interrupts are off meanwhile, so the two controllers' interrupts cannot share the page at once.
bool ReadPhysical(ULONG Physical, PVOID Out, ULONG Length) {
    if (!ReadPage || Physical >= RAM_LIMIT || Length > RAM_LIMIT - Physical)
        return false;
    auto out = (PUCHAR)Out;
    ULONG flags = save_flags_cli();
    ULONG *pte = memory::PteAddress(ReadPage), saved = *pte;
    for (ULONG i = 0; i < Length; i++) {
        ULONG p = Physical + i;
        if (i == 0 || !(p & 0xFFF)) {
            *pte = (p & ~0xFFFu) | PTE_PRESENT;
            memory::InvalidatePage(ReadPage);
        }
        out[i] = *(volatile UCHAR *)(ReadPage | (p & 0xFFF));
    }
    *pte = saved;
    memory::InvalidatePage(ReadPage);
    restore_flags(flags);
    return true;
}

// The slot of the pad whose report is at Buffer: its own, a free one, or the one that has gone longest without a report.
PAD_SLOT &SlotFor(ULONG Buffer) {
    PAD_SLOT *oldest = &PadSlots[0];
    for (auto &slot : PadSlots) {
        if (slot.Buffer == Buffer)
            return slot;
    }
    for (auto &slot : PadSlots) {
        if (!slot.Buffer)
            return slot;
        if (slot.LastReport - oldest->LastReport > 0x80000000u) // older (wrap-safe)
            oldest = &slot;
    }
    return *oldest;
}

// The last-resort combination in a report with Buttons: exactly its four buttons, nothing else held (as the in-game shortcuts match, spec/oxos_internal/48 §3).
void NoteCombination(HELD_COMBINATION &Combo, ULONG Buttons) {
    if (Buttons != LAST_RESORT) {
        Combo.Held = false;
    } else if (!Combo.Held) {
        Combo.Held = true;
        Combo.Since = KeTickCount;
    }
}

// One finished report at physical Buffer.
void RecordPadReport(ULONG Buffer, const UCHAR *Report) {
    OXOS_PAD_STATE state = {};
    if (!DecodePadReport(Report, state))
        return;
    ULONG flags = save_flags_cli();
    PAD_SLOT &slot = SlotFor(Buffer);
    ULONG port = (ULONG)(&slot - PadSlots);
    if (slot.Buffer != Buffer) { // a new pad, or a pad whose title moved it to a new buffer
        slot = {};
        slot.Buffer = Buffer;
        slot.State.Buttons = LastButtons;
    }
    state.Port = port;
    if (state.Buttons != slot.State.Buttons)
        PadChanges |= 1 << port;
    slot.State = state;
    slot.LastReport = KeTickCount;
    LastButtons = state.Buttons;
    NoteCombination(slot.Combo, state.Buttons);
    NoteCombination(AnyCombo, state.Buttons);
    restore_flags(flags);
}

} // namespace

void StartPadReports() {
    if (!ReadPage)
        ReadPage = (ULONG)MmMapIoSpace(0, PAGE_SIZE, PAGE_READWRITE);
}

void ReadPadReports(ULONG Irq) {
    auto hc = (volatile ULONG *)(Irq == 1 ? 0xFED00000 : 0xFED08000);
    if (!(hc[OHCI_INTERRUPT_STATUS / 4] & OHCI_WRITEBACK_DONE_HEAD))
        return;
    ULONG hcca = hc[OHCI_HCCA / 4], td = 0;
    if (!hcca || !ReadPhysical(hcca + HCCA_DONE_HEAD, &td, 4))
        return;
    for (ULONG n = 0; (td &= ~0xFu) && n < DONE_LIST_LIMIT; n++) {
        // A general transfer descriptor: flags (completion code in bits 28-31), current buffer pointer, next descriptor, buffer end.
        ULONG d[4];
        if (!ReadPhysical(td, d, sizeof(d)))
            return;
        // A pad's report is 20 bytes: a transfer that filled its buffer has cleared the pointer (the report ends at the buffer end); a short one leaves the
        // pointer just past the data.
        UCHAR report[20];
        ULONG start = d[1] ? d[1] - sizeof(report) : d[3] - (sizeof(report) - 1);
        if (!(d[0] >> 28) && d[3] && ReadPhysical(start, report, sizeof(report)))
            RecordPadReport(start, report);
        td = d[2];
    }
}

ULONG TakePadChanges() {
    return (ULONG)InterlockedExchange(&PadChanges, 0);
}

BOOLEAN GetPadState(ULONG Port, OXOS_PAD_STATE &State) {
    if (Port >= PAD_SLOTS)
        return FALSE;
    ULONG flags = save_flags_cli();
    BOOLEAN seen = PadSlots[Port].Buffer != 0;
    if (seen)
        State = PadSlots[Port].State;
    restore_flags(flags);
    return seen;
}

// Held 3 s on one slot (a title that keeps its buffers, any number of pads), or over every report (a title that moves its pad from buffer to buffer: with
// one pad; a second pad's reports without the combination start it over).
void CheckPadLastResort() {
    bool held = AnyCombo.Held && KeTickCount - AnyCombo.Since >= LAST_RESORT_MS;
    for (const auto &slot : PadSlots)
        held |= slot.Combo.Held && KeTickCount - slot.Combo.Since >= LAST_RESORT_MS;
    if (!held)
        return;
    XK_SERIAL("pads: LT RT BACK BLACK held 3 s: power cycle\n");
    chipset::PowerCycle();
}

} // namespace xk::hardware::usb

