#!/usr/bin/env python3
"""Boot a BIOS image in xemu and check milestone conditions through the QMP monitor.

The kernel's boot self-tests (tests/kernel_self_tests/kernel_self_tests.cpp) leave results in globals that are located through the linker map and read with HMP
`x`.

usage: boot_test.py --bios build/kernel/self_tests/bios.bin --map build/kernel/self_tests/oxoskrnl.map [--mcpx-rom mcpx.bin] [--xemu PATH]
"""
import argparse
import hashlib
import hmac, json, os, re, shutil, struct, subprocess, sys, tempfile, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qcow2 import Qcow2
import exfat  # noqa: E402
from fatx import FATX, FATXWriter  # noqa: E402
import mkxiso  # noqa: E402
import mkudf  # noqa: E402
import mkcci  # noqa: E402
from controller_test import qmp, xemu_env  # noqa: E402
from virtual_gamepad import XEMU_PORT1_BINDING, VirtualPad  # noqa: E402


def map_symbols(path):
    syms = {}
    for line in open(path):
        m = re.match(r"\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})", line)
        if m:
            syms[m.group(1)] = int(m.group(2), 16)
    return syms


class Monitor:
    def __init__(self, path, syms):
        self.path, self.syms = path, syms

    def hmp(self, cmd):
        r = qmp(self.path, "human-monitor-command", **{"command-line": cmd})  # fails the test when xemu does not answer (it froze)
        return r.get("return", r)

    def addr(self, name):
        """A symbol's address: a C name, a global C++ name, or a namespaced one ("hardware::ide::DiskLengthInSectors", in
        namespace xk), as the linker map spells them."""
        if "::" in name:
            parts = name.split("::")
            parts = parts if parts[0] == "xk" else ["xk"] + parts
            prefix = "?" + parts[-1] + "@" + "@".join(reversed(parts[:-1])) + "@@"
        else:
            prefix = f"?{name}@@"
        for k, v in self.syms.items():
            if k in ("_" + name, name) or k.startswith(prefix):
                return v
        sys.exit(f"symbol {name} not in map")

    def read(self, name):
        out = self.hmp(f"x/1wx {self.addr(name):#x}")
        return int(out.split(":")[1].split()[0], 16)

    def read_offset(self, name, offset):
        out = self.hmp(f"x/1wx {self.addr(name) + offset:#x}")
        return int(out.split(":")[1].split()[0], 16)

    def pause(self):
        """Stops the emulated CPU: what the checks read next cannot change under them."""
        self.hmp("stop")

    def resume(self):
        self.hmp("cont")

    def rate(self, name, seconds=2.0):
        return self.rates([name], seconds)[0]

    def rates(self, names, seconds=2.0):
        """The rates of several counters over the same window."""
        v0, t0 = [self.read(n) for n in names], time.monotonic()
        time.sleep(seconds)
        v1, t1 = [self.read(n) for n in names], time.monotonic()
        return [(b - a) / (t1 - t0) for a, b in zip(v0, v1)]


def run_checks(mon, hdd=None, title_map=None):
    exc = mon.read("core::LastExceptionVector")
    tick_rate = mon.rate("KeTickCount")
    timer_rate = mon.rate("KiTestTimerTicks")
    eip = int(re.search(r"EIP=([0-9a-f]+)", mon.hmp("info registers")).group(1), 16)
    values = {name: mon.read(name) for name in (
        "core::BootStage", "KiTestDpcCount", "KiTestOneShotFired", "KiTestEventState",
        "KiTestRtcCount", "KiTestRtcDisconnected", "KiTestRtcIrqlOk")}
    m2b = {name: mon.read(name) for name in ("core::LastBugCheckCode", "KiTestPingA", "KiTestPingB", "KiTestSleepMs", "KiTestSemaphoreConsumed",
        "KiTestMutantOk", "KiTestWaitAnyIndex", "KiTestWaitAllStatus", "KiTestTimeoutStatus", "KiTestJoinOk", "KiTestFpuOk", "KiTestThreadsDone")}
    m2bp = {name: mon.read(name) for name in ("KiTestApcKernelRan", "KiTestApcNormalAtPassive", "KiTestApcRewaitOk", "KiTestSpecialApcAtApcLevel",
        "KiTestUserApcRan", "KiTestUserApcStatus", "KiTestAlertStatus", "KiTestCriticalRegionOk", "KiTestSuspendOk", "KiTestQueueProcessed",
        "KiTestQueueTimeout", "KiTestDeviceQueueOk", "KiTestBoostPriority", "KiTestApcSuiteDone")}
    m2c = {name: mon.read(name) for name in ("KiTestMmContiguousOk", "KiTestMmAlignedOk", "KiTestMmReuseOk", "KiTestMmRelocateOk", "KiTestMmSystemOk",
        "KiTestMmIoSpaceOk", "KiTestMmPoolOk", "KiTestMmLayoutOk", "KiTestMmThreadChurnPages", "KiTestMmContiguousVa", "KiTestMmDone",
        "KiTestVmReserveCommitOk", "KiTestVmImageOk", "KiTestVmTopDownOk", "KiTestVmFreeRulesOk", "KiTestVmProtectOk", "KiTestVmPagesRestored",
        "KiTestVmStatsOk")}
    m3a = {name: mon.read(name) for name in ("KiTestObHandlesOk", "KiTestObNamedOk", "KiTestObNamespaceOk", "KiTestObNameErrorsOk", "KiTestPsThreadOk",
        "KiTestPsNotifyCount", "KiTestNtApcOk", "KiTestNtWaitMultipleOk", "KiTestObDuplicateOk", "KiTestObPoolBalanced", "KiTestObDone")}
    m3b = {name: mon.read(name) for name in ("KiTestIoOpenOk", "KiTestIoSyncRwOk", "KiTestIoQueryOk", "KiTestIoAsyncOk", "KiTestIoApcOk", "KiTestIoPortOk",
        "KiTestIoIoctlOk", "KiTestIoRelativeOk", "KiTestIoErrorsOk", "KiTestIoStartIoOk", "KiTestIoCloseOk", "KiTestIoDeleteOk", "KiTestIoPoolBalanced",
        "KiTestIoDone")}
    # The spinners (priority 1) get what the test suites leave over, which changes from one second to the next: both are measured over the same second, up to 5
    # times until both have run.
    for _ in range(5):
        spin_a, spin_b = mon.rates(["KiTestSpinA", "KiTestSpinB"], 1.0)
        if spin_a > 0 and spin_b > 0 and min(spin_a, spin_b) / max(spin_a, spin_b) > 0.3:
            break
    switches = mon.read_offset("KiPcr", 0x28 + 0x20)

    code, at = mon.read("core::LastExceptionCode"), mon.read("core::LastExceptionEip")
    print(f"exception vector {exc:#x} code {code:#x} at {at:#x}, EIP {eip:#x}, clock {tick_rate:.0f}/s, " f"periodic timer DPC {timer_rate:.1f}/s")
    print("  " + ", ".join(f"{k}={v}" for k, v in values.items()))
    print("  " + ", ".join(f"{k}={v:#x}" if k.endswith(("Status", "Code")) else f"{k}={v}" for k, v in m2b.items()))
    print("  " + ", ".join(f"{k}={v:#x}" if k.endswith("Status") or k.endswith("Timeout") else f"{k}={v}" for k, v in m2bp.items()))
    print("  " + ", ".join(f"{k}={v:#x}" if k.endswith("Va") else f"{k}={v}" for k, v in m2c.items()))
    print("  " + ", ".join(f"{k}={v}" for k, v in m3a.items()))
    print(f"  spinner rates {spin_a:.0f}/s and {spin_b:.0f}/s, context switches {switches}")
    return {
        "M1": {
            "reached idle (stage 0x30)": values["core::BootStage"] == 0x30,
            "no exception": exc == 0xFFFFFFFF,
            "clock ~1000 ticks/s": 900 <= tick_rate <= 1100,
            "EIP inside kernel": 0x80010000 <= eip < 0x80100000,
        },
        "M2a": {
            "DPC queued at boot ran once": values["KiTestDpcCount"] == 1,
            "10 ms periodic timer ~100 DPCs/s": 90 <= timer_rate <= 110,
            "300 ms one-shot timer fired once": values["KiTestOneShotFired"] == 1,
            "event set from timer DPC": values["KiTestEventState"] == 1,
            "RTC KINTERRUPT serviced 16 times": values["KiTestRtcCount"] == 16,
            "ISR ran at its IRQL": values["KiTestRtcIrqlOk"] == 1,
            "interrupt disconnected from a DPC": values["KiTestRtcDisconnected"] == 1,
        },
        "M2b": {
            "no bug check": m2b["core::LastBugCheckCode"] == 0,
            "event ping-pong 1000 rounds each": m2b["KiTestPingA"] == 1000 and m2b["KiTestPingB"] == 1000,
            "10 x 50 ms sleep ~500 ms": 495 <= m2b["KiTestSleepMs"] <= 530,
            "semaphore: 100 releases consumed": m2b["KiTestSemaphoreConsumed"] == 100,
            "mutant: recursion + hand-off to waiter": m2b["KiTestMutantOk"] == 1,
            "WaitAny returns index of timer": m2b["KiTestWaitAnyIndex"] == 1,
            "WaitAll on signalled events -> WAIT_0": m2b["KiTestWaitAllStatus"] == 0,
            "wait timeout -> STATUS_TIMEOUT": m2b["KiTestTimeoutStatus"] == 0x102,
            "join: terminated thread is signalled": m2b["KiTestJoinOk"] == 1,
            "x87 state survives switches (2 threads)": m2b["KiTestFpuOk"] == 2,
            "controller thread finished": m2b["KiTestThreadsDone"] == 1,
            "equal-priority spinners both run (quantum)": spin_a > 0 and spin_b > 0
                and min(spin_a, spin_b) / max(spin_a, spin_b) > 0.3,
        },
        "M2b+": {
            "kernel APC routines ran (3 APCs share the counter)": m2bp["KiTestApcKernelRan"] == 3,
            "normal kernel APC routine at PASSIVE": m2bp["KiTestApcNormalAtPassive"] == 1,
            "wait resumed after kernel APC": m2bp["KiTestApcRewaitOk"] == 1,
            "special APC at APC_LEVEL": m2bp["KiTestSpecialApcAtApcLevel"] == 1,
            "user APC ran and ended alertable wait": m2bp["KiTestUserApcRan"] == 1 and m2bp["KiTestUserApcStatus"] == 0xC0,
            "alert -> STATUS_ALERTED": m2bp["KiTestAlertStatus"] == 0x101,
            "critical region defers kernel APC": m2bp["KiTestCriticalRegionOk"] == 1,
            "suspend stops / resume restarts a thread": m2bp["KiTestSuspendOk"] == 1,
            "queue: 10 items processed": m2bp["KiTestQueueProcessed"] == 10,
            "queue: KeRemoveQueue timeout": m2bp["KiTestQueueTimeout"] == 0x102,
            "device queue semantics": m2bp["KiTestDeviceQueueOk"] == 1,
            "wake-up boost 8 + 2 = 10": m2bp["KiTestBoostPriority"] == 10,
            "APC suite finished": m2bp["KiTestApcSuiteDone"] == 1,
        },
        "M2c": {
            "contiguous: VA=0x80000000|PA, top-down, size, data": m2c["KiTestMmContiguousOk"] == 1,
            "contiguous Ex: <16 MB, 64 KB aligned, write-combine": m2c["KiTestMmAlignedOk"] == 1,
            "free + realloc reuses block, pages restored": m2c["KiTestMmReuseOk"] == 1,
            "contiguous request moves an unlocked page in use, not a locked one": m2c["KiTestMmRelocateOk"] == 1,
            "system memory mapped/unmapped": m2c["KiTestMmSystemOk"] == 1,
            "MmMapIoSpace alias + MMIO pass-through": m2c["KiTestMmIoSpaceOk"] == 1,
            "pool: 500 blocks + large block": m2c["KiTestMmPoolOk"] == 1,
            "no user mappings, RO text, stack guard page": m2c["KiTestMmLayoutOk"] == 1,
            "150 thread lifetimes: no per-thread leak (<= 4 pages)": m2c["KiTestMmThreadChurnPages"] <= 4,
            "VM: reserve 1 MB, commit 64 KB (zeroed, RW), query": m2c["KiTestVmReserveCommitOk"] == 1,
            "VM: NtProtectVirtualMemory RW -> RO": m2c["KiTestVmProtectOk"] == 1,
            "VM: free rules (not-at-base, decommit, release)": m2c["KiTestVmFreeRulesOk"] == 1,
            "VM: XBE-style image at 0x10000 + conflict": m2c["KiTestVmImageOk"] == 1,
            "VM: MEM_TOP_DOWN": m2c["KiTestVmTopDownOk"] == 1,
            "VM: all pages returned": m2c["KiTestVmPagesRestored"] == 1,
            "MmQueryStatistics": m2c["KiTestVmStatsOk"] == 1,
            "memory suite finished": m2c["KiTestMmDone"] == 1,
        },
        "M3a": {
            "handles: multiples of 4, LIFO reuse, invalid handle": m3a["KiTestObHandlesOk"] == 1,
            "named objects: OPENIF, collision, open by name, root -4, deleted on last close": m3a["KiTestObNamedOk"] == 1,
            "directory + \\??\\Q: symlink + link/dir queries": m3a["KiTestObNamespaceOk"] == 1,
            "name errors: path/name not found, invalid": m3a["KiTestObNameErrorsOk"] == 1,
            "PsCreateSystemThreadEx: TLS, id, exit status": m3a["KiTestPsThreadOk"] == 1,
            "thread notify routine (create + exit)": m3a["KiTestPsNotifyCount"] >= 2,
            "NtQueueApcThread -> STATUS_USER_APC": m3a["KiTestNtApcOk"] == 1,
            "NtWaitForMultipleObjectsEx WaitAny": m3a["KiTestNtWaitMultipleOk"] == 1,
            "NtDuplicateObject": m3a["KiTestObDuplicateOk"] == 1,
            "objects freed (pool balanced)": m3a["KiTestObPoolBalanced"] == 1,
            "object suite finished": m3a["KiTestObDone"] == 1,
        },
        "M3b": {
            "open via \\??\\R: link, driver sees remaining path": m3b["KiTestIoOpenOk"] == 1,
            "synchronous read/write + file pointer + pended sync read": m3b["KiTestIoSyncRwOk"] == 1,
            "query/set information (I/O manager + driver classes)": m3b["KiTestIoQueryOk"] == 1,
            "asynchronous read: STATUS_PENDING, event, IO_STATUS_BLOCK": m3b["KiTestIoAsyncOk"] == 1,
            "APC completion routine": m3b["KiTestIoApcOk"] == 1,
            "completion port: IRP + mini packet + timeout": m3b["KiTestIoPortOk"] == 1,
            "IOCTL by handle and IoSynchronousDeviceIoControlRequest": m3b["KiTestIoIoctlOk"] == 1,
            "relative open through a file handle": m3b["KiTestIoRelativeOk"] == 1,
            "errors: sync failure, names, initializing, exclusive, access": m3b["KiTestIoErrorsOk"] == 1,
            "StartIo serialisation in order": m3b["KiTestIoStartIoOk"] == 1,
            "cleanup/close per file, device refs released": m3b["KiTestIoCloseOk"] == 1,
            "IoDeleteDevice / IoDeleteSymbolicLink": m3b["KiTestIoDeleteOk"] == 1,
            "I/O pool balanced": m3b["KiTestIoPoolBalanced"] == 1,
            "I/O suite finished": m3b["KiTestIoDone"] == 1,
        },
        "M3c": idex_checks(mon, hdd),
        "M3c-2": fatx_checks(mon, hdd),
        "M3c-3": dvd_checks(mon),
        "M4a": title_checks(mon, title_map),
        "M4b-1": {
            "EEPROM read (0xFFFF) + security section authenticated": mon.read("KiTestHalEepromOk") == 1,
            "settings: game region, video standard, unknown index": mon.read("KiTestHalSettingsOk") == 1 and mon.read("KiTestHalRegion") != 0,
            "save setting + read back, checksum kept": mon.read("KiTestHalSaveOk") == 1,
            "SMC tray state with a disc = 0x60": mon.read("KiTestHalTrayState") == 0x60,
            "HAL suite finished": mon.read("KiTestHalDone") == 1,
        },
        "M5a": rtl_checks(mon),
        "M5b": {f"SEH {n}": mon.read(f"KiTestSeh{n}Ok") == 1 for n in ["Divide", "PageFault", "Raise", "Unwind", "Nested",
                "Capture"]} | {"SEH suite finished": mon.read("KiTestSehDone") == 1},
        "M5c": {f"Xc {n}": mon.read(f"KiTestXc{n}Ok") == 1 for n in ["Des", "ModExp", "Rsa", "Pkcs1",
               "Vector"]} | {"Xc suite finished": mon.read("KiTestXcDone") == 1},
        "M5d-1": misc_checks(mon),
    }


def is_exfat_disk(q):
    """A GPT disk (protective MBR): the exFAT layout (spec/oxos_internal/05_exfat_storage_spec.md)."""
    s0 = q.read(0, 512)
    return s0[510:512] == b"\x55\xAA" and s0[446 + 4] == 0xEE


def idex_checks(mon, hdd):
    if hdd is None:
        return {"HDD image attached": False}
    q = Qcow2(hdd)
    if is_exfat_disk(q):
        return idex_exfat_checks(mon, q)
    starts = [0, 0x55F400, 0x465400, 0x400, 0x177400, 0x2EE400]
    sigs_ok = all(
        [mon.read_offset("KiTestIdexPartitionSig", 16 * i + 4 * w) for w in range(4)]
        == list(struct.unpack("<4I", q.read(lba * 512, 16)))
        for i, lba in enumerate(starts))
    data = struct.unpack("<65536I", q.read(0x55F400 * 512, 0x40000))
    want = sum(v * (i | 1) for i, v in enumerate(data)) & 0xFFFFFFFF

    def mem(name, n):
        return b"".join(struct.pack("<I", mon.read_offset(name, i)) for i in range(0, n, 4))[:n]

    def ansi(name):  # STRING {Length, MaximumLength, Buffer}
        length = mon.read(name) & 0xFFFF
        buf = mon.read_offset(name, 4)
        raw = b"".join(struct.pack("<I", int(mon.hmp(f"x/1wx {buf + i:#x}").split(":")[1].split()[0], 16)) for i in range(0, length + 3, 4))
        return raw[:length]
    password = hmac.new(mem("XboxHDKey", 16), ansi("HalDiskModelNumber") + ansi("HalDiskSerialNumber"), hashlib.sha1).digest() + bytes(12)
    before, after = mon.read("hardware::ide::DiskSecurityWord") & 0xFFFF, mon.read("KiTestIdexSecurityAfter")
    print(f"  (IDENTIFY word 128: {before:#06x} at boot, {after:#06x} after)")
    return {
        "IDE pass-through: IDENTIFY, buffer rule, failing command completes": mon.read("KiTestIdexPassThroughOk") == 1,
        "drive password = HMAC-SHA1(XboxHDKey, model, serial)": mem("KiTestIdexPassword", 32) == password,
        "locked drive unlocked at boot (word 128: locked bit gone)": not (before & 4)
            or (after & 6) == 2 and mon.read("hardware::ide::DiskSecurityUnlocked") & 0xFF == 1,
        "disk identified: capacity of the image (64-bit size)": mon.read("hardware::ide::DiskLengthInSectors")
            | mon.read_offset("hardware::ide::DiskLengthInSectors", 4) << 32 == q.size // 512,
        "\\Device\\Harddisk0\\PartitionN on demand, names checked": mon.read("KiTestIdexNamesOk") == 1,
        "sector 0 of partitions 0-5 matches the image": sigs_ok,
        "256 KB DMA read (2 chunks) matches the image": mon.read("KiTestIdexBigOk") == 1 and mon.read("KiTestIdexBigSum") == want,
        "write + read back (page-crossing pool buffer)": mon.read("KiTestIdexWriteOk") == 1,
        "IOCTL partition info + drive geometry": mon.read("KiTestIdexIoctlOk") == 1,
        "alignment / bounds errors": mon.read("KiTestIdexErrorsOk") == 1,
        "IDE suite finished": mon.read("KiTestIdexDone") == 1,
        **lba48_kernel_checks(mon, q)}


def idex_exfat_checks(mon, q):
    """The IDE suite on an exFAT disk: Partition0 shows the retail reserved area (kept at sectors 1024-2047) and
    nothing beyond it; the folder partitions have no sectors of their own."""
    reserved = q.read(1024 * 512, 16)
    return {
        "exFAT layout detected (GPT, exFAT partition)": mon.read("storage::DiskHasEXFATLayout") & 0xFF == 1,
        "Partition0 sector 0 = the reserved-area copy at sector 1024": [
            mon.read_offset("KiTestIdexPartitionSig", 4 * w) for w in range(4)] == list(struct.unpack("<4I", reserved)),
        "Partition0 refuses access past the reserved area; folder partitions refuse raw reads": mon.read("KiTestIdexFolderOk") == 1,
        "write + read back (page-crossing pool buffer)": mon.read("KiTestIdexWriteOk") == 1,
        "IOCTL partition info + drive geometry": mon.read("KiTestIdexIoctlOk") == 1,
        "IDE suite finished": mon.read("KiTestIdexDone") == 1,
    }


# 48-bit addressing (spec/oxos_internal/04_lba48.md): the writes the kernel's self-test makes on a disk past the 28-bit limit, as (first LBA, sector count,
# tag); tests/kernel_self_tests/disk.cpp has the same list.
def lba48_writes(sectors):
    return [(0x0FFFFFFE, 1, 1), (0x0FFFFFFF, 1, 2), (0x10000000, 1, 3), (0x10000001, 1, 4), (0x0FFFFF80, 512, 5), (0x0FFFFE00, 256, 6), (0x20000000, 256, 7),
            (sectors - 8, 8, 8)]


def lba48_kernel_checks(mon, q):
    if q.size // 512 < 0x20001000:
        return {}
    return {
        "48-bit drive detected (IDENTIFY words 83/86)": mon.read("hardware::ide::DiskSupportsLba48") & 0xFF == 1,
        "48-bit self-test ran (target sectors were free)": mon.read("KiTestIdexLba48Ran") == 1,
        "48-bit writes read back (around 0x0FFFFFFF, 256-sector counts, end of disk)": mon.read("KiTestIdexLba48Ok") == 1,
    }


def lba48_disk_checks(copy, original):
    """After xemu exits: each pattern sits at exactly LBA * 512 in the image, and nothing landed near the start of
    the disk (a 28-bit command for sector 0x10000000 would wrap to sector 0)."""
    sectors = copy.size // 512
    if sectors < 0x20001000:
        return {}
    writes = lba48_writes(sectors)
    placed = True
    for lba0, count, _ in writes:
        data = copy.read(lba0 * 512, count * 512)
        for n in range(count):
            lba = lba0 + n
            tag = [t for a, c, t in writes if a <= lba < a + c][-1]
            want = struct.pack("<4I", 0x3834424C, lba & 0xFFFFFFFF, lba >> 32, tag) + struct.pack("<124I",
                *(((lba & 0xFFFFFFFF) * 2654435761 & 0xFFFFFFFF) ^ i for i in range(4, 128)))
            placed &= data[n * 512:(n + 1) * 512] == want
    # The first 0x400 sectors (before X:) and the first sectors of each partition are unchanged, apart from the self-test's own write at sectors 0x3F0-0x3F7 and
    # the refurb record in sector 3 (the kernel counts the power cycle there at every boot), which must still not hold a test pattern.
    start = [lba for lba in range(0x400) if lba != 3 and not 0x3F0 <= lba < 0x3F8]
    changed = [lba for lba in start + [0x400, 0x177400, 0x2EE400, 0x465400, 0x55F400]
               if copy.read(lba * 512, 512) != original.read(lba * 512, 512)]
    if copy.read(3 * 512, 4) == b"LB48":
        changed.append(3)
    if changed:
        print(f"  (sectors changed near the start: {[hex(x) for x in changed]})")
    start_same = not changed
    return {
        "each 48-bit pattern at exactly LBA * 512 in the image": placed,
        "start of the disk and partition starts untouched (no 28-bit wrap)": start_same,
    }


def read_string(mon, name, length):
    words = [mon.read_offset(name, i) for i in range(0, length, 4)]
    raw = b"".join(struct.pack("<I", w) for w in words)
    return raw.split(b"\0")[0].decode("latin-1")


def wait_for_suite(mon, done_flag, seconds=60):
    """Waits (bounded) until a self-test suite says it has finished, so its results are read once they are final."""
    for _ in range(int(seconds * 4)):
        if mon.read(done_flag):
            return
        time.sleep(0.25)


def fatx_checks(mon, hdd):
    if hdd is None:
        return {"HDD image attached": False}
    wait_for_suite(mon, "KiTestFATXDone")  # its exFAT part writes a file past 4 GB: about 14 s after power-on
    q = Qcow2(hdd)
    if is_exfat_disk(q):
        v = exfat.EXFAT(q)
        dash = v.read_file("/C/xboxdash.xbe")
        names = "".join(x.name + "|" for x in v.listdir("/E"))
        clusters, serial = min(v.count * v.cs, 2 << 40) // 0x4000, v.serial
    else:
        c, e = FATX(q, "C"), FATX(q, "E")
        dash = c.read_file("xboxdash.xbe")
        names = "".join(n + "|" for n, *_ in e.listdir())
        clusters, serial = e.clusters, e.serial
    s = 0
    for b in dash:
        s = (s * 31 + b) & 0xFFFFFFFF
    return {
        "C:\\xboxdash.xbe read in odd pieces matches the image": mon.read("KiTestFATXReadOk") == 1 and mon.read("KiTestFATXReadLength") == len(dash) and
            mon.read("KiTestFATXReadSum") == s,
        "file information (standard, network open, name, mode)": mon.read("KiTestFATXInfoOk") == 1,
        "E:\\ enumeration matches the image": mon.read("KiTestFATXDirOk") == 1
            and read_string(mon, "KiTestFATXDirNames", 64) == names[:62],
        "create dir + 100 KB file, reopen and verify": mon.read("KiTestFATXWriteOk") == 1,
        "mask query, rename, delete, non-empty dir refused": mon.read("KiTestFATXRenameDeleteOk") == 1,
        "error statuses (not found, collision, dir/non-dir, names)": mon.read("KiTestFATXErrorsOk") == 1,
        "D: as a hard-disk folder reports 2048-byte sectors, the volume its own": mon.read("KiTestFATXTitleFolderOk") == 1,
        "volume information (size, serial, FATX)": mon.read("KiTestFATXVolumeOk") == 1 and mon.read("KiTestFATXTotalClusters") == clusters and
            mon.read("KiTestFATXSerial") == serial,
        "free clusters balanced": mon.read("KiTestFATXFreeBalanced") == 1,
        "FATX suite finished": mon.read("KiTestFATXDone") == 1,
        **exfat_format_checks(mon, q)}


def exfat_format_checks(mon, q):
    """exFAT disk: a title-style format of X: empties the folder; E: refuses it (05_exfat_storage_spec.md §5)."""
    if not is_exfat_disk(q):
        return {}
    total, free = mon.read("KiTestEXFATFormatTotal"), mon.read("KiTestEXFATFormatFree")
    spc = mon.read("KiTestEXFATFormatSpc")
    print(f"  (X: after the format: {free} of {total} units of {spc * 512 // 1024} KB free)")
    # A FATX volume of 750 MB with the 64 KB clusters the format chose: 4 KB header, table rounded to 4 KB.
    length, cluster = 0x2EE00000, 0x10000
    fat = ((length // cluster + 1) * 2 + 0xFFF) & ~0xFFF
    clusters = (length - 0x1000 - fat) // cluster
    return {
        "X: formatted through its volume handle: folder emptied, header serial shown": mon.read("KiTestEXFATFormatOk") == 1,
        "X:'s format kept in C:\\OXOS\\cache\\oxos_cache_partitions.cache, and X: mounted with it after it was forgotten (a power-off)":
            mon.read("KiTestEXFATKeptFormatOk") == 1,
        "X: reports a 750 MB FATX volume in the format's cluster size, only its root directory in use": spc == 128 and total == clusters and free == total - 1,
        "E: refuses a format through its volume handle": mon.read("KiTestEXFATRefuseOk") == 1,
        "allocations through E: and Y: in turn succeed (the volume check below finds lost bitmap bits)": mon.read("KiTestEXFATCrossOk") == 1,
        "a FAT chain is followed only as far as the file's length (stale tail ignored, 3 new clusters)": mon.read("KiTestEXFATStaleOk") == 1,
        "an allocation size allocates nothing past the length (create and set-allocation)": mon.read("KiTestEXFATPreallocOk") == 1,
        "a file grows past 4 GB, reads back, shrinks and opens again for writing":
            mon.read("KiTestEXFATLargeOk") == 1,
        "exFAT names: 255 characters, Latin-1 too, listed whole, opened in capitals, renamed, deleted; 256 refused (43 on FATX)":
            mon.read("KiTestEXFATLongNameOk") == 1,
    }


def dvd_checks(mon):
    wait_for_suite(mon, "KiTestDvdDone")  # it starts after the FATX suite
    big = mkxiso.TREE["media"]["big.bin"]
    names = "".join(n + "|" for n in sorted(mkxiso.TREE, key=lambda n: n.upper().encode()))
    return {
        "\\Device\\CdRom0 + CHECK_VERIFY with a disc": mon.read("KiTestDvdDeviceOk") == 1,
        "XDVDFS: 300 KB file (DMA chunks) matches the image": mon.read("KiTestDvdBigLength") == len(big) and
            mon.read("KiTestDvdBigSum") == mkxiso.checksum(big),
        "XDVDFS: \\??\\CdRom0: link, nested dirs, offsets, EOF": mon.read("KiTestDvdReadOk") == 1,
        "XDVDFS: root enumeration in tree order": mon.read("KiTestDvdDirOk") == 1
            and read_string(mon, "KiTestDvdDirNames", 64) == names,
        "XDVDFS: write-protected media, lookup errors": mon.read("KiTestDvdErrorsOk") == 1,
        "XDVDFS: physical sort key (GDFX name, start sector at FsContext)": mon.read("KiTestDvdSortKeyOk") == 1,
        "RAW: Partition0 volume reads, sector alignment, no names": mon.read("KiTestDvdRawOk") == 1,
        "DVD suite finished": mon.read("KiTestDvdDone") == 1,
    }


TITLE_PATCHES = False  # set when the disk copy carries the test title's patch file


def write_title_patches(hdd_copy, title, title_map):
    """C:\\OXOS\\oxos_title_patches.ini for the test title (spec/oxos_internal/25 §3), on a FATX test disk."""
    global TITLE_PATCHES
    q = Qcow2(hdd_copy)
    try:
        w = FATXWriter(q, "C")
    except Exception:  # not a FATX disk (exFAT): no patch file
        return
    xbe = open(title, "rb").read()
    base, cert = struct.unpack_from("<I", xbe, 0x104)[0], struct.unpack_from("<I", xbe, 0x118)[0]
    tid, version = struct.unpack_from("<I", xbe, cert - base + 8)[0], struct.unpack_from("<I", xbe, cert - base + 0xAC)[0]
    syms = map_symbols(title_map)
    text = (f"; boot_test.py\r\n[{tid:08X}]\r\nname = test title: patch A\r\n"
            f"patch = {syms['_XkTitlePatchA']:X}: 11 11 11 11 > A0 A0 A0 A0\r\n"
            f"patch = {syms['_XkTitlePatchC']:X}: 00 00 00 00 > C0 ; other original bytes: not written\r\n"
            f"patch = FFF0: 00 > 90 ; below the image: skipped\r\n"
            f"[{tid:08X}/{version + 1}]\r\npatch = {syms['_XkTitlePatchB']:X}: 22 22 22 22 > B0 B0 B0 B0\r\n")
    w.makedirs("OXOS")
    w.write_file("OXOS\\oxos_title_patches.ini", text.encode())
    q.save(hdd_copy + ".new")
    os.replace(hdd_copy + ".new", hdd_copy)
    TITLE_PATCHES = True


REDIRECT = False  # set when the disk copy carries the launch redirect file (--redirect)


def write_launch_redirect(hdd_copy):
    """C:\\OXOS\\oxos_settings.ini's [launch_redirect] (spec/oxos_internal/33 §6), on a FATX test disk: the relaunch goes to the
    disc's redirected\\ folder; the third launch's missing image goes to a target that does not exist either, so the
    kernel falls back to the image the page names (§4)."""
    global REDIRECT
    q = Qcow2(hdd_copy)
    w = FATXWriter(q, "C")
    text = ("; boot_test.py --redirect\r\n[dashboard]\r\ndashboard_1 = C:\\xboxdash.xbe\r\n[launch_redirect]\r\n"
            "relaunch_test = D:\\media\\second.xbe >> D:\\redirected\\second.xbe\r\n"
            "missing_target = d:/media/missing.xbe>>D:\\nowhere\\missing.xbe\r\n")
    w.makedirs("OXOS")
    w.write_file("OXOS\\oxos_settings.ini", text.encode())
    q.save(hdd_copy + ".new")
    os.replace(hdd_copy + ".new", hdd_copy)
    REDIRECT = True


# The test extensions (tests/extensions, spec/oxos_internal/34 §9.3) in their oxos_extensions.ini order: seven that load (plain_c in C, the SDK's three
# examples, and slow_loop, whose first RunLoop after the cold boot takes 600 ms), two the kernel must unload or refuse, one switched off, one without the
# enabled = yes an extension needs to load (a copy of chain, which would load), and one in the folder that the list does not name.
EXTENSIONS_INI = ("; boot_test.py --extensions\r\n"
                  "[extension_test]\r\nextension = extension_test.xos\r\nenabled = yes\r\nanswer = 42\r\nboth = from the section\r\nlogging_enabled = no\r\n"
                  "[chain]\r\nextension = chain.xos\r\nenabled = yes\r\n"
                  "[plain C]\r\nextension = plain_c.xos\r\nenabled = yes\r\ngreeting = hello\r\n"
                  "[example_run_loop]\r\nextension = example_run_loop.xos\r\nenabled = yes\r\nlogging_enabled = yes\r\n"
                  "[example_kernel_hook]\r\nextension = example_kernel_hook.xos\r\nenabled = yes\r\nlogging_enabled = yes\r\n"
                  "[example_title_event]\r\nextension = example_title_event.xos\r\nenabled = yes\r\nlogging_enabled = yes\r\n"
                  "[slow_loop]\r\nextension = slow_loop.xos\r\nenabled = yes\r\n"
                  "[failload]\r\nextension = failload.xos\r\nenabled = yes\r\n"
                  "[badversion]\r\nextension = badversion.xos\r\nenabled = yes\r\n"
                  "[switched off]\r\nextension = disabled.xos\r\nenabled = no\r\n"
                  "[no enabled line]\r\nextension = notenabled.xos\r\n"
                  "[oxos_trainers]\r\nextension = oxos_trainers.xos\r\nenabled = yes\r\nlogging_enabled = yes\r\n")
# The extensions' own ini files, next to them (spec/oxos_internal/34 §3.1): settings at the top, data sections after them. extension_test's has 300 sections
# of padding before the one it reads (past the first 16 KB, many 4 KB reads); its "both" is also in its oxos_extensions.ini section, where it must lose.
OWN_INI_FILES = {
    "extension_test": "; boot_test.py --extensions\r\nown = file\r\nboth = own ini wins\r\ntrainer_title = 584B0001\r\n" +
                      "".join(f"[padding {i}]\r\nline = {'x' * 60}\r\n" for i in range(300)) +
                      "[Halo 2 cheats]\r\nInfinite Ammo = yes\r\nNever Die = no\r\n",
    "example_run_loop": "interval = 1\r\n",
    "example_title_event": "led_title = oxos KERNEL test title\r\n\r\n[OXOS kernel test title]\r\ngreeting = Hello from the data section.\r\n",
}


# The test title's section: two trainers (xktest.etm writes the mark 0xC0DE0001, xktest_two.etm 0xC0DE0002), the first one set, with a keys line the
# extension must not take for a switch. A third, xktest_new.etm (0xC0DE0003), is on the disk but not in the ini: the menu's scan adds it. xktest_gone.etm is
# in the ini but not on the disk (deleted): the scan removes its trainer 3 line, the section of a title it was the only trainer of (with the blank line and
# comment above it), and gives the title it was chosen for its other trainer, off.
TRAINERS_INI = ("; boot_test.py --extensions\r\n[584B0001]\r\ntitle = OXOS kernel test title\r\ntrainer = xktest.etm\r\ntrainer_enabled = no\r\n"
                "1. Write the mark = yes\r\n1 keys = RIGHT STICK + D-PAD DOWN\r\n2. Unused = no\r\n"
                "trainer 1 = xktest.etm | build any | keys 1: RIGHT STICK + D-PAD DOWN\r\ntrainer 2 = xktest_two.etm | build any\r\n"
                "trainer 3 = xktest_gone.etm | build any\r\n"
                "\r\n; its only trainer deleted\r\n[584B00FF]\r\ntitle = Gone\r\ntrainer = xktest_gone.etm\r\ntrainer_enabled = yes\r\n1. Cheat = yes\r\n"
                "\r\n; its chosen trainer deleted\r\n[584B00FE]\r\ntitle = Chosen again\r\ntrainer = xktest_gone.etm\r\ntrainer_enabled = yes\r\n"
                "1. Cheat = yes\r\ntrainer 1 = xktest_gone.etm | build any\r\ntrainer 2 = xktest_two.etm | build any\r\n")
# The ini as the menu leaves it: the scan's trainer 4 and the deleted trainer's lines gone; the second trainer chosen and switched on with its cheat 1
# (TRAINER_MENU_KEYS).
TRAINERS_INI_AFTER = ("; boot_test.py --extensions\r\n[584B0001]\r\ntitle = OXOS kernel test title\r\ntrainer = xktest_two.etm\r\ntrainer_enabled = yes\r\n"
                      "1. Write the mark = yes\r\n2. Unused = no\r\n"
                      "trainer 1 = xktest.etm | build any | keys 1: RIGHT STICK + D-PAD DOWN\r\ntrainer 2 = xktest_two.etm | build any\r\n"
                      "trainer 4 = xktest_new.etm | build unknown\r\n"
                      "\r\n; its chosen trainer deleted\r\n[584B00FE]\r\ntitle = Chosen again\r\ntrainer = xktest_two.etm\r\ntrainer_enabled = no\r\n"
                      "1. Write the mark = no\r\n2. Unused = no\r\ntrainer 2 = xktest_two.etm | build any\r\n")
TRAINER_MENU_PATH = "\\Device\\Harddisk0\\Partition2\\OXOS\\extensions\\oxos_trainers.xbe"
# At the relaunch's menu: right to the second trainer, down to the trainer's switch and on, down to cheat 1 and on, start the game.
TRAINER_MENU_KEYS = (("HATX", 1), ("HATY", 1), ("A", 1), ("HATY", 1), ("A", 1), ("START", 1))


def image_file_name(mon):
    """XeImageFileName: the XBE the kernel loaded last."""
    ptr = mon.read_offset("XeImageFileName", 4)  # ANSI_STRING: Length, MaximumLength, Buffer
    raw = b"".join(struct.pack("<I", int(w, 16)) for w in re.findall(r"0x([0-9a-f]{8})", mon.hmp(f"x/24wx {ptr:#x}").split(":", 1)[1]))
    return raw.split(b"\0")[0].decode("latin-1")


def use_trainer_menu(mon, pad, reboots, keys):
    """Waits for the trainer menu (oxos_trainers.xos starts it in place of the test title) after at least `reboots` quick reboots, then presses `keys` on the
    virtual pad. True when the menu came up."""
    for _ in range(300):
        if mon.read("KiQuickRebootCount") >= reboots and image_file_name(mon) == TRAINER_MENU_PATH:
            break
        time.sleep(0.1)
    else:
        return False
    time.sleep(4)  # the menu draws its logo, then reads the pad
    for control, value in keys:
        pad.set(control, value)
        time.sleep(0.15)
        pad.set(control, 0)
        time.sleep(0.4)
    return True


def make_test_trainer(title_id, mark_address, mark=0xC0DE0001):
    """A trainer (.etm, format 1, spec/oxos_internal/37) of our own for the test title: two cheats, both off in the file; its code writes mark to the test
    title's XkTitleTrainerMark when cheat 1 is on, 0xC0DE0000 when off. Entered with ebp at the trainer, as a dashboard's stub enters it."""
    switches, texts, ids, entry = 0x40, 0x50, 0x68, 0x20
    names = [b"OXOS test trainer", b"boot_test.py", b"Write the mark", b"Unused"]
    code = (b"\x80\xBD" + struct.pack("<I", switches) + b"\x00"            # cmp byte [ebp + switches], 0
            + b"\x74\x0B"                                                  # je off
            + b"\xC7\x05" + struct.pack("<II", mark_address, mark) + b"\xC3"        # mov dword [mark_address], mark; ret
            + b"\xC7\x05" + struct.pack("<II", mark_address, 0xC0DE0000) + b"\xC3") # off: mov dword [mark], 0xC0DE0000; ret
    data = bytearray(0x100)
    data[entry:entry + len(code)] = code
    data[switches:switches + 2] = b"\x00\x00"
    data[ids:ids + 12] = struct.pack("<III", title_id, 0, 0)
    at = 0x80
    for i, name in enumerate(names):
        struct.pack_into("<I", data, texts + 4 * i, at)
        data[at:at + len(name)] = name
        at += len(name) + 1
    struct.pack_into("<IHIIIIII", data, 0, len(data), 1, 0, switches, texts, ids, entry, 0)
    return bytes(data)


def write_extensions(hdd_copy, folder, title_map=None):
    """C:\\OXOS\\oxos_extensions.ini and the test extensions in C:\\OXOS\\extensions, on a FATX test disk, with the Universal Trainers test trainer (title_map:
    the test title's map, for its XkTitleTrainerMark). disabled.xos and unlisted.xos are copies of failload.xos: neither may load."""
    q = Qcow2(hdd_copy)
    w = FATXWriter(q, "C")
    w.makedirs("OXOS\\extensions")
    w.write_file("OXOS\\oxos_extensions.ini", EXTENSIONS_INI.encode())
    # Universal Trainers for the test title (0x584B0001): its section has the first trainer set, off, and its cheat 1 on (off in the trainer file). The
    # trainer menu (oxos_trainers.xbe) comes up at the cold boot (its scan adds xktest_new.etm) and at the relaunch, where boot_test picks the second trainer
    # and switches it and its cheat 1 on (TRAINER_MENU_KEYS); extension_test.xos reads the mark the trainer writes (its settings trainer_mark, trainer_title).
    mark = map_symbols(title_map)["_XkTitleTrainerMark"] if title_map else 0
    w.makedirs("OXOS\\trainers")
    w.write_file("OXOS\\trainers\\xktest.etm", make_test_trainer(0x584B0001, mark))
    w.write_file("OXOS\\trainers\\xktest_two.etm", make_test_trainer(0x584B0001, mark, 0xC0DE0002))
    w.write_file("OXOS\\trainers\\xktest_new.etm", make_test_trainer(0x584B0001, mark, 0xC0DE0003))
    w.write_file("OXOS\\extensions\\oxos_trainers.ini", TRAINERS_INI.encode())
    w.write_file("OXOS\\extensions\\oxos_trainers.xbe", open(os.path.join(folder, "oxos_trainers.xbe"), "rb").read())
    for name, text in OWN_INI_FILES.items():
        if name == "extension_test":  # the mark's address in the test title, a setting
            text = text.replace("trainer_title = ", f"trainer_mark = {mark:X}\r\ntrainer_title = ")
        w.write_file(f"OXOS\\extensions\\{name}.ini", text.encode())
    for name in ("extension_test", "chain", "plain_c", "example_run_loop", "example_kernel_hook", "example_title_event", "slow_loop", "failload",
                 "badversion", "oxos_trainers"):
        w.write_file(f"OXOS\\extensions\\{name}.xos", open(os.path.join(folder, f"{name}.xos"), "rb").read())
    for name in ("disabled", "unlisted"):
        w.write_file(f"OXOS\\extensions\\{name}.xos", open(os.path.join(folder, "failload.xos"), "rb").read())
    w.write_file("OXOS\\extensions\\notenabled.xos", open(os.path.join(folder, "chain.xos"), "rb").read())
    q.save(hdd_copy + ".new")
    os.replace(hdd_copy + ".new", hdd_copy)


def extension_state_pages(mon):
    """The state pages of the loaded extensions, in load order (none when no extension is loaded)."""
    state = mon.read("extensions::LoadedExtensions")
    if not state:
        return []
    count = int(mon.hmp(f"x/1wx {state:#x}").split(":")[1].split()[0], 16)
    # XK_EXTENSIONS_STATE: Count, then XK_EXTENSION[16] of 64 bytes, each starting with its OXOS_CONTEXT (State at +40).
    return [int(mon.hmp(f"x/1wx {state + 4 + 64 * i + 40:#x}").split(":")[1].split()[0], 16) for i in range(count)]


def extension_loaded_titles(mon):
    """The titles both extension_test.xos and plain_c.xos (the first and the last to count them, in one event) have
    seen loaded: their OXOS_EVENT_TITLE_LOADED counts; 0 before they load."""
    pages = extension_state_pages(mon)
    if len(pages) < 3:
        return 0
    def word(address):
        return int(mon.hmp(f"x/1wx {address:#x}").split(":")[1].split()[0], 16)
    return min(word(pages[0] + 4 * 8), word(pages[2] + 4 * 2))


def extension_checks(mon, launches, hdd):
    """What the test extensions recorded in their state pages (tests/extensions/extension_test.cpp, chain.cpp,
    plain_c.c, the SDK's examples example_run_loop, example_kernel_hook and example_title_event, slow_loop.cpp) after `launches` launches: the cold boot's
    title and each quick reboot's
    (spec/oxos_internal/34 §9.3). The emulator is paused at the last launch (title_checks)."""
    def words(address, n):
        out = mon.hmp(f"x/{n}wx {address:#x}")
        return [int(w, 16) for line in out.splitlines() for w in line.split(":")[1].split()][:n]
    pages = extension_state_pages(mon)
    count = len(pages)
    x = words(pages[0], 19) if count >= 1 else [0] * 19
    c = words(pages[1], 3) if count >= 2 else [0] * 3
    p = words(pages[2], 6) if count >= 3 else [0] * 6
    t = words(pages[5], 1) if count >= 6 else [0]  # example_title_event: its launches
    s = words(pages[6], 4) if count >= 7 else [0] * 4
    # C:\OXOS\log\extensions.log: one line per RunLoop call over 500 ms, all slow_loop's (the disk is read while the emulator is paused)
    log = FATX(Qcow2(hdd), "C").read_file("OXOS\\log\\extensions.log") or b""
    ini = FATX(Qcow2(hdd), "C").read_file("OXOS\\extensions\\oxos_trainers.ini") or b""  # as the trainer menu wrote it
    lines = log.decode("latin-1").split("\r\n")
    delays = [re.fullmatch(r"extension slow_loop delayed extension loop by (\d+) ms", line) for line in lines if line.startswith("extension ")]
    hook_lines = [line for line in lines if line.startswith("[example_kernel_hook] ")]
    hook_start = "[example_kernel_hook] Starting extension example_kernel_hook version 1.0.0."
    hook_end = "[example_kernel_hook] Terminating extension example_kernel_hook version 1.0.0."
    hook_counts = [bool(re.fullmatch(r"\[example_kernel_hook\] NtOpenFile called [1-9]\d* times\.", line)) for line in hook_lines[1::3]]
    if not count:
        return {"extensions loaded (extensions::LoadedExtensions)": False}
    print(f"  (slow_loop: {s[1]} Loads, {s[2]} slow calls of {s[3]}; extensions.log: {len(lines) - 1} lines, {[l for l in lines if l.startswith('extension ')]})")
    return {
        "extension_test, chain, plain_c, the three examples, slow_loop and oxos_trainers loaded; failload unloaded, badversion refused; disabled, the one "
        "without enabled = yes and unlisted not loaded":
            count == 8 and s[0] == 0x574F4C53 and x[0] == 0x54534F58 and c[0] == 0x4E414843 and p[0] == 0x434E4C50,
        f"state page kept across the quick reboots: {launches} Loads, Generation {launches}": x[1] == launches and x[2] == launches and x[3] == 0 and
            c[1] == launches,
        "RunLoop called while the titles ran, never after Terminate": x[5] > 0 and x[6] == 0,
        # The cold boot's title may end before the 600 ms are up: the kernel then logs the call still running after its 500 ms wait at the title's end.
        "slow_loop's 600 ms RunLoop at the cold boot; the loop went on calling RunLoop in the later launches": s[1] == launches and s[2] <= 1 and s[3] > 2,
        "C:\\OXOS\\log\\extensions.log: 'extension slow_loop delayed extension loop by N ms' for it, once, 500 < N < 2000": len(delays) == 1 and delays[0] and
            500 < int(delays[0][1]) < 2000,
        "logging_enabled = no in extension_test's section, though it asks for the file and the serial port (oxos_set_log_target), and plain C without the "
        "line: none of their lines in extensions.log; the examples, with logging_enabled = yes, log there":
            not any(l.startswith("[extension_test]") for l in lines) and not any(l.startswith("[plain C]") for l in lines) and any(l.startswith("[example_kernel_hook] ") for l in lines),
        "example_run_loop: '1 s into this title.' from its RunLoop; at each title's end how long it ran": "[example_run_loop] 1 s into this title." in lines and
            sum(bool(re.fullmatch(r"\[example_run_loop\] This title ran \d+ s\.", l)) for l in lines) == launches - 1,
        f"example_kernel_hook: its start line at each of the {launches} launches (oxos_log's default: the file and the serial port); its NtOpenFile count and "
        "end line at each title's end": hook_lines[0::3] == [hook_start] * launches and hook_lines[2::3] == [hook_end] * (launches - 1) and
            len(hook_counts) == launches - 1 and all(hook_counts),
        f"example_title_event: {launches} launches in its state page; the title's name and the one before it": t[0] == launches and
            '[example_title_event] "OXOS Trainers" is starting, the first title since the cold boot.' in lines and  # the menu, before the disc
            any(l.startswith('[example_title_event] ') and 'the title before it was "OXOS kernel test title"' in l for l in lines),
        "example_title_event: the LED orange from its RunLoop, for the title led_title names (any case)":
            '[example_title_event] "OXOS kernel test title": the LED is orange.' in lines,
        "Terminate at each quick reboot": x[4] == launches - 1,
        "events: a load and a loaded title per launch, an exit per quick reboot":
            x[7] >= launches and x[8] == launches and x[9] == launches - 1,
        "hook on NtOpenFile: the title's calls pass extension_test's hook, then chain's; its own (RunLoop) only chain's": x[10] > 0 and c[2] == x[10] + launches,
        "settings from oxos_extensions.ini and oxos_file_read": x[12] == 1 and x[13] == 1,
        "settings from the extension's own extension_test.ini, which win over its oxos_extensions.ini section": x[15] == 1,
        f"data: oxos_ini_section reads one section of a 28 KB extension_test.ini, keys with spaces; a missing section (step {x[16]}; 1 = all)": x[16] == 1,
        f"Universal Trainers (oxos_trainers.xos): the trainer menu first at the cold boot's disc (its scan adding the trainer new on the disk and removing "
        f"the deleted one's lines) and at the relaunch, which chose the second trainer and switched it on in oxos_trainers.ini; that trainer then ran "
        f"with its cheat 1 on (mark {x[17]:#x} at the test title's last exit, of {x[18]})": x[17] == 0xC0DE0002 and
            x[18] == 2 and "[oxos_trainers] 584B0001: the trainer menu first (\\Device\\CdRom0\\default.xbe)." in lines and
            "[oxos_trainers] 584B0001: the trainer menu first (\\Device\\CdRom0\\media\\second.xbe)." in lines and
            lines.count("[oxos_trainers] OXOS kernel test title: trainer xktest_two.etm applied, 1 of 2 cheats on.") == 1 and
            ini == TRAINERS_INI_AFTER.encode(),
        "example_title_event: its title's greeting from the data section of its own ini":
            '[example_title_event] "OXOS kernel test title": Hello from the data section.' in lines,
        f"the SDK's file and folder functions: create, write, read, list, rename, delete (step {x[14]}; 1 = all)": x[14] == 1,
        f"plain C extension: {launches} Loads and loaded titles, its setting": p[1] == launches and p[2] == launches and p[5] == 1,
        "the kernel's memcpy, memset, memcmp and 64-bit division for extensions (exports 402-415)": p[3] == 1 and p[4] == 1,
    }


def kept_cache_format_checks(mon, title_map):
    """The second boot of test_exfat's disk, with the release kernel (no self-tests): X:'s format from the first boot (serial 0x12345678, 64 KB clusters,
    written by the FATX suite's title-style format) must come back from C:\\OXOS\\cache\\oxos_cache_partitions.cache after the cold boot, as the test
    title reads it (spec/oxos_internal/05 §5)."""
    if not title_map:
        return {"test title built": False}
    syms = map_symbols(title_map)
    def word(name):
        return int(mon.hmp(f"x/1wx {syms[name]:#x}").split(":")[1].split()[0], 16)
    for _ in range(160):  # the title's results are complete once its magic is there
        if word("_XkTitleResults") == 0x54544B58:
            break
        time.sleep(0.25)
    mon.pause()
    serial, spc = word("_XkTitleCacheSerial"), word("_XkTitleCacheSpc")
    mon.resume()
    return {f"X: after a cold boot keeps the format of the boot before: serial {serial:#x} (0x12345678), {spc} sectors per cluster (128)":
            serial == 0x12345678 and spc == 128}


def title_checks(mon, title_map, relaunch=False, reboots=0, extensions=False, pad=None):
    # reboots: quick reboots before the title's first run (the ISO launcher's launch). With relaunch the console does not stop at the third launch: the
    # dashboard starts the test disc again, and the cycle repeats. So the checks pause the emulator for each reading: once the title's results are complete, and
    # at the third launch's dashboard, where it stays paused for the checks after this one (RAM, extensions).
    if not title_map:
        return {"test title built": False}
    syms = map_symbols(title_map)
    base = syms["_XkTitleResults"]
    for _ in range(40):  # the title starts after the kernel suites
        if mon.read("titles::TitleLaunched"):
            break
        time.sleep(0.25)
    # With the extensions, the trainer menu comes up in place of the disc at the cold boot (START: the title as it is set) and of the relaunch (the trainer
    # switched on): two more quick reboots, both before the second run. pad drives it.
    menu = 2 if pad else 0
    menu_shown = pad and use_trainer_menu(mon, pad, 0, (("START", 1),)) and use_trainer_menu(mon, pad, 2, TRAINER_MENU_KEYS)
    for i in range(160):  # includes the quick reboot into media\second.xbe
        v = mon.hmp(f"x/1wx {base:#x}")
        if os.environ.get("XK_DEBUG"):
            print(i, time.time(), v.strip(), mon.read("titles::TitleLaunched"), mon.read("KiQuickRebootCount"))
        # The results are complete once the title has finished; with relaunch, in its second run (second.xbe).
        if v.split(":")[1].split()[0] == "0x54544b58" and (not relaunch or mon.read("KiQuickRebootCount") >= 1 + menu):
            break
        time.sleep(0.25)
    mon.pause()
    count = 9 + 16 + 3 + 16
    out = mon.hmp(f"x/{count}wx {base:#x}")
    try:
        words = [int(w, 16) for line in out.splitlines() for w in line.split(":")[1].split()][:count]
    except (ValueError, IndexError):  # no title mapped there (it never started): every title check fails
        words = []
    words += [0] * (count - len(words))

    def text(ws):
        return b"".join(struct.pack("<I", w) for w in ws).split(b"\0")[0].decode("latin-1")
    name, second = text(words[9:25]), text(words[28:44])
    launched = mon.read("titles::TitleLaunched")
    mon.resume()
    checks = {
        "title launched from \\Device\\CdRom0\\default.xbe": words[1] == 1
            and launched == (3 if relaunch or reboots else 1),
        "XeImageFileName": name == "\\Device\\CdRom0\\default.xbe",
        "kernel thunks resolved: KeTickCount (data) + sleep": words[2] == 1,
        "pool, disc file read, system thread from the title": words[3] == 1 and words[4] == 1 and words[5] == 1,
        "path symbolic link \\??\\T: -> Partition1\\XkTest": words[6] == 1,
        "XeLoadSection / XeUnloadSection (digest-checked)": words[7] == 1,
        "XcSHA known answer": words[8] == 1,
        "title finished": words[0] == 0x54544B58,
    }
    if relaunch:
        # The third launch: the dashboard, after the kernel's events for it (the extensions count them).
        for _ in range(200):
            if mon.read("KiQuickRebootCount") == 2 + reboots + menu and mon.read("titles::TitleLaunched") == 2 and (not extensions or
                    extension_loaded_titles(mon) >= 3 + reboots + menu):
                break
            time.sleep(0.1)
        mon.pause()
        # What the kernel wrote to the launch page for the dashboard (a dashboard may consume the page itself, as the stock one does).
        page = [mon.read_offset("titles::DashboardLaunchData", 4 * i) for i in range(3)]
        checks["missing image from the launch page: dashboard with LDP {type 1, reason 1, code 1}"] = \
            mon.read("titles::TitleLaunched") == 2 and page == [1, 1, 1]
        del checks["path symbolic link \\??\\T: -> Partition1\\XkTest"]  # keep.bin comes from the FATX self-test
        checks |= {
            "quick reboot (HalReturnToFirmware(2)): kernel re-entered (twice), LaunchDataPage re-adopted":
                mon.read("KiQuickRebootCount") == 2 + reboots + menu and
                mon.read("memory::PersistedPages") >= 1,
            "LaunchDataPage launch of media;second.xbe"
            + (", redirected to redirected\\second.xbe (oxos_settings.ini [launch_redirect])" if REDIRECT else "")
            + ", first run's results carried over": words[25] == 1 and
                second == ("\\Device\\CdRom0\\redirected\\second.xbe" if REDIRECT else "\\Device\\CdRom0\\media\\second.xbe"),
            "persisted launch page contents (type, title id, flags, data)": words[27] == 1,
            "\\??\\D: -> launch directory": words[26] == 1,
        }
        if pad:
            checks["the trainer menu (oxos_trainers.xbe) in place of the disc at the cold boot and of the relaunch; the second run got the launch page as the "
                   "first run wrote it"] = bool(menu_shown)
    else:
        checks["no relaunch without the flag file"] = words[25] == 0 and mon.read("KiQuickRebootCount") == 0
    if TITLE_PATCHES and not relaunch:  # after a relaunch the dashboard runs: the title's memory is gone
        def word(sym):
            return int(mon.hmp(f"x/1wx {syms[sym]:#x}").split(":")[1].split()[0], 16)
        checks["title patches: written where the original bytes are, not for another version or other bytes"] = (word("_XkTitlePatchA") == 0xA0A0A0A0 and
            word("_XkTitlePatchB") == 0x22222222 and word("_XkTitlePatchC") == 0x33333333)
    return checks


def ram_checks(mon, memory):
    """The RAM the kernel manages, read after the quick reboots: the cold boot's probe has to survive them."""
    pages = memory * 256
    return {
        # 2 = more than 64 MB; 1 (64 MB) and 3 (unclear: xemu at 64 MB) are both used as 64 MB.
        f"{memory} MB probed at the cold boot": (mon.read("memory::RamProbe") == 2) == (memory == 128),
        f"{pages} pages managed after the quick reboots": mon.read("memory::NumberOfPhysicalPages") == pages and
            mon.read("memory::HighestPhysicalPage") == pages - 1,
    }


# --- Disc images (spec/oxos_internal/16_virtual_discs.md): the ISO launcher on the disc mounts an image of the kernel
# test title (with relaunch.txt) from F:\titles on an exFAT disk; the title then runs from the image, relaunches itself (the mount survives the quick reboots)
# and ends in the dashboard.
ISO_KINDS = ("plain", "split", "cci", "splitcci", "large")


def iso_launch_setup(a, work, dvd, hdd_copy):
    xbe = open(a.title, "rb").read()
    tree = dict(mkxiso.TREE, **{"default.xbe": xbe, "relaunch.txt": b"quick reboot"})
    media = dict(tree["media"], **{"second.xbe": xbe})
    if a.iso_kind == "large":  # the file the title reads lies beyond 4 GB in a > 4 GB image
        media["a.txt"] = mkxiso.High(media["a.txt"])
    tree["media"] = media
    img = mkxiso.write_image(os.path.join(work, "inner.iso"), tree)
    os.remove(os.path.join(work, "inner.iso"))
    name = "Test Title"
    if a.iso_kind == "split":
        cut = (len(img) // 2) & ~0x7FF
        files = {f"{name}.1.iso": bytes(img[:cut]), f"{name}.2.iso": bytes(img[cut:])}
        launch = f"F:\\titles\\{name}.1.iso"
    elif a.iso_kind == "cci":
        cci = os.path.join(work, "inner.cci")
        mkcci.write(bytes(img), cci)
        files = {f"{name}.cci": open(cci, "rb").read()}
        launch = f"F:\\titles\\{name}"  # a folder: the launcher picks the image in it
    elif a.iso_kind == "splitcci":  # two CCI files, each for its half of the sectors
        cut = (len(img) // 2) & ~0x7FF
        files = {}
        for i, half in ((1, img[:cut]), (2, img[cut:])):
            cci = os.path.join(work, f"inner.{i}.cci")
            mkcci.write(bytes(half), cci)
            files[f"{name}.{i}.cci"] = open(cci, "rb").read()
        launch = f"F:\\titles\\{name}.1.cci"
    else:
        files = {f"{name}.iso": img}
        launch = f"F:\\titles\\{name}.iso"
    import fatx
    src = Qcow2(a.hdd)
    b = exfat.Builder(256 << 30, 16 << 10)

    def copy(part, letter, first=None, prefix=""):
        for n, attr, cl, sz, _ in part.listdir(first):
            dest = f"/{letter}{prefix}/{n}"
            if attr & 0x10:
                b.add(dest, attrs=attr)
                copy(part, letter, cl, f"{prefix}/{n}")
            else:
                b.add(dest, part.read_chain(cl, sz), attrs=attr)
    for letter in ("C", "E"):
        copy(fatx.FATX(src, letter), letter)
    folder = f"/F/titles/{name}" if a.iso_kind == "cci" else "/F/titles"
    for n, data in files.items():
        b.add(f"{folder}/{n}", data)
    b.write(hdd_copy, src.read(0, 0x400 * 512))
    mkxiso.write_image(dvd, {"default.xbe": open(a.iso_launch, "rb").read(), "iso.txt": launch.encode() + b"\r\n"})


def iso_checks(mon, a):
    checks = title_checks(mon, a.title_map, relaunch=True, reboots=1)
    checks["image mounted by the launcher (storage::VirtualDiscLastMountStatus 0), read without errors"] = (
        mon.read("storage::VirtualDiscLastMountStatus") == 0 and mon.read("storage::VirtualDiscReads") > 0
        and mon.read("storage::VirtualDiscErrors") == 0)
    return checks


# --- Checks of the other test phases (read from the running console over QMP, or from the serial log and the disk)
def rtl_checks(mon):
    for _ in range(80):
        if mon.read("KiTestRtlDone"):
            break
        time.sleep(0.25)
    names = ["Sprintf", "Interlocked", "CritSec", "RwLock", "Strings", "Math", "Refurb", "Time", "Dbg", "Boost", "Fpu"]
    out = {f"Rtl {n}": mon.read(f"KiTestRtl{n}Ok") == 1 for n in names}
    out["Rtl suite finished"] = mon.read("KiTestRtlDone") == 1
    if not out["Rtl Refurb"]:
        steps = [mon.read_offset("KiTestRtlRefurbSteps", 4 * i) for i in range(4)]
        print(f"  (Refurb: first read {steps[0]:#x}, signature {steps[1]:#x}, write and read again {steps[2]:#x}, " f"short length {steps[3]:#x})")
    return out


def hw_info_ok(mon):
    flags, revs = mon.read("XboxHardwareInfo"), mon.read_offset("XboxHardwareInfo", 4)
    boot_id = int(mon.hmp("xp/1wx 0xfd000000").split(":")[1].split()[0], 16)
    print(f"  (XboxHardwareInfo: flags {flags:#x}, GPU {revs & 0xFF:#x}, MCPX {revs >> 8 & 0xFF:#x}; " f"NV2A boot ID {boot_id:#x})")
    return revs & 0xFF == boot_id & 0xFF and revs >> 8 & 0xFF != 0


def misc_checks(mon):
    for _ in range(80):
        if mon.read("KiTestMiscDone"):
            break
        time.sleep(0.25)
    print(f"  (performance counter: {mon.read('KiTestMiscPmTicksPerSecond')} per KeTickCount second)")
    print(f"  (PHY: PhyInitialize -> {mon.read('KiTestMiscPhyStatus'):#x}, link state {mon.read('KiTestMiscPhyLink'):#x})")
    return {
        "FATX dismount by name, remount on next open, not-mounted error": mon.read("KiTestMiscDismountOk") == 1,
        "disc dismount + remount": mon.read("KiTestMiscDiscRemountOk") == 1,
        "Fsc cache size limits": mon.read("KiTestMiscFscOk") == 1,
        "PhyInitialize completes (success or E_FAIL), link state read": mon.read("KiTestMiscPhyStatus") in (0, 0x801F0001),
        "CMOS clock-set signature: write, verify, damaged byte detected": mon.read("KiTestMiscCmosOk") == 1,
        "SMC tray events update the tray cache and change count": mon.read("KiTestMiscSmcOk") == 1,
        "performance counter: 3.375 MHz, monotonic, matches the clock": mon.read("KiTestMiscPerfOk") == 1,
        "FATX scatter/gather: NtWriteFileGather + NtReadFileScatter round trip": mon.read("KiTestMiscScatterOk") == 1,
        "ACPI SCI (IRQ 12) delivered and acknowledged (PM timer overflow)": mon.read("hardware::chipset::SciCount") >= 1,
        "XboxHardwareInfo: GpuRevision = NV2A boot ID, McpRevision set": hw_info_ok(mon),
        "TV encoder: every SD and HD mode word programs the Conexant registers (read back)": mon.read("KiTestAvModesTried") == 15 and
            mon.read("KiTestAvModesOk") == 15,
        "TV encoder options: WSS/CGMS, flicker filter, captions, luma filter (Conexant registers)": mon.read("KiTestAvOptionsOk") == 0x1F,
        "misc suite finished": mon.read("KiTestMiscDone") == 1,
    }


def dash_checks(mon, evox=False, xbl=False, order=""):
    for _ in range(60):
        if mon.read("titles::TitleLaunched"):
            break
        time.sleep(0.25)
    time.sleep(4)
    exc = mon.read("core::LastExceptionVector")
    t0 = mon.read("KeTickCount")
    time.sleep(0.5)
    ticking = mon.read("KeTickCount") > t0
    # Boot-only memory (spec/oxos_internal/18): pages freed, and the page database (the PTEs at 0x83FF0000) no longer maps the splash's code; the tables of
    # xemu's encoder (a Conexant) stay mapped.
    boot_freed = (bin(mon.read("memory::BootPagesFreed")).count("1")
                  + bin(mon.read_offset("memory::BootPagesFreed", 4)).count("1"))
    def mapped(name):
        pfn = (mon.addr(name) & 0x03FFFFFF) >> 12
        return int(mon.hmp(f"x/1wx {0x83FF0000 + 4 * pfn:#x}").split(":")[1].split()[0], 16) & 1
    boot_code_present, tables_present = mapped("boot_splash::StartSplash"), mapped("ConexantTablesStart")
    start = int(mon.hmp("xp/1wx 0xfd600800").split(":")[1].split()[0], 16)
    fb = start & 0x03FFFFFF
    words = []
    for off in range(0, 640 * 4 * 40, 4096):  # first 40 lines of a 640-wide 32 bpp surface
        out = mon.hmp(f"xp/16wx {fb + off:#x}")
        words += [int(w, 16) for w in re.findall(r"0x([0-9a-f]{8})", out.split(":", 1)[1] if ":" in out else "")]
    ptr = mon.read_offset("XeImageFileName", 4)  # ANSI_STRING: Length, MaximumLength, Buffer
    raw = b"".join(struct.pack("<I", int(w, 16)) for w in re.findall(r"0x([0-9a-f]{8})", mon.hmp(f"x/16wx {ptr:#x}").split(":", 1)[1]))
    dash = raw.split(b"\0")[0].decode("latin-1")
    # Which dashboard should start (spec/oxos_internal/19): the first of --dash-order that is on the disk, else C:\xboxdash.xbe.
    listed_evox = evox and "evoxdash" in order.lower()
    listed_xbl = xbl and "dashboard\\default.xbe" in order.lower()
    want = "\\Device\\Harddisk0\\Partition2\\" + ("evoxdash.xbe" if listed_evox else "Dashboard\\default.xbe" if listed_xbl else "xboxdash.xbe")
    case = (f"[dashboard] {order}" if order else "no [dashboard]") + (", EvoX on the disk" if evox else "") + (", XBL loader on the disk" if xbl else "")
    # The boot screen's diagnostics (kernel/src/boot_splash), read once: the splash has ended.
    bs = {name: mon.read(f"boot_splash::{name}") for name in ("SplashFullMs", "SplashFrames", "SplashSource", "SplashStatus")}
    return {"M4b-2": {
        f"dashboard launched without a disc: {dash} "
        f"({case})": mon.read("titles::TitleLaunched") == 2 and
            mon.read("titles::LastLoadStatus") == 0 and dash == want,
        "no exception, clock running": exc == 0xFFFFFFFF and ticking,
        "AvSetDisplayMode called, CRTC start set": mon.read("hardware::video::SetDisplayModeCalls") >= 1 and fb != 0,
        "framebuffer has drawn pixels": any(w != 0 for w in words),
        f"boot splash faded in before the dashboard ({bs['SplashFrames']} frames, the full picture {bs['SplashFullMs']} ms after the screen came on)":
            bs["SplashFrames"] >= 3 and 0 < bs["SplashFullMs"] < 1000,
        "splash scene (flash, copied to RAM by the 2BL) drawn": bs["SplashSource"] == 2
            and bs["SplashStatus"] == 0,
        f"boot-only pages freed before the dashboard ({boot_freed} pages; the splash code's page free, the encoder "
        f"tables' kept)": boot_freed >= 5 and not boot_code_present and tables_present,
    }, "M5e": launch_reason_checks(mon)}


def fatal_checks(mon):
    for _ in range(240):  # the disk wait times out first
        if mon.read("user_interface::FatalErrorCode"):
            break
        time.sleep(0.25)
    time.sleep(2)
    code, fb = mon.read("user_interface::FatalErrorCode"), mon.read("user_interface::FatalErrorFrameBuffer")
    lit = text = 0
    for y in range(72, 182, 8):  # the two digit cells (x 250-390)
        out = mon.hmp(f"xp/36wx {fb + (y * 640 + 250) * 4:#x}") if fb else ""
        lit += len(re.findall(r"0xffffffff", out))
    for y in range(204, 228, 2):  # the help's title, white text, centred (x 48-592)
        out = mon.hmp(f"xp/544wx {fb + (y * 640 + 48) * 4:#x}") if fb else ""
        text += len(re.findall(r"0x00ffffff", out))
    print(f"  (fatal error {code}, framebuffer {fb:#x}, {lit} lit digit samples, {text} title samples)")
    return {"M5f": {
        "no hard disk: disk fatal error (7/8/9)": code in (7, 8, 9),
        "error code drawn on screen": fb != 0 and lit > 20 and int(mon.hmp("xp/1wx 0xfd600800").split(":")[1].split()[0], 16) & 0x03FFFFFF == fb,
        "the error's title drawn under it": text > 20,
    }}, code


def blank_disk_fatal_checks(mon, reason, seconds, pad=None):
    """A blank (reason 1) or foreign (2) disk: the kernel counts down, formats it, finds no dashboard on it and stops
    with error 13, whose screen says to press X for the File Manager (spec/oxos_internal/30 §4, 32 §1). With pad,
    X is then pressed and the File Manager must start."""
    shown = 0
    for _ in range(4 * (seconds + 30)):
        if not shown and mon.read("user_interface::FatalErrorFrameBuffer") == 0 and mon.read("storage::DiskCountdownSeconds"):
            shown = int(mon.hmp("xp/1wx 0xfd600800").split(":")[1].split()[0], 16)  # the countdown on screen
        if mon.read("user_interface::FatalErrorCode") or mon.read("webserver::Serving"):
            break
        time.sleep(0.25)
    code, serving = mon.read("user_interface::FatalErrorCode"), mon.read("webserver::Serving")
    got = (mon.read("storage::DiskCountdownSeconds"), mon.read("storage::DiskCountdownReason"))
    print(f"  (fatal error {code}, upload server {serving}, countdown {got[0]} s, reason {got[1]})")
    checks = {f"countdown of {seconds} s, reason {reason}": got == (seconds, reason),
              "formatted, then no dashboard and X not pressed: error 13, no File Manager": code == 13 and serving == 0 and
              mon.read("gamepad::XButtonPressed") == 0}
    if pad and code == 13:
        time.sleep(3)  # the error screen sets up the controllers
        pad.set("X", 1)  # the pad's X is X in xemu's mapping
        for _ in range(80):
            if mon.read("webserver::Serving"):
                break
            time.sleep(0.25)
        pad.set("X", 0)
        checks[f"X on the error screen: the File Manager ({mon.read('gamepad::ControllersFound')} pads)"] = \
            mon.read("webserver::Serving") == 1 and mon.read("user_interface::FatalErrorCode") == 0
    if seconds:
        checks["the countdown screen was shown"] = shown != 0
    return {"blank disk": checks}


def upload_checks(mon, port, xbe, fs="exFAT"):
    """The File Manager (spec/oxos_internal/31, 32), opened by X held on a virtual pad during the splash, on a
    freshly formatted disk with no dashboard: the pad seen, the page, the drives,
    an upload read back through a listing, deleting a file and an empty folder, creating a folder, and a launch.
    fs="FATX" (--hdd instead of --blank-hdd): a FATX disk; its C: is not empty."""
    import http.client, json
    def request(method, path, body=None, timeout=30):
        c = http.client.HTTPConnection("127.0.0.1", port, timeout=timeout)
        c.request(method, path, body=body)
        r = c.getresponse()
        data = r.read()
        c.close()
        return r.status, data
    page = None
    for _ in range(240):  # the boot, the format, DHCP
        try:
            status, page = request("GET", "/", timeout=5)
            if status == 200:
                break
        except OSError:
            pass
        time.sleep(0.5)
    pads = mon.read("gamepad::ControllersFound")
    checks = {f"X seen during the splash ({pads} pads)": mon.read("gamepad::XButtonPressed") == 1, "the page is served": bool(page) and
              b"OXOS File Manager" in page}
    if not checks["the page is served"]:
        return {"upload server": checks}
    status, drives = request("GET", "/api/list?path=%2F")
    names = [e["name"] for e in json.loads(drives)] if status == 200 else []
    expected = {"C", "E", "F"} if fs == "exFAT" else {"C", "E"}  # an 8 GB FATX disk has no F:
    checks[f"the drives are listed ({', '.join(names)})"] = expected <= set(names)
    if "F" in names:  # a larger disk: F: takes a file and lists it back
        status, _ = request("PUT", "/api/file?path=%2FF%2Fon%20F.bin", b"F" * 5000)
        _, listing = request("GET", "/api/list?path=%2FF")
        listed = any(e["name"] == "on F.bin" and e["size"] == 5000 for e in json.loads(listing))
        checks[f"a file written to F: and listed ({status})"] = status == 201 and listed
    status, disk = request("GET", "/api/disk")
    disk = json.loads(disk) if status == 200 else {}
    checks[f"the disk is reported ({disk})"] = disk.get("fs") == fs and disk.get("bytes", 0) >= 8 << 30
    checks["the page asks for it (header, Drives: or Folders:)"] = b"/api/disk" in page and b"'Folders:'" in page
    blob = bytes((i * 7 + 3) & 0xFF for i in range(300000))
    status, _ = request("PUT", "/api/file?path=%2FE%2Fupload%20test%2Fdata.bin", blob)
    checks["a 300 KB upload into a new folder (201)"] = status == 201
    status, listing = request("GET", "/api/list?path=%2FE%2Fupload%20test")
    entries = json.loads(listing) if status == 200 else []
    checks["it is listed with its size"] = any(e["name"] == "data.bin" and e["size"] == len(blob) for e in entries)
    if fs == "exFAT":
        status, empty = request("GET", "/api/list?path=%2FY")  # a fresh disk: Y: is an empty folder
        checks[f"an empty folder lists as [] ({status} {empty[:40]!r})"] = status == 200 and json.loads(empty) == []
        # C: holds only what the kernel makes at every boot: C:\OXOS, with its log, extensions and cache folders (spec/oxos_internal/34 §3, 35)
        status, c = request("GET", "/api/list?path=%2FC")
        checks[f"C: on a fresh disk holds only the OXOS folder made at boot ({status} {c[:60]!r})"] = \
            status == 200 and [(e["name"], e["dir"]) for e in json.loads(c)] == [("OXOS", True)]
        status, o = request("GET", "/api/list?path=%2FC%2FOXOS")
        checks[f"C:\\OXOS holds the cache, extensions and log folders made at boot ({status} {o[:80]!r})"] = \
            status == 200 and sorted((e["name"], e["dir"]) for e in json.loads(o)) == [("cache", True), ("extensions", True), ("log", True)]
    status, _ = request("GET", "/api/list?path=%2FE%2F..")
    checks["'..' in a path is refused (400)"] = status == 400
    status, _ = request("PUT", "/api/file?path=%2FE%2Fdelete%20test%2Fx.bin", b"x" * 1000)
    status_full, _ = request("DELETE", "/api/file?path=%2FE%2Fdelete%20test")
    checks[f"a folder with a file in it is not deleted (409: {status_full})"] = status == 201 and status_full == 409
    status, _ = request("DELETE", "/api/file?path=%2FE%2Fdelete%20test%2Fx.bin")
    _, listing = request("GET", "/api/list?path=%2FE%2Fdelete%20test")
    checks[f"a file deleted ({status}, then {listing[:20]!r})"] = status == 200 and json.loads(listing) == []
    status, _ = request("DELETE", "/api/file?path=%2FE%2Fdelete%20test")
    _, listing = request("GET", "/api/list?path=%2FE")
    gone = not any(e["name"] == "delete test" for e in json.loads(listing))
    checks[f"the empty folder deleted ({status})"] = status == 200 and gone
    status, text = request("DELETE", "/api/file?path=%2FE")
    said = b"Cannot delete /E (a top-level folder)" if fs == "exFAT" else b"Cannot delete E:\\ (a drive)"
    checks[f"a drive is not deleted (403: {status} {text!r})"] = status == 403 and text == said
    status, _ = request("PUT", "/api/folder?path=%2FE%2Fnew%20folder")
    again, _ = request("PUT", "/api/folder?path=%2FE%2Fnew%20folder")
    _, listing = request("GET", "/api/list?path=%2FE")
    made = any(e["name"] == "new folder" and e["dir"] for e in json.loads(listing))
    checks[f"a folder created (201: {status}), not twice (409: {again})"] = status == 201 and again == 409 and made
    request("PUT", "/api/file?path=%2FE%2Fnew%20folder%2Fa.txt", b"a")
    request("PUT", "/api/file?path=%2FE%2Fnew%20folder%2Fb.txt", b"b")
    status, _ = request("POST", "/api/rename?path=%2FE%2Fnew%20folder%2Fa.txt&name=c.txt")
    taken, _ = request("POST", "/api/rename?path=%2FE%2Fnew%20folder%2Fb.txt&name=c.txt")
    _, listing = request("GET", "/api/list?path=%2FE%2Fnew%20folder")
    names = sorted(e["name"] for e in json.loads(listing))
    checks[f"a file renamed ({status}), not onto a name in use (409: {taken}): {names}"] = \
        status == 200 and taken == 409 and names == ["b.txt", "c.txt"]
    status, _ = request("POST", "/api/rename?path=%2FE%2Fnew%20folder&name=renamed%20folder")
    _, listing = request("GET", "/api/list?path=%2FE%2Frenamed%20folder")
    moved = sorted(e["name"] for e in json.loads(listing)) == ["b.txt", "c.txt"]
    checks[f"a folder renamed, its files with it ({status})"] = status == 200 and moved
    status, _ = request("POST", "/api/rename?path=%2FE&name=Q")
    checks[f"a drive is not renamed (403: {status})"] = status == 403
    if xbe:
        data = open(xbe, "rb").read()
        status, _ = request("PUT", "/api/file?path=%2FE%2Fapp%2Fdefault.xbe", data)
        checks[f"an XBE uploaded ({len(data) >> 10} KB)"] = status == 201
        status, _ = request("POST", "/api/launch?path=%2FE%2Fapp%2Fdefault.xbe")
        checks["its launch is accepted"] = status == 200
        def running():
            ptr = mon.read_offset("XeImageFileName", 4)
            raw = b"".join(struct.pack("<I", int(w, 16)) for w in re.findall(r"0x([0-9a-f]{8})", mon.hmp(f"x/16wx {ptr:#x}").split(":", 1)[1]))
            return raw.split(b"\0")[0].decode("latin-1")
        launched, wanted = "", "\\Device\\Harddisk0\\Partition1\\app\\default.xbe"
        for _ in range(60):
            time.sleep(0.5)
            launched = running()
            if launched.endswith("default.xbe"):
                break
        checks[f"the console runs it: {launched}"] = launched == wanted
        # The name is set before the loader checks the file. A rejected XBE leaves the File Manager up (its screen shows "launch failed"); a running title has
        # stopped it, so the page no longer answers.
        time.sleep(10)
        try:
            answer, _ = request("GET", "/", timeout=5)
        except OSError as e:
            answer = type(e).__name__
        checks[f"the File Manager has stopped 10 s later ({answer})"] = answer != 200
    return {"upload server": checks}


def blank_disk_checks(path, gigabytes):
    """The disk the kernel formatted: GPT, one exFAT partition from sector 2048, the folders, a clean volume."""
    q = Qcow2(path)
    s0 = q.read(0, 512)
    checks = {"protective MBR (GPT)": s0[510:512] == b"\x55\xAA" and s0[446 + 4] == 0xEE}
    try:
        v = exfat.EXFAT(q)
    except ValueError as e:
        return {**checks, f"exFAT volume ({e})": False}
    names = sorted(x.name for x in v.listdir("/") or [])
    in_c = [x.name for x in v.listdir("/C") or []]
    checks.update({
        "the partition starts at sector 2048": v.lba == 2048,
        "folders C E F X Y Z, none of OXOS's at the root": names == ["C", "E", "F", "X", "Y", "Z"],
        f"C:\\OXOS is /C/OXOS ({in_c})": "OXOS" in in_c,
        f"volume fills the disk ({v.vol_len * 512 >> 20} MiB of {gigabytes} GiB)": v.vol_len * 512 >= (gigabytes << 30) - (4 << 20),
        f"no problems found by the reader ({', '.join(v.problems) or 'none'})": not v.problems,
    })
    return checks


def udf_checks(mon):
    for _ in range(160):
        if mon.read("KiTestDvdDone"):
            break
        time.sleep(0.25)
    vob, ifo = mkudf.TREE["VIDEO_TS"]["VTS_01_1.VOB"], mkudf.TREE["VIDEO_TS"]["VIDEO_TS.IFO"]
    names = "".join(mkudf.plain(n) + "|" for n in mkudf.TREE)
    stage, status = mon.read("filesystems::UdfMountStage"), mon.read("filesystems::UdfMountStatus")
    print(f"  (UDF mount: stage {stage}, status {status:#x}; "
          f"names {read_string(mon, 'KiTestUdfDirNames', 64)!r})")
    return {"M8": {
        "UDF mounted on \\Device\\CdRom0 (volume attributes, offset read)": mon.read("KiTestUdfVolumeOk") == 1,
        "UDF: 300 KB file in two long_ad extents": mon.read("KiTestUdfVobLength") == len(vob) and mon.read("KiTestUdfVobSum") == mkxiso.checksum(vob),
        "UDF: short_ad file, case-insensitive path": mon.read("KiTestUdfIfoSum") == mkxiso.checksum(ifo),
        "UDF: data embedded in the File Entry": mon.read("KiTestUdfSmallOk") == 1,
        "UDF: 16-bit OSTA CS0 file name": mon.read("KiTestUdfWideOk") == 1,
        "UDF: root enumeration in on-disc order": mon.read("KiTestUdfDirOk") == 1
            and read_string(mon, "KiTestUdfDirNames", 64) == names,
        "UDF: read-only media and lookup errors": mon.read("KiTestUdfErrorsOk") == 1,
    }}


def serial_checks(path):
    """Debug serial port (spec/oxos_internal/03_debug_serial.md): the boot banner, the start-up steps and the title load reach the SuperIO."""
    text = open(path, "rb").read().decode("latin1") if os.path.exists(path) else ""
    print(f"  (serial log: {len(text)} bytes, {text.count(chr(10))} lines)")
    return {
        "boot banner (cold boot)": "OXOS kernel (" in text and "): cold boot" in text,
        "start-up steps in order": all(f"boot: {s}" in text for s in ("processor", "memory manager", "IDE", "title launcher")) and
            text.index("boot: processor") < text.index("boot: title launcher"),
        "lines end in CR LF": "\r\n" in text and "\n" not in text.replace("\r\n", ""),
        "title load reported": "load \\Device\\CdRom0\\default.xbe: 0" in text,
    }


def launch_reason_checks(mon):
    reason, ldp = mon.read("titles::LaunchReason"), mon.read("LaunchDataPage")
    page = [int(mon.hmp(f"x/1wx {ldp + off:#x}").split(":")[1].split()[0], 16) for off in (0, 0x400, 0x408)] \
        if ldp else None
    print(f"  (cold-boot launch reason {reason:#x}, LaunchDataPage {ldp:#x} {page})")
    return {
        "no disc + launch reason: dashboard gets LDP {type 1, reason 3, code = reason} (or none if 0)": page == [1, 3, reason] if reason else ldp == 0,
        "the launch page sits below the kernel (spec/stock/05 §5)": ldp == 0 or ldp < 0x80010000,
    }


def large_file_ok(v):
    """E:\\XkLarge.bin as the self-test left it (EXFATLargeFileTest): 0x100000800 bytes, the first 4 KB of its
    pattern below the 4 GB mark, the 2 KB written after it was opened again from there on."""
    e = v.lookup("/E/XkLarge.bin")
    if e is None or e.size != 0x100000800 or e.valid != e.size:
        return False
    clusters = v.clusters(e.first, e.size, e.nofat)

    def read_at(offset, n):
        first, last = offset // v.cs, (offset + n - 1) // v.cs
        data = b"".join(v.disk.read(v.coff(clusters[i]), v.cs) for i in range(first, last + 1))
        return data[offset % v.cs:offset % v.cs + n]
    below = bytes((i * 13 + 5) & 0xFF for i in range(0x1000))
    again = bytes((i * 3 + 1) & 0xFF for i in range(0x800))
    return read_at(0xFFFFF000, 0x1000) == below and read_at(0x100000000, 0x800) == again


def rotated_log_ok(log_part):
    """C:\\OXOS\\log\\selftest*.log, as the FATX self-test suite writes it (spec/oxos_internal/35): 50 numbered lines rotated at 4 KB, 20 a part, so
    selftest_2.log holds lines 0-19, selftest_1.log 20-39 and selftest.log 40-49. log_part(name) reads one part (None when missing)."""
    def line_numbers(name):
        text = (log_part(name) or b"").decode("latin-1")
        return [int(line.split()[2]) for line in text.split("\r\n") if line.startswith("rotation line ")]
    parts = [line_numbers(name) for name in ("selftest_2.log", "selftest_1.log", "selftest.log")]
    if parts != [list(range(0, 20)), list(range(20, 40)), list(range(40, 50))]:
        print(f"  (C:\\OXOS\\log\\selftest*.log, oldest first: {[(len(p), p[:1], p[-1:]) for p in parts]})")
    return parts == [list(range(0, 20)), list(range(20, 40)), list(range(40, 50))] and log_part("selftest_3.log") is None


def disk_checks(hdd_copy):
    """After xemu exits: what the kernel wrote must read back with an independent FATX (or exFAT) reader."""
    q = Qcow2(hdd_copy)
    if is_exfat_disk(q):
        v = exfat.EXFAT(q)
        problems, _, _ = v.check()
        if problems:
            print("  (exFAT checker: " + "; ".join(problems[:6]) + ")")
        listing = v.listdir("/E/XkTest")
        return {
            "E:\\XkTest\\keep.bin readable by the host exFAT reader": v.read_file("/E/XkTest/keep.bin") == b"open xbox kernel fatx test",
            "deleted/renamed entries gone, directory consistent": listing is not None and sorted(x.name for x in listing) == ["keep.bin"],
            "exFAT volume consistent (checksums, chains, bitmap)": not problems,
            "X: folder empty after the format": v.listdir("/X") == [],
            "E:\\XkStale.stale grown past its stale chain reads back": v.read_file("/E/XkStale.stale")
                == bytes((i * 7 + i // 251) & 0xFF for i in range(3 * 16384 + 100)),
            "E:\\XkPre.bin (allocation size 256 KB, 100 bytes written) reads back": v.read_file("/E/XkPre.bin") == b"p" * 100,
            "E:\\XkLarge.bin (over 4 GB) has its size and reads back across the 4 GB mark": large_file_ok(v),
            "C:\\OXOS\\log: a log rotated at its size limit into selftest_2.log, selftest_1.log, selftest.log": rotated_log_ok(lambda name: v.read_file(f"/C/OXOS/log/{name}")),
        }
    e = FATX(q, "E")
    names = sorted(n for n, *_ in e.listdir(e.lookup("XkTest")[2])) if e.lookup("XkTest") else None
    return {
        "E:\\XkTest\\keep.bin readable by an independent FATX reader": e.read_file("XkTest\\keep.bin") == b"open xbox kernel fatx test",
        "deleted/renamed entries gone, directory consistent": names == ["keep.bin"],
        "C:\\OXOS\\log: a log rotated at its size limit into selftest_2.log, selftest_1.log, selftest.log": rotated_log_ok(lambda name: FATX(q,
            "C").read_file(f"OXOS\\log\\{name}")),
    }


# --- Video encoders (make test_encoders)
ENCODERS = {"conexant": (0x8A, 0, 0), "focus": (0xD4, 0x20, 1), "xcalibur": (0xE0, 0x400, 2)}


def xcalibur_expected(mode):
    """The Xcalibur registers a mode set leaves, from spec/stock/20 alone: the row the maps select (§6.1; region not
    Japan), its entries for variant (mode bit 29) << 1 (§2, §6.2). Registers the later steps change (0, 1, 4, 7, 9,
    the indirect port 0x0C-0x0E, 0x58, the flicker filter 0x60-0x66) are left out."""
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
    import gen_av_tables as g
    rows = {g.row_of(b["title"]): b for b in g.section(g.blocks(g.XC_SPEC), "6.2")}
    maps, in_maps = [], False
    for line in open(os.path.join(g.SPEC, g.XC_SPEC), encoding="utf-8"):
        in_maps = (in_maps or line.startswith("### 6.1 ")) and not line.startswith("### 6.2 ")
        m = re.match(r"^\| (0x[0-9A-F]{2}) \| (.*)\|$", line)
        if in_maps and m:
            maps.append([int(c, 16) if c.strip() else 0 for c in m.group(2).split("|")])
    row, hd = mode & 0x7F, (mode >> 30) == 2
    if hd and row == 1 and (mode >> 8) & 0xFF == 7:
        row = 4
    x = maps[row][2] if hd else maps[row][0]
    variant, regs = ((mode >> 29) & 1) << 1, {}
    for w in rows[x]["writes"]:
        if w[0] == "w":
            regs[w[1]] = w[2]
        elif w[1] == "always" or w[1] == f"only when v == {variant}" or w[1] == f"only when (v & 1) == {variant & 1}":
            regs.update(dict(w[2]))
    skip = {0x00, 0x01, 0x04, 0x07, 0x09, 0x0C, 0x0D, 0x0E, 0x58} | set(range(0x60, 0x67))
    return x, {r: v for r, v in regs.items() if r not in skip}


def encoder_checks(mon, name, self_test=False):
    """The video encoder xemu presents (-machine video-encoder=NAME) is found by the bus probe and reported in
    XboxHardwareInfo.Flags as the reference would (spec/stock/17 §1.2); a mode set programs it from its register maps
    (spec/stock/18-20)."""
    address, flag, kind = ENCODERS[name]
    for _ in range(40):
        if mon.read("hardware::video::EncoderAddress") & 0xFF:
            break
        time.sleep(0.25)
    found = mon.read("hardware::video::EncoderAddress") & 0xFF
    flags = mon.read("XboxHardwareInfo")
    print(f"  encoder {name}: found {found:#x}, XboxHardwareInfo.Flags {flags:#x}")
    checks = {
        f"{name}: the probe finds the encoder at {address:#x}": found == address,
        f"{name}: XboxHardwareInfo.Flags encoder bits = {flag:#x}": flags & 0x420 == flag,
    }
    # The dashboard sets an SD mode: Conexant and Focus registers read back as the tables have them; on the Xcalibur every write completes and the values
    # written are the ones spec/stock/20 gives for that mode.
    for _ in range(60):
        if mon.read("hardware::video::EncoderProgrammed"):
            break
        time.sleep(0.25)
    mode, bad = mon.read("hardware::video::EncoderProgrammed"), mon.read("hardware::video::EncoderMismatches")
    print(f"  {name}: mode {mode:#010x} programmed, {bad} registers read back differently or writes failed")
    checks[f"{name}: the dashboard's mode set programs the encoder"] = mode != 0 and bad == 0
    if name == "xcalibur" and mode and not self_test:
        row, want = xcalibur_expected(mode)
        got = {r: mon.read_offset("hardware::video::XcShadow", r * 4) for r in want}
        diff = {f"{r:#04x}": (f"{got[r]:#010x}", f"{v:#010x}") for r, v in want.items() if got[r] != v}
        print(f"  Xcalibur row {row:#04x}: {len(want) - len(diff)} of {len(want)} registers as spec/stock/20 gives them"
              + (f"; differ: {diff}" if diff else ""))
        checks["xcalibur: the registers written are spec/stock/20's for the mode"] = not diff
    if self_test:
        # Self-test build: the misc suite sets every supported mode word on this encoder and checks it.
        for _ in range(240):
            if mon.read("KiTestMiscDone"):
                break
            time.sleep(0.25)
        tried, good = mon.read("KiTestAvModesTried"), mon.read("KiTestAvModesOk")
        want = 15
        print(f"  self-test: {good} of {tried} mode words programmed and checked")
        checks[f"{name}: all {want} supported mode words program the encoder (self-test)"] = tried == want and good == want
        opts = mon.read("KiTestAvOptionsOk")
        print(f"  self-test: encoder options {opts:#x} (0x1f = WSS/CGMS, flicker, captions on/off, caption data, luma)")
        checks[f"{name}: encoder options program the encoder (self-test)"] = opts == 0x1F
    if name == "xcalibur":
        # xemu models the Xcalibur as byte registers, so a 4-byte block read returns its register 0 as the byte count and the kernel rejects it, as it would a
        # short reply on hardware (spec/stock/20 §1); the block writes themselves complete on the bus.
        last = mon.read("hardware::chipset::SMBusLastBlock")
        print(f"  last block transfer {last:#x}")
        checks["xcalibur: a 4-byte block SMBus transfer completes on the bus"] = (last >> 8) & 0x10 != 0
    return checks


# --- Command line
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bios", required=True)
    ap.add_argument("--mcpx-rom", help="MCPX boot ROM dump; without it xemu starts at the open image's reset code")
    ap.add_argument("--map", required=True)
    ap.add_argument("--xemu", default=shutil.which("xemu") or os.path.expanduser("~/.local/bin/xemu"))
    ap.add_argument("--boot-wait", type=float, default=8.0)
    ap.add_argument("--no-disc", action="store_true")
    ap.add_argument("--udf", action="store_true", help="attach a UDF test disc (tests/mkudf.py) and check UDF")
    ap.add_argument("--fatal", action="store_true", help="boot without a hard disk: check the fatal-error report")
    ap.add_argument("--blank-hdd", type=int, metavar="GB", help="attach a blank disk of GB gigabytes: check that the "
                    "kernel formats it as exFAT (spec/oxos_internal/30), then stops with 13 (no dashboard yet)")
    ap.add_argument("--blank-marker", type=int, metavar="SECONDS",
                    help="--blank-hdd: with the blank-disk marker a " "partition delete leaves, asking for SECONDS of countdown")
    ap.add_argument("--countdown", type=int, default=5,
                    help="--blank-hdd: the countdown expected without a marker " "(5 in development builds, 30 in release builds)")
    ap.add_argument("--upload", type=int, metavar="PORT", help="--blank-hdd (exFAT) or --hdd (FATX): xemu's NAT network with host port PORT "
                    "forwarded to the console's 80, and X held on a virtual pad during the boot; check the File "
                    "Manager (spec/oxos_internal/31, 32): its page, a listing, an upload read back, and launching "
                    "--upload-xbe")
    ap.add_argument("--upload-xbe", help="--upload: the XBE to upload and launch")
    ap.add_argument("--x-at-error", action="store_true", help="--blank-hdd: press X on a virtual pad once "
                    "error 13 is on screen, and check that the File Manager starts (spec/oxos_internal/32 §1)")
    ap.add_argument("--foreign", action="store_true", help="--blank-hdd: a PC disk instead (MBR, one NTFS partition)")
    ap.add_argument("--dash", action="store_true", help="check the dashboard phase instead of the self-tests")
    ap.add_argument("--title", help="the kernel test title's XBE (tests/kernel_test_title), placed on the disc as default.xbe")
    ap.add_argument("--title-map")
    ap.add_argument("--relaunch", action="store_true", help="check the title's quick-reboot relaunch instead")
    ap.add_argument("--kept-cache-format", action="store_true",
                    help="a second boot of test_exfat's kept disk: X:'s format from the first boot, as the title reads it after a cold boot")
    ap.add_argument("--extensions", metavar="DIR", help="with --relaunch: the test extensions built in DIR "
                    "(tests/extensions) and an oxos_extensions.ini on the test disk (spec/oxos_internal/34 §9.3)")
    ap.add_argument("--redirect", action="store_true", help="--relaunch through C:\\OXOS\\oxos_settings.ini [launch_redirect]: "
                    "the relaunch must start the disc's redirected\\second.xbe (spec/oxos_internal/33 §6)")
    ap.add_argument("--hdd", help="qcow2 HDD image; a temporary copy is attached (the original is never written)")
    ap.add_argument("--keep-hdd", metavar="PATH", help="keep the disk copy after the run (as PATH), to inspect it")
    ap.add_argument("--serial", action="store_true", help="attach xemu's SuperIO and check the debug serial log")
    ap.add_argument("--iso-launch", metavar="LAUNCHER_XBE", help="boot the ISO launcher (tools/iso_launcher) with an image of --title on an exFAT disk")
    ap.add_argument("--iso-kind", choices=ISO_KINDS, default="plain", help="the image's form (--iso-launch)")
    ap.add_argument("--xbl", action="store_true",
                    help="--dash: move C:\\xboxdash.xbe to C:\\Dashboard\\default.xbe on " "the disk copy and check that the kernel starts it from there")
    ap.add_argument("--dash-order", metavar="PATH,PATH",
                    help="--dash: write C:\\OXOS\\oxos_settings.ini with these dashboards as [dashboard] dashboard_1, dashboard_2, ... on the disk copy")
    ap.add_argument("--evox", action="store_true",
                    help="--dash: put C:\\evoxdash.xbe on the disk copy and check that " "the kernel starts it instead of C:\\xboxdash.xbe")
    ap.add_argument("--encoder", choices=sorted(ENCODERS),
                    help="present this video encoder (xemu -machine " "video-encoder=...) and check that the kernel finds and reports it")
    ap.add_argument("--memory", type=int, choices=(64, 128), default=64, help="the console's RAM in MB; --relaunch "
                    "also checks that the kernel still manages all of it after the quick reboots (spec/stock/09 §10)")
    a = ap.parse_args()

    work = tempfile.mkdtemp(prefix="xk-boot-")
    sock = os.path.join(work, "qmp.sock")
    cfg = os.path.join(work, "xemu.toml")
    hdd_copy = os.path.join(work, "hdd.qcow2")
    dvd = os.path.join(work, "test.iso")
    if a.iso_launch:
        iso_launch_setup(a, work, dvd, hdd_copy)
    elif not a.no_disc:
        if a.udf:
            mkudf.build(dvd)
        else:
            mkxiso.build(dvd, open(a.title, "rb").read() if a.title else None, a.relaunch, a.redirect)
    if a.blank_hdd:
        subprocess.run(["qemu-img", "create", "-q", "-f", "qcow2", hdd_copy, f"{a.blank_hdd}G"], check=True)
        if a.blank_marker is not None or a.foreign:
            q = Qcow2(hdd_copy)
            if a.blank_marker is not None:  # sector 1023 (kernel/src/storage/disk_format.h)
                q.write(1023 * 512, b"OXOS BLANK DISK\0" + struct.pack("<I", a.blank_marker))
            else:  # an MBR with one NTFS partition at sector 2048
                mbr = bytearray(512)
                mbr[446 + 4] = 0x07
                struct.pack_into("<II", mbr, 446 + 8, 2048, 0x100000)
                mbr[510:512] = b"\x55\xAA"
                q.write(0, bytes(mbr))
                q.write(2048 * 512, b"\xEB\x52\x90NTFS    " + bytes(501))
            q.save(hdd_copy + ".new")
            os.replace(hdd_copy + ".new", hdd_copy)
    if a.hdd and not a.iso_launch:
        shutil.copyfile(a.hdd, hdd_copy)
        if a.title and a.title_map and not a.dash:
            write_title_patches(hdd_copy, a.title, a.title_map)
        if a.redirect:
            write_launch_redirect(hdd_copy)
        if a.extensions:
            write_extensions(hdd_copy, a.extensions, a.title_map)
        if a.xbl:  # C:\xboxdash.xbe moved to C:\Dashboard\default.xbe, the XBL Dashboard's loader (spec/oxos_internal/19)
            q = Qcow2(hdd_copy)
            dash = FATX(q, "C").read_file("xboxdash.xbe")
            c = FATXWriter(q, "C")
            c.mkdir("Dashboard")
            c.write_file("Dashboard\\default.xbe", dash)
            c.delete_entry("xboxdash.xbe")
            q.save(hdd_copy + ".new")
            os.replace(hdd_copy + ".new", hdd_copy)
        if a.evox:  # an EvoX-style install: C:\evoxdash.xbe (here a copy of C:\xboxdash.xbe)
            q = Qcow2(hdd_copy)
            FATXWriter(q, "C").write_file("evoxdash.xbe", FATX(q, "C").read_file("xboxdash.xbe"))
            q.save(hdd_copy + ".new")
            os.replace(hdd_copy + ".new", hdd_copy)
        if a.dash_order:  # C:\OXOS\oxos_settings.ini's [dashboard]: dashboard_1, dashboard_2, ... (spec/oxos_internal/19)
            q = Qcow2(hdd_copy)
            w = FATXWriter(q, "C")
            lines = "".join(f"dashboard_{i} = {p}\r\n" for i, p in enumerate(a.dash_order.split(","), 1))
            w.makedirs("OXOS")
            w.write_file("OXOS\\oxos_settings.ini", ("; boot_test.py --dash-order\r\n[dashboard]\r\n" + lines).encode())
            q.save(hdd_copy + ".new")
            os.replace(hdd_copy + ".new", hdd_copy)
    open(cfg, "w").write(
        f"[general]\nshow_welcome = false\nskip_boot_anim = false\n\n[sys]\nmem_limit = '{a.memory}'\n\n[sys.files]\n"
        f"bootrom_path = '{os.path.abspath(a.mcpx_rom) if a.mcpx_rom else ''}'\n"
        f"flashrom_path = '{os.path.abspath(a.bios)}'\n"
        f"eeprom_path = '{os.path.join(work, 'eeprom.bin')}'\n"
        + (f"hdd_path = '{hdd_copy}'\n" if a.hdd or a.blank_hdd else "")
        + ("" if a.no_disc else f"dvd_path = '{dvd}'\n")
        + (f"\n[net]\nenable = true\nbackend = 'nat'\n[net.nat]\n"
           f"forward_ports = [{{host = {a.upload}, guest = 80, protocol = 'tcp'}}]\n" if a.upload else "")
        + (XEMU_PORT1_BINDING if a.upload or a.x_at_error or a.extensions else ""))
    log = open(os.path.join(work, "xemu.log"), "w")
    serial_log = os.path.join(work, "serial.log")
    serial_args = ["-device", "lpc47m157", "-serial", f"file:{serial_log}"] if a.serial else []
    if a.encoder:
        serial_args += ["-machine", f"video-encoder={a.encoder}"]
    pad = None
    if a.upload or a.x_at_error or a.extensions:  # the pad exists before xemu starts, so xemu binds it to a controller port
        pad = VirtualPad()
    if a.upload:  # X held from power-on
        pad.set("X", 1)
    proc = subprocess.Popen([a.xemu, "-config_path", cfg, "-qmp", f"unix:{sock},server,nowait"] + serial_args, stdout=log, stderr=subprocess.STDOUT,
                            env=xemu_env())
    ok = False
    try:
        for _ in range(100):
            if os.path.exists(sock):
                break
            time.sleep(0.1)
        time.sleep(a.boot_wait)
        mon = Monitor(sock, map_symbols(a.map))
        if a.encoder:
            results = {"encoder": encoder_checks(mon, a.encoder, any(k.startswith("?KiTestAvModesOk@") or k == "KiTestAvModesOk" for k in mon.syms))}
        elif a.udf:
            results = udf_checks(mon)
        elif a.fatal:
            results, fatal_code = fatal_checks(mon)
        elif a.upload:
            results = upload_checks(mon, a.upload, a.upload_xbe, "exFAT" if a.blank_hdd else "FATX")
        elif a.blank_hdd:
            results = blank_disk_fatal_checks(mon, 2 if a.foreign else 1, a.blank_marker if a.blank_marker is not None else a.countdown,
                                              pad if a.x_at_error else None)
        elif a.dash:
            results = dash_checks(mon, a.evox, a.xbl, a.dash_order or "")
        elif a.kept_cache_format:
            results = {"exFAT cache partitions": kept_cache_format_checks(mon, a.title_map)}
        elif a.relaunch:
            results = {"M5d-2": title_checks(mon, a.title_map, True, extensions=bool(a.extensions), pad=pad if a.extensions else None),
                       "RAM": ram_checks(mon, a.memory)}
            if a.extensions:
                results["extensions"] = extension_checks(mon, 5, hdd_copy)  # with the trainer menu's two launches
        elif a.iso_launch:
            results = {f"disc image ({a.iso_kind})": iso_checks(mon, a)}
        else:
            results = run_checks(mon, a.hdd, a.title_map)
        if a.serial:
            results["serial"] = serial_checks(serial_log)
            if a.extensions:  # the SDK's examples write what their sources show, on the serial port too
                text = open(serial_log, "rb").read().decode("latin1") if os.path.exists(serial_log) else ""
                results["serial"]["the examples' lines on the serial port: example_kernel_hook's start, example_title_event's title"] = (
                    any(line.startswith("[example_kernel_hook] Starting extension ") for line in text.splitlines())
                    and any(line.startswith('[example_title_event] "OXOS kernel test title" is starting') for line in text.splitlines()))
        ok = True
        for group, checks in results.items():
            for name, passed in checks.items():
                print(f"  [{'PASS' if passed else 'FAIL'}] {group}: {name}")
                ok &= passed
    finally:
        if pad:
            pad.close()
        proc.terminate()
        try:
            proc.wait(5)
        except subprocess.TimeoutExpired:
            proc.kill()
        log.close()
    if a.fatal:  # xemu writes the EEPROM image back to its file
        e = open(os.path.join(work, "eeprom.bin"), "rb").read() if os.path.exists(os.path.join(work, "eeprom.bin")) else b""
        history = struct.unpack_from("<H", e, 0xFC)[0] if len(e) >= 256 else 0
        passed = fatal_code > 4 and bool(history & (1 << (fatal_code - 5)))
        print(f"  [{'PASS' if passed else 'FAIL'}] M5f: EEPROM error history bit {fatal_code - 5} (word {history:#06x})")
        ok &= passed
    if ok and a.hdd and not a.dash and not a.relaunch and not a.udf and not a.encoder and not a.iso_launch \
            and not a.upload:  # the self-test's files; an upload run has none
        for name, passed in {**disk_checks(hdd_copy), **lba48_disk_checks(Qcow2(hdd_copy), Qcow2(a.hdd))}.items():
            print(f"  [{'PASS' if passed else 'FAIL'}] disk: {name}")
            ok &= passed
    if a.blank_hdd:  # xemu has written the disk back
        extra = {}
        if a.upload:  # the upload, read back from the disk
            try:
                v = exfat.EXFAT(Qcow2(hdd_copy))
                got = v.read_file("/E/upload test/data.bin")
                extra["the upload on the disk, byte for byte"] = got == bytes((i * 7 + 3) & 0xFF for i in range(300000))
            except Exception as e:
                extra[f"the upload on the disk ({e})"] = False
        for name, passed in {**blank_disk_checks(hdd_copy, a.blank_hdd), **extra}.items():
            print(f"  [{'PASS' if passed else 'FAIL'}] blank disk: {name}")
            ok &= passed
    if os.path.exists(hdd_copy) and a.keep_hdd:
        shutil.move(hdd_copy, a.keep_hdd)
    elif os.path.exists(hdd_copy):
        os.remove(hdd_copy)  # 1 GB; logs are kept on failure, the disk copy is not
    print("RESULT:", "PASS" if ok else f"FAIL (logs in {work})")
    if ok:
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
