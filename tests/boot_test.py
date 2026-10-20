#!/usr/bin/env python3
"""Boot a BIOS image in xemu and check milestone conditions through the QMP monitor.

The kernel's boot self-tests (kernel/src/ke/selftest.cpp) leave results in globals that are
located through the linker map and read with HMP `x`.

usage: boot_test.py --bios build/selftest/bios.bin --map build/selftest/xboxkrnl.map [--mcpx-rom mcpx.bin] [--xemu PATH]
"""
import argparse
import hashlib
import hmac, json, os, re, shutil, socket, struct, subprocess, sys, tempfile, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qcow2 import Qcow2
import exfat  # noqa: E402
from fatx import Fatx  # noqa: E402
import mkxiso  # noqa: E402
import mkudf  # noqa: E402


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
        s = socket.socket(socket.AF_UNIX)
        s.connect(self.path)
        f = s.makefile("rw")
        f.readline()
        f.write('{"execute":"qmp_capabilities"}\n'); f.flush(); f.readline()
        f.write(json.dumps({"execute": "human-monitor-command", "arguments": {"command-line": cmd}}) + "\n")
        f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r:
                break
        s.close()
        return r.get("return", r)

    def addr(self, name):
        for k, v in self.syms.items():
            if k in ("_" + name, name) or k.startswith(f"?{name}@@"):
                return v
        sys.exit(f"symbol {name} not in map")

    def read(self, name):
        out = self.hmp(f"x/1wx {self.addr(name):#x}")
        return int(out.split(":")[1].split()[0], 16)

    def read_offset(self, name, offset):
        out = self.hmp(f"x/1wx {self.addr(name) + offset:#x}")
        return int(out.split(":")[1].split()[0], 16)

    def rate(self, name, seconds=2.0):
        v0, t0 = self.read(name), time.monotonic()
        time.sleep(seconds)
        v1, t1 = self.read(name), time.monotonic()
        return (v1 - v0) / (t1 - t0)


def run_checks(mon, hdd=None, title_map=None):
    exc = mon.read("KiLastExceptionVector")
    tick_rate = mon.rate("KeTickCount")
    timer_rate = mon.rate("KiTestTimerTicks")
    eip = int(re.search(r"EIP=([0-9a-f]+)", mon.hmp("info registers")).group(1), 16)
    values = {name: mon.read(name) for name in (
        "KiBootStage", "KiTestDpcCount", "KiTestOneShotFired", "KiTestEventState",
        "KiTestRtcCount", "KiTestRtcDisconnected", "KiTestRtcIrqlOk")}
    m2b = {name: mon.read(name) for name in (
        "KiBugCheckCode", "KiTestPingA", "KiTestPingB", "KiTestSleepMs", "KiTestSemaphoreConsumed",
        "KiTestMutantOk", "KiTestWaitAnyIndex", "KiTestWaitAllStatus", "KiTestTimeoutStatus", "KiTestJoinOk",
        "KiTestFpuOk", "KiTestThreadsDone")}
    m2bp = {name: mon.read(name) for name in (
        "KiTestApcKernelRan", "KiTestApcNormalAtPassive", "KiTestApcRewaitOk", "KiTestSpecialApcAtApcLevel",
        "KiTestUserApcRan", "KiTestUserApcStatus", "KiTestAlertStatus", "KiTestCriticalRegionOk",
        "KiTestSuspendOk", "KiTestQueueProcessed", "KiTestQueueTimeout", "KiTestDeviceQueueOk",
        "KiTestBoostPriority", "KiTestApcSuiteDone")}
    m2c = {name: mon.read(name) for name in (
        "KiTestMmContiguousOk", "KiTestMmAlignedOk", "KiTestMmReuseOk", "KiTestMmRelocateOk", "KiTestMmSystemOk", "KiTestMmIoSpaceOk",
        "KiTestMmPoolOk", "KiTestMmLayoutOk", "KiTestMmThreadChurnPages", "KiTestMmContiguousVa", "KiTestMmDone",
        "KiTestVmReserveCommitOk", "KiTestVmImageOk", "KiTestVmTopDownOk", "KiTestVmFreeRulesOk", "KiTestVmProtectOk",
        "KiTestVmPagesRestored", "KiTestVmStatsOk")}
    m3a = {name: mon.read(name) for name in (
        "KiTestObHandlesOk", "KiTestObNamedOk", "KiTestObNamespaceOk", "KiTestObNameErrorsOk", "KiTestPsThreadOk",
        "KiTestPsNotifyCount", "KiTestNtApcOk", "KiTestNtWaitMultipleOk", "KiTestObDuplicateOk", "KiTestObPoolBalanced",
        "KiTestObDone")}
    m3b = {name: mon.read(name) for name in (
        "KiTestIoOpenOk", "KiTestIoSyncRwOk", "KiTestIoQueryOk", "KiTestIoAsyncOk", "KiTestIoApcOk",
        "KiTestIoPortOk", "KiTestIoIoctlOk", "KiTestIoRelativeOk", "KiTestIoErrorsOk", "KiTestIoStartIoOk",
        "KiTestIoCloseOk", "KiTestIoDeleteOk", "KiTestIoPoolBalanced", "KiTestIoDone")}
    spin_a, spin_b = mon.rate("KiTestSpinA", 1.0), mon.rate("KiTestSpinB", 1.0)
    switches = mon.read_offset("KiPcr", 0x28 + 0x20)

    print(f"exception vector {exc:#x} code {mon.read('KiLastExceptionCode'):#x} at {mon.read('KiLastExceptionEip'):#x}, EIP {eip:#x}, clock {tick_rate:.0f}/s, periodic timer DPC {timer_rate:.1f}/s")
    print("  " + ", ".join(f"{k}={v}" for k, v in values.items()))
    print("  " + ", ".join(f"{k}={v:#x}" if k.endswith(("Status", "Code")) else f"{k}={v}" for k, v in m2b.items()))
    print("  " + ", ".join(f"{k}={v:#x}" if k.endswith("Status") or k.endswith("Timeout") else f"{k}={v}" for k, v in m2bp.items()))
    print("  " + ", ".join(f"{k}={v:#x}" if k.endswith("Va") else f"{k}={v}" for k, v in m2c.items()))
    print("  " + ", ".join(f"{k}={v}" for k, v in m3a.items()))
    print(f"  spinner rates {spin_a:.0f}/s and {spin_b:.0f}/s, context switches {switches}")
    return {
        "M1": {
            "reached idle (stage 0x30)": values["KiBootStage"] == 0x30,
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
            "no bug check": m2b["KiBugCheckCode"] == 0,
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
            "user APC ran and ended alertable wait": m2bp["KiTestUserApcRan"] == 1
                and m2bp["KiTestUserApcStatus"] == 0xC0,
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
            "settings: game region, video standard, unknown index": mon.read("KiTestHalSettingsOk") == 1
                and mon.read("KiTestHalRegion") != 0,
            "save setting + read back, checksum kept": mon.read("KiTestHalSaveOk") == 1,
            "SMC tray state with a disc = 0x60": mon.read("KiTestHalTrayState") == 0x60,
            "HAL suite finished": mon.read("KiTestHalDone") == 1,
        },
        "M5a": rtl_checks(mon),
        "M5b": {f"SEH {n}": mon.read(f"KiTestSeh{n}Ok") == 1
                for n in ["Divide", "PageFault", "Raise", "Unwind", "Nested", "Capture"]}
               | {"SEH suite finished": mon.read("KiTestSehDone") == 1},
        "M5c": {f"Xc {n}": mon.read(f"KiTestXc{n}Ok") == 1 for n in ["Des", "ModExp", "Rsa", "Pkcs1", "Vector"]}
               | {"Xc suite finished": mon.read("KiTestXcDone") == 1},
        "M5d-1": misc_checks(mon),
    }


def is_exfat_disk(q):
    """A GPT disk (protective MBR): the exFAT layout (spec/oxos-internal/exfat-storage-spec.md)."""
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
        raw = b"".join(struct.pack("<I", int(mon.hmp(f"x/1wx {buf + i:#x}").split(":")[1].split()[0], 16))
                       for i in range(0, length + 3, 4))
        return raw[:length]
    password = hmac.new(mem("XboxHDKey", 16), ansi("HalDiskModelNumber") + ansi("HalDiskSerialNumber"),
                        hashlib.sha1).digest() + bytes(12)
    before, after = mon.read("IdexDiskSecurityWord") & 0xFFFF, mon.read("KiTestIdexSecurityAfter")
    print(f"  (IDENTIFY word 128: {before:#06x} at boot, {after:#06x} after)")
    return {
        "IDE pass-through: IDENTIFY, buffer rule, failing command completes": mon.read("KiTestIdexPassThroughOk") == 1,
        "drive password = HMAC-SHA1(XboxHDKey, model, serial)": mem("KiTestIdexPassword", 32) == password,
        "locked drive unlocked at boot (word 128: locked bit gone)": not (before & 4)
            or (after & 6) == 2 and mon.read("IdexDiskSecurityUnlocked") & 0xFF == 1,
        "disk identified: capacity of the image (64-bit size)": mon.read("IdexDiskLengthInSectors")
            | mon.read_offset("IdexDiskLengthInSectors", 4) << 32 == q.size // 512,
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
        "exFAT layout detected (GPT, exFAT partition)": mon.read("IdexDiskExfatLayout") & 0xFF == 1,
        "Partition0 sector 0 = the reserved-area copy at sector 1024": [
            mon.read_offset("KiTestIdexPartitionSig", 4 * w) for w in range(4)] == list(struct.unpack("<4I", reserved)),
        "Partition0 refuses access past the reserved area; folder partitions refuse raw reads":
            mon.read("KiTestIdexFolderOk") == 1,
        "write + read back (page-crossing pool buffer)": mon.read("KiTestIdexWriteOk") == 1,
        "IOCTL partition info + drive geometry": mon.read("KiTestIdexIoctlOk") == 1,
        "IDE suite finished": mon.read("KiTestIdexDone") == 1,
    }


# 48-bit addressing (spec/oxos-internal/lba48.md): the writes the kernel's self-test makes on a disk past the 28-bit
# limit, as (first LBA, sector count, tag); kernel/src/ke/selftest_idex.cpp has the same list.
def lba48_writes(sectors):
    return [(0x0FFFFFFE, 1, 1), (0x0FFFFFFF, 1, 2), (0x10000000, 1, 3), (0x10000001, 1, 4), (0x0FFFFF80, 512, 5),
            (0x0FFFFE00, 256, 6), (0x20000000, 256, 7), (sectors - 8, 8, 8)]


def lba48_kernel_checks(mon, q):
    if q.size // 512 < 0x20001000:
        return {}
    return {
        "48-bit drive detected (IDENTIFY words 83/86)": mon.read("IdexDiskLba48") & 0xFF == 1,
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
            want = struct.pack("<4I", 0x3834424C, lba & 0xFFFFFFFF, lba >> 32, tag) + struct.pack(
                "<124I", *(((lba & 0xFFFFFFFF) * 2654435761 & 0xFFFFFFFF) ^ i for i in range(4, 128)))
            placed &= data[n * 512:(n + 1) * 512] == want
    # The first 0x400 sectors (before X:) and the first sectors of each partition are unchanged, apart from the
    # self-test's own write at sectors 0x3F0-0x3F7 and the refurb record in sector 3 (the kernel counts the power
    # cycle there at every boot), which must still not hold a test pattern.
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


def fatx_checks(mon, hdd):
    if hdd is None:
        return {"HDD image attached": False}
    q = Qcow2(hdd)
    if is_exfat_disk(q):
        v = exfat.Exfat(q)
        dash = v.read_file("/C/xboxdash.xbe")
        names = "".join(x.name + "|" for x in v.listdir("/E"))
        clusters, serial = min(v.count * v.cs, 2 << 40) // 0x4000, v.serial
    else:
        c, e = Fatx(q, "C"), Fatx(q, "E")
        dash = c.read_file("xboxdash.xbe")
        names = "".join(n + "|" for n, *_ in e.listdir())
        clusters, serial = e.clusters, e.serial
    s = 0
    for b in dash:
        s = (s * 31 + b) & 0xFFFFFFFF
    return {
        "C:\\xboxdash.xbe read in odd pieces matches the image": mon.read("KiTestFatxReadOk") == 1
            and mon.read("KiTestFatxReadLength") == len(dash) and mon.read("KiTestFatxReadSum") == s,
        "file information (standard, network open, name, mode)": mon.read("KiTestFatxInfoOk") == 1,
        "E:\\ enumeration matches the image": mon.read("KiTestFatxDirOk") == 1
            and read_string(mon, "KiTestFatxDirNames", 64) == names[:62],
        "create dir + 100 KB file, reopen and verify": mon.read("KiTestFatxWriteOk") == 1,
        "mask query, rename, delete, non-empty dir refused": mon.read("KiTestFatxRenameDeleteOk") == 1,
        "error statuses (not found, collision, dir/non-dir, names)": mon.read("KiTestFatxErrorsOk") == 1,
        "volume information (size, serial, FATX)": mon.read("KiTestFatxVolumeOk") == 1
            and mon.read("KiTestFatxTotalClusters") == clusters and mon.read("KiTestFatxSerial") == serial,
        "free clusters balanced": mon.read("KiTestFatxFreeBalanced") == 1,
        "FATX suite finished": mon.read("KiTestFatxDone") == 1,
        **exfat_format_checks(mon, q)}


def exfat_format_checks(mon, q):
    """exFAT disk: a title-style format of X: empties the folder; E: refuses it (exfat-storage-spec.md §5)."""
    if not is_exfat_disk(q):
        return {}
    total, free = mon.read("KiTestExfatFormatTotal"), mon.read("KiTestExfatFormatFree")
    print(f"  (X: after the format: {free} of {total} 16 KB units free)")
    return {
        "X: formatted through its volume handle: folder emptied, header serial shown": mon.read("KiTestExfatFormatOk") == 1,
        "X: reports its retail 750 MB, less only its directory's cluster": total == 0x2EE00000 // 0x4000
            and free == total - 1,
        "E: refuses a format through its volume handle": mon.read("KiTestExfatRefuseOk") == 1,
    }


def dvd_checks(mon):
    big = mkxiso.TREE["media"]["big.bin"]
    names = "".join(n + "|" for n in sorted(mkxiso.TREE, key=lambda n: n.upper().encode()))
    return {
        "\\Device\\CdRom0 + CHECK_VERIFY with a disc": mon.read("KiTestDvdDeviceOk") == 1,
        "XDVDFS: 300 KB file (DMA chunks) matches the image": mon.read("KiTestDvdBigLength") == len(big)
            and mon.read("KiTestDvdBigSum") == mkxiso.checksum(big),
        "XDVDFS: \\??\\CdRom0: link, nested dirs, offsets, EOF": mon.read("KiTestDvdReadOk") == 1,
        "XDVDFS: root enumeration in tree order": mon.read("KiTestDvdDirOk") == 1
            and read_string(mon, "KiTestDvdDirNames", 64) == names,
        "XDVDFS: write-protected media, lookup errors": mon.read("KiTestDvdErrorsOk") == 1,
        "RAW: Partition0 volume reads, sector alignment, no names": mon.read("KiTestDvdRawOk") == 1,
        "DVD suite finished": mon.read("KiTestDvdDone") == 1,
    }


def title_checks(mon, title_map, relaunch=False):
    if not title_map:
        return {"test title built": False}
    syms = map_symbols(title_map)
    base = syms["_XkTitleResults"]
    for _ in range(40):  # the title starts after the kernel suites
        if mon.read("XeTitleLaunched"):
            break
        time.sleep(0.25)
    for i in range(160):  # includes the quick reboot into media\second.xbe
        v = mon.hmp(f"x/1wx {base:#x}")
        if os.environ.get("XK_DEBUG"):
            print(i, time.time(), v.strip(), mon.read("XeTitleLaunched"), mon.read("KiQuickRebootCount"))
        if v.split(":")[1].split()[0] == "0x54544b58":
            break
        time.sleep(0.25)
    count = 9 + 16 + 3 + 16
    out = mon.hmp(f"x/{count}wx {base:#x}")  # one command: the relaunch test moves on after 8 s
    words = [int(w, 16) for line in out.splitlines() for w in line.split(":")[1].split()][:count]

    def text(ws):
        return b"".join(struct.pack("<I", w) for w in ws).split(b"\0")[0].decode("latin-1")
    name, second = text(words[9:25]), text(words[28:44])
    checks = {
        "title launched from \\Device\\CdRom0\\default.xbe": words[1] == 1
            and mon.read("XeTitleLaunched") == (3 if relaunch else 1),
        "XeImageFileName": name == "\\Device\\CdRom0\\default.xbe",
        "kernel thunks resolved: KeTickCount (data) + sleep": words[2] == 1,
        "pool, disc file read, system thread from the title": words[3] == 1 and words[4] == 1 and words[5] == 1,
        "path symbolic link \\??\\T: -> Partition1\\XkTest": words[6] == 1,
        "XeLoadSection / XeUnloadSection (digest-checked)": words[7] == 1,
        "XcSHA known answer": words[8] == 1,
        "title finished": words[0] == 0x54544B58,
    }
    if relaunch:
        for _ in range(80):
            if mon.read("KiQuickRebootCount") == 2 and mon.read("XeTitleLaunched") == 2:
                break
            time.sleep(0.25)
        # What the kernel wrote to the launch page for the dashboard (a dashboard may consume the page itself, as
        # the stock one does).
        page = [mon.read_offset("XeDashboardLaunchData", 4 * i) for i in range(3)]
        checks["missing image from the launch page: dashboard with LDP {type 1, reason 1, code 1}"] = \
            mon.read("XeTitleLaunched") == 2 and page == [1, 1, 1]
        del checks["path symbolic link \\??\\T: -> Partition1\\XkTest"]  # keep.bin comes from the FATX self-test
        checks |= {
            "quick reboot (HalReturnToFirmware(2)): kernel re-entered (twice), LaunchDataPage re-adopted":
                mon.read("KiQuickRebootCount") == 2 and mon.read("MmPersistedPages") >= 1,
            "LaunchDataPage launch of media;second.xbe, first run's results carried over": words[25] == 1
                and second == "\\Device\\CdRom0\\media\\second.xbe",
            "persisted launch page contents (type, title id, flags, data)": words[27] == 1,
            "\\??\\D: -> launch directory": words[26] == 1,
        }
    else:
        checks["no relaunch without the flag file"] = words[25] == 0 and mon.read("KiQuickRebootCount") == 0
    return checks


def rtl_checks(mon):
    for _ in range(80):
        if mon.read("KiTestRtlDone"):
            break
        time.sleep(0.25)
    names = ["Sprintf", "Interlocked", "CritSec", "RwLock", "Strings", "Math", "Refurb", "Time", "Dbg", "Boost", "Fpu"]
    out = {f"Rtl {n}": mon.read(f"KiTestRtl{n}Ok") == 1 for n in names}
    out["Rtl suite finished"] = mon.read("KiTestRtlDone") == 1
    return out


def hw_info_ok(mon):
    flags, revs = mon.read("XboxHardwareInfo"), mon.read_offset("XboxHardwareInfo", 4)
    boot_id = int(mon.hmp("xp/1wx 0xfd000000").split(":")[1].split()[0], 16)
    print(f"  (XboxHardwareInfo: flags {flags:#x}, GPU {revs & 0xFF:#x}, MCPX {revs >> 8 & 0xFF:#x}; "
          f"NV2A boot ID {boot_id:#x})")
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
        "ACPI SCI (IRQ 12) delivered and acknowledged (PM timer overflow)": mon.read("HalpSciCount") >= 1,
        "XboxHardwareInfo: GpuRevision = NV2A boot ID, McpRevision set": hw_info_ok(mon),
        "TV encoder: every SD and HD mode word programs the Conexant registers (read back)":
            mon.read("KiTestAvModesTried") == 15 and mon.read("KiTestAvModesOk") == 15,
        "misc suite finished": mon.read("KiTestMiscDone") == 1,
    }


def dash_checks(mon):
    for _ in range(60):
        if mon.read("XeTitleLaunched"):
            break
        time.sleep(0.25)
    time.sleep(4)
    exc = mon.read("KiLastExceptionVector")
    t0 = mon.read("KeTickCount")
    time.sleep(0.5)
    ticking = mon.read("KeTickCount") > t0
    start = int(mon.hmp("xp/1wx 0xfd600800").split(":")[1].split()[0], 16)
    fb = start & 0x03FFFFFF
    words = []
    for off in range(0, 640 * 4 * 40, 4096):  # first 40 lines of a 640-wide 32 bpp surface
        out = mon.hmp(f"xp/16wx {fb + off:#x}")
        words += [int(w, 16) for w in re.findall(r"0x([0-9a-f]{8})", out.split(":", 1)[1] if ":" in out else "")]
    return {"M4b-2": {
        "dashboard (C:\\xboxdash.xbe) launched without a disc": mon.read("XeTitleLaunched") == 2
            and mon.read("XeLastLoadStatus") == 0,
        "no exception, clock running": exc == 0xFFFFFFFF and ticking,
        "AvSetDisplayMode called, CRTC start set": mon.read("AvpSetDisplayModeCalls") >= 1 and fb != 0,
        "framebuffer has drawn pixels": any(w != 0 for w in words),
        "boot splash shown (all fade frames) before the dashboard": mon.read("HalpSplashFrames") == 24,
        "splash image read from flash bank 1 and decoded": mon.read("HalpSplashSource") == 2
            and mon.read("HalpSplashStatus") == 0,
    }, "M5e": launch_reason_checks(mon)}


ENCODERS = {"conexant": (0x8A, 0, 0), "focus": (0xD4, 0x20, 1), "xcalibur": (0xE0, 0x400, 2)}


def encoder_checks(mon, name, self_test=False):
    """The video encoder xemu presents (-machine video-encoder=NAME) is found by the bus probe and reported in
    XboxHardwareInfo.Flags as the reference would (spec/oxos-internal/tv-encoders.md §2, spec/stock/19 §1)."""
    address, flag, kind = ENCODERS[name]
    for _ in range(40):
        if mon.read("AvpEncoderAddress") & 0xFF:
            break
        time.sleep(0.25)
    found = mon.read("AvpEncoderAddress") & 0xFF
    flags = mon.read("XboxHardwareInfo")
    print(f"  encoder {name}: found {found:#x}, XboxHardwareInfo.Flags {flags:#x}")
    checks = {
        f"{name}: the probe finds the encoder at {address:#x}": found == address,
        f"{name}: XboxHardwareInfo.Flags encoder bits = {flag:#x}": flags & 0x420 == flag,
    }
    if name == "conexant":
        # The dashboard sets an SD mode: the generated CX25871 registers must read back as written
        # (tools/gen_tv_encoders.py; xemu's encoder model stores registers).
        for _ in range(60):
            if mon.read("AvpEncoderProgrammed"):
                break
            time.sleep(0.25)
        mode, bad = mon.read("AvpEncoderProgrammed"), mon.read("AvpEncoderMismatches")
        print(f"  Conexant: mode {mode:#010x} programmed, {bad} registers read back differently")
        checks["conexant: an SD mode set programs the encoder (generated registers read back)"] = mode != 0 and bad == 0
    if self_test and name in ("conexant", "focus"):
        # Self-test build: the misc suite sets every supported mode word on this encoder and reads the registers back.
        for _ in range(240):
            if mon.read("KiTestMiscDone"):
                break
            time.sleep(0.25)
        tried, good = mon.read("KiTestAvModesTried"), mon.read("KiTestAvModesOk")
        want = 15
        print(f"  self-test: {good} of {tried} mode words programmed and read back")
        checks[f"{name}: all {want} supported mode words program the encoder (self-test)"] = tried == want and good == want
    if name == "xcalibur":
        st = mon.read("AvpEncoderWideStatus")
        print(f"  32-bit read of Xcalibur register 0: status {st:#x}, last block {mon.read('HalpSMBusLastBlock'):#x}")
        # xemu models the Xcalibur as 256 byte registers, so a block read returns its register 0 (zero) as the byte
        # count and the kernel rejects it, as it would a short reply on hardware (spec/stock/19 §2). What xemu can
        # show is that the block transfer itself completes on the bus.
        last = mon.read("HalpSMBusLastBlock")
        checks["xcalibur: a 4-byte block SMBus transfer completes on the bus"] = (last >> 8) & 0x10 != 0
    return checks


def fatal_checks(mon):
    for _ in range(240):  # the disk wait times out first
        if mon.read("IdexFatalErrorCode"):
            break
        time.sleep(0.25)
    time.sleep(2)
    code, fb = mon.read("IdexFatalErrorCode"), mon.read("XkFatalErrorFrameBuffer")
    lit = 0
    for y in range(185, 295, 8):  # the two digit cells (x 250-390)
        out = mon.hmp(f"xp/36wx {fb + (y * 640 + 250) * 4:#x}") if fb else ""
        lit += len(re.findall(r"0xffffffff", out))
    print(f"  (fatal error {code}, framebuffer {fb:#x}, {lit} lit samples)")
    return {"M5f": {
        "no hard disk: disk fatal error (7/8/9)": code in (7, 8, 9),
        "error code drawn on screen": fb != 0 and lit > 20
            and int(mon.hmp("xp/1wx 0xfd600800").split(":")[1].split()[0], 16) & 0x03FFFFFF == fb,
    }}, code


def udf_checks(mon):
    for _ in range(160):
        if mon.read("KiTestDvdDone"):
            break
        time.sleep(0.25)
    vob, ifo = mkudf.TREE["VIDEO_TS"]["VTS_01_1.VOB"], mkudf.TREE["VIDEO_TS"]["VIDEO_TS.IFO"]
    names = "".join(mkudf.plain(n) + "|" for n in mkudf.TREE)
    print(f"  (UDF mount: stage {mon.read('UdfMountStage')}, status {mon.read('UdfMountStatus'):#x}; "
          f"names {read_string(mon, 'KiTestUdfDirNames', 64)!r})")
    return {"M8": {
        "UDF mounted on \\Device\\CdRom0 (volume attributes, offset read)": mon.read("KiTestUdfVolumeOk") == 1,
        "UDF: 300 KB file in two long_ad extents": mon.read("KiTestUdfVobLength") == len(vob)
            and mon.read("KiTestUdfVobSum") == mkxiso.checksum(vob),
        "UDF: short_ad file, case-insensitive path": mon.read("KiTestUdfIfoSum") == mkxiso.checksum(ifo),
        "UDF: data embedded in the File Entry": mon.read("KiTestUdfSmallOk") == 1,
        "UDF: 16-bit OSTA CS0 file name": mon.read("KiTestUdfWideOk") == 1,
        "UDF: root enumeration in on-disc order": mon.read("KiTestUdfDirOk") == 1
            and read_string(mon, "KiTestUdfDirNames", 64) == names,
        "UDF: read-only media and lookup errors": mon.read("KiTestUdfErrorsOk") == 1,
    }}


def serial_checks(path):
    """Debug serial port (spec/stock/18): the boot banner, the start-up steps and the title load reach the SuperIO."""
    text = open(path, "rb").read().decode("latin1") if os.path.exists(path) else ""
    print(f"  (serial log: {len(text)} bytes, {text.count(chr(10))} lines)")
    return {
        "boot banner (cold boot)": "OXOS kernel (" in text and "): cold boot" in text,
        "start-up steps in order": all(f"boot: {s}" in text for s in ("processor", "memory manager", "IDE", "title launcher"))
            and text.index("boot: processor") < text.index("boot: title launcher"),
        "lines end in CR LF": "\r\n" in text and "\n" not in text.replace("\r\n", ""),
        "title load reported": "load \\Device\\CdRom0\\default.xbe: 0" in text,
    }


def launch_reason_checks(mon):
    reason, ldp = mon.read("XeLaunchReason"), mon.read("LaunchDataPage")
    page = [int(mon.hmp(f"x/1wx {ldp + off:#x}").split(":")[1].split()[0], 16) for off in (0, 0x400, 0x408)] \
        if ldp else None
    print(f"  (cold-boot launch reason {reason:#x}, LaunchDataPage {ldp:#x} {page})")
    return {
        "no disc + launch reason: dashboard gets LDP {type 1, reason 3, code = reason} (or none if 0)":
            page == [1, 3, reason] if reason else ldp == 0,
        "the launch page sits below the kernel (spec/stock/05 §5)": ldp == 0 or ldp < 0x80010000,
    }


def disk_checks(hdd_copy):
    """After xemu exits: what the kernel wrote must read back with an independent FATX (or exFAT) reader."""
    q = Qcow2(hdd_copy)
    if is_exfat_disk(q):
        v = exfat.Exfat(q)
        problems, _, _ = v.check()
        if problems:
            print("  (exFAT checker: " + "; ".join(problems[:6]) + ")")
        listing = v.listdir("/E/XkTest")
        return {
            "E:\\XkTest\\keep.bin readable by the host exFAT reader": v.read_file("/E/XkTest/keep.bin")
                == b"open xbox kernel fatx test",
            "deleted/renamed entries gone, directory consistent": listing is not None
                and sorted(x.name for x in listing) == ["keep.bin"],
            "exFAT volume consistent (checksums, chains, bitmap)": not problems,
            "X: folder empty after the format": v.listdir("/X") == [],
        }
    e = Fatx(q, "E")
    names = sorted(n for n, *_ in e.listdir(e.lookup("XkTest")[2])) if e.lookup("XkTest") else None
    return {
        "E:\\XkTest\\keep.bin readable by an independent FATX reader": e.read_file("XkTest\\keep.bin")
            == b"open xbox kernel fatx test",
        "deleted/renamed entries gone, directory consistent": names == ["keep.bin"],
    }


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
    ap.add_argument("--dash", action="store_true", help="check the dashboard phase instead of the self-tests")
    ap.add_argument("--title", help="the kernel test title's XBE (tests/kernel-test-title), placed on the disc as default.xbe")
    ap.add_argument("--title-map")
    ap.add_argument("--relaunch", action="store_true", help="check the title's quick-reboot relaunch instead")
    ap.add_argument("--hdd", help="qcow2 HDD image; a temporary copy is attached (the original is never written)")
    ap.add_argument("--serial", action="store_true", help="attach xemu's SuperIO and check the debug serial log")
    ap.add_argument("--encoder", choices=sorted(ENCODERS), help="present this video encoder (xemu -machine "
                    "video-encoder=...) and check that the kernel finds and reports it")
    a = ap.parse_args()

    work = tempfile.mkdtemp(prefix="xk-boot-")
    sock = os.path.join(work, "qmp.sock")
    cfg = os.path.join(work, "xemu.toml")
    hdd_copy = os.path.join(work, "hdd.qcow2")
    dvd = os.path.join(work, "test.iso")
    if not a.no_disc:
        if a.udf:
            mkudf.build(dvd)
        else:
            mkxiso.build(dvd, open(a.title, "rb").read() if a.title else None, a.relaunch)
    if a.hdd:
        shutil.copyfile(a.hdd, hdd_copy)
    open(cfg, "w").write(
        "[general]\nshow_welcome = false\nskip_boot_anim = false\n\n[sys.files]\n"
        f"bootrom_path = '{os.path.abspath(a.mcpx_rom) if a.mcpx_rom else ''}'\n"
        f"flashrom_path = '{os.path.abspath(a.bios)}'\n"
        f"eeprom_path = '{os.path.join(work, 'eeprom.bin')}'\n"
        + (f"hdd_path = '{hdd_copy}'\n" if a.hdd else "")
        + ("" if a.no_disc else f"dvd_path = '{dvd}'\n"))
    log = open(os.path.join(work, "xemu.log"), "w")
    serial_log = os.path.join(work, "serial.log")
    serial_args = ["-device", "lpc47m157", "-serial", f"file:{serial_log}"] if a.serial else []
    if a.encoder:
        serial_args += ["-machine", f"video-encoder={a.encoder}"]
    proc = subprocess.Popen([a.xemu, "-config_path", cfg, "-qmp", f"unix:{sock},server,nowait"] + serial_args,
                            stdout=log, stderr=subprocess.STDOUT)
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
        elif a.dash:
            results = dash_checks(mon)
        elif a.relaunch:
            results = {"M5d-2": title_checks(mon, a.title_map, True)}
        else:
            results = run_checks(mon, a.hdd, a.title_map)
        if a.serial:
            results["serial"] = serial_checks(serial_log)
        ok = True
        for group, checks in results.items():
            for name, passed in checks.items():
                print(f"  [{'PASS' if passed else 'FAIL'}] {group}: {name}")
                ok &= passed
    finally:
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
    if ok and a.hdd and not a.dash and not a.relaunch and not a.udf and not a.encoder:
        for name, passed in {**disk_checks(hdd_copy), **lba48_disk_checks(Qcow2(hdd_copy), Qcow2(a.hdd))}.items():
            print(f"  [{'PASS' if passed else 'FAIL'}] disk: {name}")
            ok &= passed
    if os.path.exists(hdd_copy):
        os.remove(hdd_copy)  # 1 GB; logs are kept on failure, the disk copy is not
    print("RESULT:", "PASS" if ok else f"FAIL (logs in {work})")
    if ok:
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
