#!/usr/bin/env python3
"""Dump the import tracer's ring: the kernel calls a running title made most recently (TRACE_IMPORTS builds).

usage: trace.py --qmp SOCK --map xboxkrnl.map [--exports exports.tsv] [--last N] [--only NAMES] [--hide NAMES]
                [--all] [--thread ID] [--save FILE | --load FILE]

Run the title with `make run-imports` (xemu with a QMP socket), then `make trace-dump` while it runs, or after it
crashed or hung. xemu is paused while the ring is copied (a fraction of a second) and then resumed.

Each line: sequence number, function, return value (DEADDEAD while the call has not returned, e.g. the thread is
blocked in it or crashed there), caller's return address, thread, the first three stack arguments, and:
- NtCreateFile/NtOpenFile: the end of the path, share access and create/open options;
- NtQueryFullAttributesFile, NtDeleteFile, IoCreate/DeleteSymbolicLink: the end of the name;
- NtReadFile/NtWriteFile: APC context, I/O status block, buffer, length, byte offset, the buffer's first dword;
- NtSetInformationFile: information class, length, the first four dwords of the information.
The kernel itself never records the most frequent calls (IRQL, critical sections, spin locks, NtYieldExecution, ...);
by default this tool also hides a list of other very common ones (waits, events, timers, pool), --all shows them.
"""
import argparse, json, os, re, socket, struct, sys, tempfile

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
RING, WORDS = 0x8000, 15  # kernel/src/xe/trace.cpp: XK_TRACE_RING entries of XK_TRACE_WORDS dwords
NOT_RETURNED = 0xDEADDEAD
HIDDEN = ("KeSetEvent KeResetEvent KeDelayExecutionThread KeWaitForSingleObject KeWaitForMultipleObjects "
          "NtWaitForSingleObjectEx NtWaitForMultipleObjectsEx NtSetEvent NtClearEvent NtUserIoApcDispatcher "
          "KeSetTimer KeSetTimerEx KeCancelTimer KeTestAlertThread KeQueryPerformanceFrequency "
          "KeStallExecutionProcessor KeSynchronizeExecution KeRemoveQueueDpc MmQueryAddressProtect "
          "MmSetAddressProtect MmGetPhysicalAddress MmQueryAllocationSize ExAllocatePoolWithTag ExFreePool "
          "NtQueryInformationFile").split()
NAMED = {190, 202, 210, 195, 67, 69}  # entries that carry a name in words 7-14


class Qmp:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX)
        try:
            self.s.connect(path)
        except OSError as e:
            sys.exit(f"cannot connect to xemu at {path} ({e.strerror}): start it with `make run-imports`")
        self.f = self.s.makefile("rw")
        self.f.readline()
        self.command("qmp_capabilities")

    def command(self, name, **args):
        self.f.write(json.dumps({"execute": name, "arguments": args}) + "\n")
        self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if "error" in r:
                sys.exit(f"xemu: {r['error'].get('desc', r['error'])}")
            if "return" in r:
                return r["return"]

    def hmp(self, line):
        return self.command("human-monitor-command", **{"command-line": line})

    def word(self, address):
        return int(self.hmp(f"x/1wx {address:#x}").split(":")[1].split()[0], 16)


def symbols(path):
    syms = {}
    for line in open(path):
        m = re.match(r"\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})", line)
        if m:
            syms[m.group(1)] = int(m.group(2), 16)
    return syms


def symbol(syms, name):
    for k in ("_" + name, name):
        if k in syms:
            return syms[k]
    sys.exit(f"{name} is not in the map: is this a TRACE_IMPORTS build?")


def grab(a):
    """The count and the raw ring, from the running xemu."""
    syms = symbols(a.map)
    q = Qmp(a.qmp)
    q.command("stop")
    try:
        count = q.word(symbol(syms, "XkTraceCount"))
        ring = q.word(symbol(syms, "XkTraceRing"))
        if not ring:
            sys.exit("the ring is not allocated yet: no title has been loaded")
        fd, tmp = tempfile.mkstemp(prefix="xk-ring-")
        os.close(fd)
        try:
            # Quoted: unquoted, the monitor reads the path's slashes as division.
            out = q.hmp(f'memsave {ring:#x} {RING * WORDS * 4} "{tmp}"').strip()
            if out:
                sys.exit(f"xemu memsave: {out}")
            data = open(tmp, "rb").read()
        finally:
            os.remove(tmp)
    finally:
        q.command("cont")
    if len(data) != RING * WORDS * 4:
        sys.exit(f"short read of the ring ({len(data)} bytes)")
    return count, data


def text(words):
    return b"".join(struct.pack("<I", w) for w in words).split(b"\0")[0].decode("latin-1")


def detail(e):
    ordinal = e[0]
    if e[7] >> 28 == 1:
        if ordinal in (219, 236):
            offset = "-" if e[12] == e[13] == 0xFFFFFFFF else f"{e[13] << 32 | e[12]:#x}"
            return f"apc {e[8]:#x} iosb {e[9]:#x} buf {e[10]:#x} len {e[11]:#x} at {offset} first {e[14]:#010x}"
        if ordinal == 226:
            return f"class {e[8]} len {e[9]:#x} info " + " ".join(f"{w:#x}" for w in e[10:14])
        return " ".join(f"{w:#x}" for w in e[8:15])
    if ordinal in (190, 202):
        return f"'{text(e[7:13])}' share {e[13]:#x} opts {e[14]:#x}"
    if ordinal in NAMED:
        return f"'{text(e[7:15])}'"
    return ""


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--qmp", help="xemu's QMP socket (make run-imports: kernel/build-imports2/qmp.sock)")
    ap.add_argument("--map", default=os.path.join(root, "kernel/build-imports2/xboxkrnl.map"))
    ap.add_argument("--exports", default=os.path.join(root, "kernel/exports.tsv"))
    ap.add_argument("--last", type=int, default=200, help="how many calls to print (0 = the whole ring)")
    ap.add_argument("--only", help="comma-separated function names to show")
    ap.add_argument("--hide", help="comma-separated function names to hide (in addition to the default list)")
    ap.add_argument("--all", action="store_true", help="do not hide the default list of common calls")
    ap.add_argument("--thread", type=lambda s: int(s, 16), help="only this thread (hex KTHREAD address)")
    ap.add_argument("--save", help="also write the raw ring to FILE (decode later with --load)")
    ap.add_argument("--load", help="decode a ring saved with --save instead of reading xemu")
    a = ap.parse_args()

    names = {}
    for line in open(a.exports).read().splitlines()[1:]:
        f = line.split("\t")
        names[int(f[0])] = f[1]

    if a.load:
        raw = open(a.load, "rb").read()
        count, data = struct.unpack_from("<I", raw)[0], raw[4:]
    else:
        if not a.qmp:
            sys.exit("--qmp SOCK (or --load FILE) is needed")
        count, data = grab(a)
        if a.save:
            open(a.save, "wb").write(struct.pack("<I", count) + data)
            print(f"ring saved to {a.save}")

    only = set(a.only.split(",")) if a.only else None
    hidden = set() if a.all or only else set(HIDDEN)
    if a.hide:
        hidden |= set(a.hide.split(","))
    first = max(0, count - RING)
    lines = []
    for seq in range(count - 1, first - 1, -1):  # newest first, until enough are found
        e = struct.unpack_from(f"<{WORDS}I", data, (seq % RING) * WORDS * 4)
        if not e[0]:
            continue  # cleared: from before a quick reboot
        name = names.get(e[0], f"#{e[0]}")
        if (only and name not in only) or name in hidden or (a.thread is not None and e[2] != a.thread):
            continue
        ret = "(no return)" if e[6] == NOT_RETURNED else f"{e[6]:#010x}"
        lines.append(f"{seq:7d} {name:30s} -> {ret:11s} from {e[1]:#010x} thr {e[2]:#010x} "
                     f"args {e[3]:#x} {e[4]:#x} {e[5]:#x}  {detail(e)}".rstrip())
        if a.last and len(lines) == a.last:
            break
    print(f"{count} calls recorded, the last {min(count, RING)} kept; showing {len(lines)}"
          + ("" if a.all or only else " (common calls hidden: --all shows them)"))
    print("\n".join(reversed(lines)))


if __name__ == "__main__":
    main()
