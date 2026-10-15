#!/usr/bin/env python3
"""Boot a BIOS image in xemu and check milestone conditions through the QMP monitor.

The kernel's boot self-tests (kernel/src/ke/selftest.cpp) leave results in globals that are
located through the linker map and read with HMP `x`.

usage: boot_test.py --bios build/bios.bin --mcpx-rom mcpx.bin --map build/xboxkrnl.map [--xemu PATH]
"""
import argparse, json, os, re, shutil, socket, struct, subprocess, sys, tempfile, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qcow2 import Qcow2  # noqa: E402
from fatx import Fatx  # noqa: E402
import mkxiso  # noqa: E402


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
        "KiTestMmContiguousOk", "KiTestMmAlignedOk", "KiTestMmReuseOk", "KiTestMmSystemOk", "KiTestMmIoSpaceOk",
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
    }


def idex_checks(mon, hdd):
    if hdd is None:
        return {"HDD image attached": False}
    q = Qcow2(hdd)
    starts = [0, 0x55F400, 0x465400, 0x400, 0x177400, 0x2EE400]
    sigs_ok = all(
        [mon.read_offset("KiTestIdexPartitionSig", 16 * i + 4 * w) for w in range(4)]
        == list(struct.unpack("<4I", q.read(lba * 512, 16)))
        for i, lba in enumerate(starts))
    data = struct.unpack("<65536I", q.read(0x55F400 * 512, 0x40000))
    want = sum(v * (i | 1) for i, v in enumerate(data)) & 0xFFFFFFFF
    return {
        "disk identified: 8 GB LBA28 capacity": mon.read("IdexDiskLengthInSectors") == q.size // 512,
        "\\Device\\Harddisk0\\PartitionN on demand, names checked": mon.read("KiTestIdexNamesOk") == 1,
        "sector 0 of partitions 0-5 matches the image": sigs_ok,
        "256 KB DMA read (2 chunks) matches the image": mon.read("KiTestIdexBigOk") == 1 and mon.read("KiTestIdexBigSum") == want,
        "write + read back (page-crossing pool buffer)": mon.read("KiTestIdexWriteOk") == 1,
        "IOCTL partition info + drive geometry": mon.read("KiTestIdexIoctlOk") == 1,
        "alignment / bounds errors": mon.read("KiTestIdexErrorsOk") == 1,
        "IDE suite finished": mon.read("KiTestIdexDone") == 1,
    }


def read_string(mon, name, length):
    words = [mon.read_offset(name, i) for i in range(0, length, 4)]
    raw = b"".join(struct.pack("<I", w) for w in words)
    return raw.split(b"\0")[0].decode("latin-1")


def fatx_checks(mon, hdd):
    if hdd is None:
        return {"HDD image attached": False}
    q = Qcow2(hdd)
    c, e = Fatx(q, "C"), Fatx(q, "E")
    dash = c.read_file("xboxdash.xbe")
    s = 0
    for b in dash:
        s = (s * 31 + b) & 0xFFFFFFFF
    names = "".join(n + "|" for n, *_ in e.listdir())
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
            and mon.read("KiTestFatxTotalClusters") == e.clusters and mon.read("KiTestFatxSerial") == e.serial,
        "free clusters balanced": mon.read("KiTestFatxFreeBalanced") == 1,
        "FATX suite finished": mon.read("KiTestFatxDone") == 1,
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


def title_checks(mon, title_map):
    if not title_map:
        return {"test title built": False}
    syms = map_symbols(title_map)
    base = syms["_XkTitleResults"]
    for _ in range(40):  # the title starts after the kernel suites
        if mon.read("XeTitleLaunched"):
            break
        time.sleep(0.25)
    for _ in range(40):
        if mon.hmp(f"x/1wx {base:#x}").split(":")[1].split()[0] == "0x54544b58":
            break
        time.sleep(0.25)
    words = [int(mon.hmp(f"x/1wx {base + 4 * i:#x}").split(":")[1].split()[0], 16) for i in range(9 + 16)]
    name = b"".join(struct.pack("<I", w) for w in words[9:]).split(b"\0")[0].decode("latin-1")
    return {
        "title launched from \\Device\\CdRom0\\default.xbe": mon.read("XeTitleLaunched") == 1 and words[1] == 1,
        "XeImageFileName": name == "\\Device\\CdRom0\\default.xbe",
        "kernel thunks resolved: KeTickCount (data) + sleep": words[2] == 1,
        "pool, disc file read, system thread from the title": words[3] == 1 and words[4] == 1 and words[5] == 1,
        "path symbolic link \\??\\T: -> Partition1\\XkTest": words[6] == 1,
        "XeLoadSection / XeUnloadSection (digest-checked)": words[7] == 1,
        "XcSHA known answer": words[8] == 1,
        "title finished": words[0] == 0x54544B58,
    }


def rtl_checks(mon):
    for _ in range(80):
        if mon.read("KiTestRtlDone"):
            break
        time.sleep(0.25)
    names = ["Sprintf", "Interlocked", "CritSec", "RwLock", "Strings", "Math", "Refurb", "Time", "Dbg", "Boost", "Fpu"]
    out = {f"Rtl {n}": mon.read(f"KiTestRtl{n}Ok") == 1 for n in names}
    out["Rtl suite finished"] = mon.read("KiTestRtlDone") == 1
    return out


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
    }}


def disk_checks(hdd_copy):
    """After xemu exits: what the kernel wrote must read back with an independent FATX reader."""
    e = Fatx(Qcow2(hdd_copy), "E")
    names = sorted(n for n, *_ in e.listdir(e.lookup("XkTest")[2])) if e.lookup("XkTest") else None
    return {
        "E:\\XkTest\\keep.bin readable by an independent FATX reader": e.read_file("XkTest\\keep.bin")
            == b"open xbox kernel fatx test",
        "deleted/renamed entries gone, directory consistent": names == ["keep.bin"],
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bios", required=True)
    ap.add_argument("--mcpx-rom", required=True)
    ap.add_argument("--map", required=True)
    ap.add_argument("--xemu", default=shutil.which("xemu") or os.path.expanduser("~/.local/bin/xemu"))
    ap.add_argument("--boot-wait", type=float, default=8.0)
    ap.add_argument("--no-disc", action="store_true")
    ap.add_argument("--dash", action="store_true", help="check the dashboard phase instead of the self-tests")
    ap.add_argument("--title", help="test title XBE placed on the disc as default.xbe")
    ap.add_argument("--title-map")
    ap.add_argument("--hdd", help="qcow2 HDD image; a temporary copy is attached (the original is never written)")
    a = ap.parse_args()

    work = tempfile.mkdtemp(prefix="xk-boot-")
    sock = os.path.join(work, "qmp.sock")
    cfg = os.path.join(work, "xemu.toml")
    hdd_copy = os.path.join(work, "hdd.qcow2")
    dvd = os.path.join(work, "test.iso")
    if not a.no_disc:
        mkxiso.build(dvd, open(a.title, "rb").read() if a.title else None)
    if a.hdd:
        shutil.copyfile(a.hdd, hdd_copy)
    open(cfg, "w").write(
        "[general]\nshow_welcome = false\nskip_boot_anim = true\n\n[sys.files]\n"
        f"bootrom_path = '{os.path.abspath(a.mcpx_rom)}'\n"
        f"flashrom_path = '{os.path.abspath(a.bios)}'\n"
        f"eeprom_path = '{os.path.join(work, 'eeprom.bin')}'\n"
        + (f"hdd_path = '{hdd_copy}'\n" if a.hdd else "")
        + ("" if a.no_disc else f"dvd_path = '{dvd}'\n"))
    log = open(os.path.join(work, "xemu.log"), "w")
    proc = subprocess.Popen([a.xemu, "-config_path", cfg, "-qmp", f"unix:{sock},server,nowait"],
                            stdout=log, stderr=subprocess.STDOUT)
    ok = False
    try:
        for _ in range(100):
            if os.path.exists(sock):
                break
            time.sleep(0.1)
        time.sleep(a.boot_wait)
        mon = Monitor(sock, map_symbols(a.map))
        results = dash_checks(mon) if a.dash else run_checks(mon, a.hdd, a.title_map)
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
    if ok and a.hdd and not a.dash:
        for name, passed in disk_checks(hdd_copy).items():
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
