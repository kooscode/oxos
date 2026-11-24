// The sound recovery module (spec/oxos_internal/46 §3, 43): a sound codec whose cold reset was asserted after it had been running can stop clocking the
// AC-link until the console's power is removed (2026-11-23). At every quick reboot, before the next title is chosen, the AC-link is tested; a frozen codec is
// shown on screen, the launch about to happen is saved in C:\OXOS\cache\oxos_sound_recovery.bin, and the console is powered off and on. At the cold boot the
// saved launch is made again, without the boot video. A guard keeps a title that freezes the codec at every start from cycling the console for ever.
#include "modules.h"
#include <core/core.h>
#include <diagnostics/diagnostics.h>
#include <hardware/chipset/chipset.h>
#include <io/io.h>
#include <memory/memory.h>
#include <runtime/runtime.h>
#include <storage/storage.h>
#include <toasts/toasts.h>
#include <titles/titles.h>

namespace xk::modules {

namespace {

// --- The AC-link test: the PCM output's DMA run over a silent buffer. It never reads or writes a codec register: on the MCPX a codec access with no codec
// answering hangs the console.

// The AC'97 controller's registers (spec/stock/25): the global control and the PCM output channel's bus-master registers.
constexpr ULONG AC97_GLOB_CNT = 0xFEC0012C;
constexpr ULONG GLOB_CNT_COLD_RESET_RELEASED = 0x02;   // GLOB_CNT bit 1: the codec is out of cold reset
constexpr ULONG PCM_OUT_BDBAR = 0xFEC00110, PCM_OUT_LVI = 0xFEC00115, PCM_OUT_SR = 0xFEC00116, PCM_OUT_PICB = 0xFEC00118,
                PCM_OUT_CR = 0xFEC0011B;
constexpr UCHAR CR_RUN = 0x01, CR_RESET = 0x02;        // the channel's control: DMA running, channel reset
constexpr USHORT SR_CLEAR_ALL = 0x1C;                  // the status bits that clear when written as 1 (LVBCI, BCIS, FIFOE)
constexpr ULONG DESCRIPTOR_BUFFER_UNDERRUN = 1u << 30; // a descriptor's control: past the last buffer, send silence
constexpr ULONG SILENT_SAMPLES = 1024;                 // 16-bit samples in the test's buffer: 512 stereo frames, about 11 ms at 48 kHz
constexpr ULONG CLOCK_WAIT_MS = 20;                    // a running codec moves the position within a fraction of a millisecond

template <typename T> volatile T &Register(ULONG Address) {
    return *(volatile T *)Address;
}

// The PCM output channel stopped and back in its reset state (as the stock kernel's sound teardown leaves it, spec/stock/25 §4), at most 1 ms.
void ResetPcmOut() {
    Register<UCHAR>(PCM_OUT_CR) = 0;
    Register<UCHAR>(PCM_OUT_CR) = CR_RESET;
    for (ULONG us = 0; us < 1000 && (Register<UCHAR>(PCM_OUT_CR) & CR_RESET); us += 10)
        KeStallExecutionProcessor(10);
    Register<USHORT>(PCM_OUT_SR) = SR_CLEAR_ALL;
    Register<ULONG>(PCM_OUT_BDBAR) = 0;
}

// True when the codec drives the AC-link: the DMA's position moves. A codec held in reset (as after power-on) counts as working: the next title's release
// starts it cleanly.
bool IsSoundLinkClocking() {
    if (!(Register<ULONG>(AC97_GLOB_CNT) & GLOB_CNT_COLD_RESET_RELEASED))
        return true;
    auto page = (PULONG)MmAllocateContiguousMemory(PAGE_SIZE); // the descriptor list (one entry) at its start, the silent buffer after it
    if (!page)
        return true;
    memset(page, 0, PAGE_SIZE);
    ULONG physical = MmGetPhysicalAddress(page);
    page[0] = physical + 0x100;
    page[1] = SILENT_SAMPLES | DESCRIPTOR_BUFFER_UNDERRUN;
    ResetPcmOut();
    Register<ULONG>(PCM_OUT_BDBAR) = physical;
    Register<UCHAR>(PCM_OUT_LVI) = 0;
    Register<UCHAR>(PCM_OUT_CR) = CR_RUN;
    // The controller loads the descriptor without the codec (the position count becomes the buffer's length; a frozen codec's too, 2026-11-23); only the
    // codec's clock then counts it down. So the count is taken once loaded, and must go down from there.
    KeStallExecutionProcessor(1000);
    USHORT start = Register<USHORT>(PCM_OUT_PICB), now = start;
    for (ULONG ms = 0; ms < CLOCK_WAIT_MS && now >= start; ms++) {
        KeStallExecutionProcessor(1000);
        now = Register<USHORT>(PCM_OUT_PICB);
    }
    bool moved = start && now < start;
    ResetPcmOut();
    MmFreeContiguousMemory(page);
    return moved;
}

// --- The saved launch

constexpr char RECOVERY_FILE[] = "\\Device\\Harddisk0\\Partition2\\OXOS\\cache\\oxos_sound_recovery.bin"; // in the kernel's cache folder on C:
constexpr ULONG RECOVERY_MAGIC = 0x444E5358;                                                          // "XSND"
constexpr ULONG POOL_TAG = 'cRsX';                                                                    // the file's copy in pool memory

enum RECOVERY_STATE : ULONG {
    RECOVERY_PENDING = 1,    // written before the power cycle: the cold boot makes the saved launch
    RECOVERY_RELAUNCHED = 2, // the cold boot made it: a frozen codec again before a healthy test is the guard's case
};

// The file: the launch the quick reboot was about to make (the launch data page, when there was one) and the disc images mounted then.
struct RECOVERY_FILE_DATA {
    ULONG Magic;              // RECOVERY_MAGIC
    RECOVERY_STATE State;     // pending until the cold boot has made the launch
    ULONG HasLaunchData;      // 1 when LaunchData holds the launch data page
    UCHAR LaunchData[0x1000]; // the launch data page (titles::LaunchDataPage) as the quick reboot had it
    ULONG MountsSize;         // the bytes of Mounts used (storage::CopyVirtualDiscMounts)
    UCHAR Mounts[0x800];      // the disc images mounted then
};

RECOVERY_FILE_DATA *ReadRecoveryFile() {
    ULONG got;
    NTSTATUS status;
    auto data = (RECOVERY_FILE_DATA *)titles::ReadOXOSFile(RECOVERY_FILE, sizeof(RECOVERY_FILE_DATA), POOL_TAG, &got, &status);
    if (data && (got != sizeof(*data) || data->Magic != RECOVERY_MAGIC)) {
        ExFreePool(data);
        return nullptr;
    }
    return data;
}

bool WriteRecoveryFile(const RECOVERY_FILE_DATA &Data) {
    HANDLE file;
    if (!NT_SUCCESS(io::CreateFile(RECOVERY_FILE, GENERIC_WRITE, 0, FILE_OVERWRITE_IF, FILE_NON_DIRECTORY_FILE, &file)))
        return false;
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER at = {};
    NTSTATUS status = NtWriteFile(file, nullptr, nullptr, nullptr, &iosb, (PVOID)&Data, sizeof(Data), &at);
    NtClose(file);
    return NT_SUCCESS(status);
}

} // namespace

bool CheckSoundAtQuickReboot() {
    RECOVERY_FILE_DATA *saved = ReadRecoveryFile();
    if (IsSoundLinkClocking()) {
        if (saved) { // a recovery worked: the guard starts over
            ExFreePool(saved);
            io::DeleteFile(RECOVERY_FILE);
        }
        return false;
    }
    // The guard: frozen again since the last recovery's relaunch, before any healthy test. No cycle; the title starts without sound and a toast says why.
    if (saved && saved->State == RECOVERY_RELAUNCHED) {
        ExFreePool(saved);
        io::DeleteFile(RECOVERY_FILE);
        XK_SERIAL("sound: the codec is frozen again after a recovery: no second power cycle\n");
        return true;
    }
    if (saved)
        ExFreePool(saved);
    auto data = (RECOVERY_FILE_DATA *)ExAllocatePoolWithTag(sizeof(RECOVERY_FILE_DATA), POOL_TAG);
    if (!data)
        return true;
    memset(data, 0, sizeof(*data));
    data->Magic = RECOVERY_MAGIC;
    data->State = RECOVERY_PENDING;
    if (LaunchDataPage) {
        data->HasLaunchData = 1;
        memcpy(data->LaunchData, LaunchDataPage, sizeof(data->LaunchData));
    }
    data->MountsSize = storage::CopyVirtualDiscMounts(data->Mounts, sizeof(data->Mounts));
    [[maybe_unused]] bool written = WriteRecoveryFile(*data);
    ExFreePool(data);
    XK_SERIAL("sound: the codec is frozen (the AC-link has no clock): power cycle, then the same launch%s\n",
              written ? "" : " (not saved: the dashboard follows)");
    toasts::ShowNotice("Audio Codec Frozen - Restarting");
    LARGE_INTEGER wait;
    wait.QuadPart = -2000LL * 10000; // 2 s to read it
    KeDelayExecutionThread(KernelMode, FALSE, &wait);
    hardware::chipset::PowerCycle();
    return true;
}

bool RestoreSoundRecoveryLaunch() {
    RECOVERY_FILE_DATA *saved = ReadRecoveryFile();
    if (!saved)
        return false;
    if (saved->State != RECOVERY_PENDING) { // left from an earlier recovery: this cold boot is an ordinary one
        ExFreePool(saved);
        io::DeleteFile(RECOVERY_FILE);
        return false;
    }
    if (saved->HasLaunchData) {
        auto page = (PUCHAR)MmAllocateContiguousMemory(0x1000);
        if (page) {
            MmPersistContiguousMemory(page, 0x1000, TRUE);
            memcpy(page, saved->LaunchData, 0x1000);
            LaunchDataPage = page;
        }
    }
    storage::RestoreVirtualDiscMountsFrom(saved->Mounts, saved->MountsSize);
    saved->State = RECOVERY_RELAUNCHED;
    WriteRecoveryFile(*saved);
    ExFreePool(saved);
    XK_SERIAL("sound: after the power cycle, the launch the codec's freeze interrupted\n");
    return true;
}

} // namespace xk::modules
