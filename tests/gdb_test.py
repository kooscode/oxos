#!/usr/bin/env python3
r"""GDB stub test (spec/oxos_internal/51): drives the stub over xemu's serial port with the gdb remote protocol, as gdb does (no gdb needed), with the gdb
test title (tests/gdb_test_title) on a disc. Two ways in:
  - a GDB=1 GDB_WAIT=1 kernel, whose own stub waits for gdb at the boot (make test_gdb);
  - with --extension: a release kernel with oxos_gdb.xos enabled on the disk (C:\OXOS\oxos_extensions.ini, and its oxos_gdb.ini with wait_for_gdb = yes),
    which waits for gdb at the title's start (make test_gdb_extension).

Checks, in order (the boot stop's checks 1-3 only with a GDB_WAIT=1 kernel; the extension's start replaces them):
  1. the boot stop: the kernel waits for gdb ('?' answers SIGTRAP in the stub's WaitForDebugger, called by InitGdbStub); registers ('g'), kernel memory
     ('m'), qSupported; the title's low addresses still map the kernel's own pages there
  2. a title breakpoint set at that stop (`monitor title CdRom0`, then Z0 at StepGdbTest) waits for the title: the kernel page it maps now is left alone
     (an int3 put there once hung the console's boot)
  3. Ctrl-C: the running kernel stops (SIGINT)
  1x. (extension) the title's start: the extension announces the wait on the serial port and '?' answers SIGTRAP; registers, kernel memory, qSupported;
      Ctrl-C while the title starts stops it (SIGINT); then the title breakpoint (Z0 at StepGdbTest) goes in at once (the title is there)
  4. the title breakpoint is hit once the title runs; the float registers ('p', 'P') from the thread's save area (no float instruction yet)
  5. the thread list has the idle thread and the title's; a function called from the debugger (gdb `call`: arguments below the stack pointer, esp
     moved) returns its value and the title goes on; a single step moves eip
  5a. memory search on the console (qSearch:memory, gdb's `find`) finds the XBE header at the title's base and the counter's value at its address,
      and nothing in a range without it; the library list (qXfer:libraries:read) names the kernel (oxoskrnl.elf; with --extension also oxos_gdb.xos), and the title once `monitor symbols
      main.exe` names its file, at its first section; watchpoints in pieces: an unaligned one takes three debug registers, one too long for the four is
      refused
  5b. vRun with no path starts the title again: the stop reply comes at once, and after 'c' a library stop (T05 library:) as the console comes back with
      the title; then the title breakpoint, set again, is hit in the new launch (gdb follows the console from one title into the next)
  6. a write watchpoint (Z2 on GdbTestCounter) stops with its address
  7. the title's access violation stops as SIGSEGV in main; its DbgPrint lines came as O packets while it ran; the float registers from the processor
  8. after detaching, the bug check ends in the stub: '?' answers SIGABRT in WaitAfterBugCheck (attach after a crash)
  9. the kernel's code is unchanged at the end: KeInsertQueueDpc (and, with a GDB_WAIT=1 kernel, the kernel page under the title's breakpoint address)

usage: gdb_test.py --bios BIOS --map oxoskrnl.map --title DIR --hdd HDD [--extension oxos_gdb.xos] [--bridge] [--mcpx-rom MCPX] [--xemu PATH]
DIR is the gdb test title's build folder: bin/default.xbe and main.exe (linked with -debug:dwarf, which keeps a symbol table). --bridge goes through
tools/gdb_bridge.py, as gdb on the Modxo rig does (console.py log --gdb): the bridge passes gdb's bytes on only once the stub has said it is ready.
"""
import argparse, os, re, shutil, socket, subprocess, sys, tempfile, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import mkxiso  # noqa: E402
from qcow2 import Qcow2  # noqa: E402
from fatx import FATXWriter  # noqa: E402
import gdb_bridge  # noqa: E402  (RspSplitter: the serial stream's log text and packets)

KERNEL_BASE = 0x80000000  # the kernel's image is at 0x80010000 and up; at boot the low addresses map the same pages


def map_symbol(path, name):
    """A symbol's address from the kernel's linker map (the decorated name as the map lists it; a name ending in '*' matches any name it starts, for
    functions in an anonymous namespace)."""
    for line in open(path):
        fields = line.split()
        if len(fields) >= 3 and (fields[1] == name or (name.endswith("*") and fields[1].startswith(name[:-1]))):
            return int(fields[2], 16)
    sys.exit(f"  [FAIL] {name} is not in {path}\nRESULT: FAIL")


def title_symbols(exe, work):
    """The title's C symbols (name without the leading underscore -> address), from main.exe's symbol table (objcopy to ELF, then nm)."""
    elf = os.path.join(work, "main.elf")
    subprocess.run(["objcopy", "-O", "elf32-i386", exe, elf], check=True, timeout=60)
    out = subprocess.run(["nm", elf], capture_output=True, text=True, check=True, timeout=60).stdout
    symbols = {}
    for line in out.splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[2].startswith("_"):
            symbols[fields[2][1:]] = int(fields[0], 16)
    return symbols


def first_section(exe):
    """The lowest section address of a PE file (where gdb relocates it from)."""
    out = subprocess.run(["objdump", "-h", exe], capture_output=True, text=True, check=True, timeout=60).stdout
    return min(int(f[3], 16) for f in (line.split() for line in out.splitlines()) if len(f) >= 7 and f[0].isdigit())


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Stub:
    """gdb's side of the remote protocol over xemu's serial port (a TCP server): log lines and O packets are collected, other packets are replies."""

    def __init__(self, port, log_file=None):
        deadline = time.time() + 30
        while True:
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.2)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.lines = []    # the kernel's log lines, from text and from O packets
        self.log_file = log_file  # through gdb_bridge.py: the bridge's log (it keeps the serial port's text lines to itself)
        self.replies = []  # packet bodies from the stub, other than O packets
        self.splitter = gdb_bridge.RspSplitter(self.lines.append, self._packet, lambda ack: None)

    def _packet(self, packet):
        self.sock.sendall(b"+")
        body = packet[1:-3]
        if body[:1] == b"O" and len(body) > 1 and re.fullmatch(rb"([0-9a-f]{2})+", body[1:]):
            return  # console output (RspSplitter gave its lines to self.lines)
        self.replies.append(body)

    def read(self, seconds):
        """Takes what the serial port sends within `seconds`."""
        self.sock.settimeout(seconds)
        try:
            data = self.sock.recv(65536)
        except socket.timeout:
            return
        if not data:
            raise ConnectionError("xemu closed the serial port")
        self.splitter.feed(data)

    def send(self, body):
        self.sock.sendall(b"$" + body + b"#%02x" % (sum(body) & 0xFF))

    def reply(self, timeout):
        deadline = time.time() + timeout
        while not self.replies:
            if time.time() > deadline:
                raise TimeoutError("no reply from the stub within %d s" % timeout)
            self.read(0.2)
        return self.replies.pop(0)

    def command(self, body, timeout=10):
        self.send(body)
        return self.reply(timeout)

    def bridge_lines(self):
        try:
            return open(self.log_file, errors="replace").read().splitlines() if self.log_file else []
        except OSError:
            return []

    def wait_line(self, text, timeout):
        deadline = time.time() + timeout
        while not any(text in line for line in self.lines + self.bridge_lines()):
            if time.time() > deadline:
                raise TimeoutError("no %r line within %d s" % (text, timeout))
            self.read(0.2)

    def memory(self, address, length):
        data = self.command(b"m%x,%x" % (address, length))
        return bytes.fromhex(data.decode()) if re.fullmatch(rb"([0-9a-f]{2})+", data) else None


def stop_eip(reply):
    """eip (register 08) from a stop reply's expedited registers."""
    m = re.search(rb"(?:^T..|;)08:([0-9a-f]{8})", reply)
    return int.from_bytes(bytes.fromhex(m.group(1).decode()), "little") if m else None


def continue_to_stop(stub, timeout):
    """'c', and the next stop that is not a library stop (gdb goes on after one by itself)."""
    stub.send(b"c")
    stop = stub.reply(timeout)
    while b"library:" in stop:
        stub.send(b"c")
        stop = stub.reply(timeout)
    return stop


def read_libraries(stub):
    """The library list's XML, read in pieces as gdb reads it."""
    data, offset = b"", 0
    while True:
        piece = stub.command(b"qXfer:libraries:read::%x,200" % offset)
        if not piece or piece[:1] not in (b"m", b"l"):
            return data + piece
        data += piece[1:]
        offset += len(piece) - 1
        if piece[:1] == b"l":
            return data


def float_checks(stub, where, report):
    """The float registers through 'p' and 'P' (gdb's i386 numbers: 0x27 xmm7, 0x28 mxcsr): mxcsr with every exception masked, and xmm7 written, read
    back and restored."""
    mxcsr = stub.command(b"p28")
    ok = re.fullmatch(rb"[0-9a-f]{8}", mxcsr) is not None and int.from_bytes(bytes.fromhex(mxcsr.decode()), "little") & 0x1F80 == 0x1F80
    report(f"{where}: mxcsr ('p')", ok, mxcsr.decode(errors="replace"))
    original = stub.command(b"p27")
    pattern = bytes(range(0x10, 0x20)).hex().encode()
    wrote = stub.command(b"P27=" + pattern)
    back = stub.command(b"p27")
    restored = stub.command(b"P27=" + original) if re.fullmatch(rb"[0-9a-f]{32}", original) else b"(not read)"
    report(f"{where}: xmm7 written and read back ('P', 'p')", wrote == b"OK" and back == pattern and restored == b"OK",
           f"{original!r} {wrote!r} {back!r} {restored!r}")


def boot_stop_checks(stub, kernel_map, step, report):
    """Checks 1-3 (a GDB_WAIT=1 kernel): the boot stop, a title breakpoint set there, Ctrl-C while the kernel boots. The kernel's code there (KeInsertQueueDpc
    and the page under the title's breakpoint address), for check 9."""
    init_stub = map_symbol(kernel_map, "?WaitForDebugger@gdb_stub@oxos@@YAXPBD@Z")
    insert_dpc = map_symbol(kernel_map, "_KeInsertQueueDpc@12")
    alias = KERNEL_BASE + step  # the kernel page the title's breakpoint address maps at boot
    # 1. The boot stop.
    stub.wait_line("gdb: waiting for gdb", 60)
    stop = stub.command(b"?")
    eip = stop_eip(stop)
    report("boot stop: SIGTRAP in WaitForDebugger", stop.startswith(b"T05") and eip is not None and init_stub <= eip < init_stub + 0x400,
           f"{stop[:40]!r}, eip {eip and hex(eip)}")
    supported = stub.command(b"qSupported:swbreak+;hwbreak+")
    report("qSupported", b"PacketSize=" in supported, supported.decode(errors="replace"))
    registers = stub.command(b"g")
    report("registers ('g')", len(registers) >= 128 and int.from_bytes(bytes.fromhex(registers[64:72].decode()), "little") == eip,
           f"{len(registers)} hex digits")
    dpc_code = stub.memory(insert_dpc, 64)
    alias_code = stub.memory(alias, 16)
    report("kernel memory ('m')", dpc_code is not None and alias_code is not None and len(dpc_code) == 64)
    # At this stop the low addresses still map the kernel's pages: what gdb reads at the title's address is the kernel's code. A breakpoint gdb sets
    # now must wait for the title, or its int3 goes into the kernel.
    low_code = stub.memory(step, 16)
    report("at the boot the title's addresses map the kernel (what the next checks guard)", low_code is not None and low_code == alias_code,
           "not mapped" if low_code is None else "other bytes")

    # 2. A title breakpoint set at the boot stop, before the title is there, as gdb does when it connects.
    answer = stub.command(b"qRcmd," + b"title CdRom0".hex().encode())
    report("monitor title CdRom0", answer == b"OK", answer.decode(errors="replace"))
    answer = stub.command(b"Z0,%x,1" % step)
    report("title breakpoint set at the boot (Z0, pending)", answer == b"OK", answer.decode(errors="replace"))
    report("kernel page under the title's breakpoint address left alone", stub.memory(alias, 16) == alias_code)

    # 3. Ctrl-C while the kernel boots.
    stub.send(b"c")
    stub.read(1.0)
    stub.sock.sendall(b"\x03")
    stop = stub.reply(5)
    report("Ctrl-C: SIGINT", stop.startswith(b"T02"), stop[:40].decode(errors="replace"))
    return {insert_dpc: dpc_code, alias: alias_code}


def extension_start_checks(stub, kernel_map, step, report):
    """Check 1x (oxos_gdb.xos with wait_for_gdb = yes): the title's start, Ctrl-C while the title runs, the title breakpoint set. The kernel's code
    (KeInsertQueueDpc), for check 9."""
    insert_dpc = map_symbol(kernel_map, "_KeInsertQueueDpc@12")
    stub.wait_line("gdb: waiting for gdb at the title's start", 90)
    stop = stub.command(b"?")
    report("the title's start: SIGTRAP", stop.startswith(b"T05") and stop_eip(stop) is not None, stop[:40].decode(errors="replace"))
    supported = stub.command(b"qSupported:swbreak+;hwbreak+")
    report("qSupported", b"PacketSize=" in supported, supported.decode(errors="replace"))
    registers = stub.command(b"g")
    report("registers ('g')", len(registers) >= 128 and int.from_bytes(bytes.fromhex(registers[64:72].decode()), "little") == stop_eip(stop),
           f"{len(registers)} hex digits")
    dpc_code = stub.memory(insert_dpc, 64)
    report("kernel memory ('m')", dpc_code is not None and len(dpc_code) == 64)
    title_code = stub.memory(step, 16)
    report("the title's memory ('m')", title_code is not None and len(title_code) == 16)

    # Ctrl-C while the title starts: well before its fourth round (100 ms apart), whose access violation check 7 wants.
    stub.send(b"c")
    stub.read(0.05)
    stub.sock.sendall(b"\x03")
    stop = stub.reply(5)
    report("Ctrl-C: SIGINT", stop.startswith(b"T02"), stop[:40].decode(errors="replace"))
    answer = stub.command(b"Z0,%x,1" % step)
    report("title breakpoint set (Z0)", answer == b"OK", answer.decode(errors="replace"))
    return {insert_dpc: dpc_code}


def write_extension(hdd, extension):
    """oxos_gdb.xos on the test disk, enabled in C:\\OXOS\\oxos_extensions.ini, with its oxos_gdb.ini: the title from the disc waits for gdb at its start."""
    q = Qcow2(hdd)
    w = FATXWriter(q, "C")
    w.makedirs("OXOS\\extensions")
    w.write_file("OXOS\\oxos_extensions.ini", b"[oxos_gdb]\r\nextension = oxos_gdb.xos\r\nenabled = yes\r\nlogging_enabled = yes\r\n")
    w.write_file("OXOS\\extensions\\oxos_gdb.xos", open(extension, "rb").read())
    w.write_file("OXOS\\extensions\\oxos_gdb.ini", b"wait_for_gdb = yes\r\ntitle = CdRom0\r\n")
    q.save(hdd + ".new")
    os.replace(hdd + ".new", hdd)


def run(a, work, report):
    kernel_map = a.map
    wait_after = None if a.extension else map_symbol(kernel_map, "?WaitAfterBugCheck@?A0x*")
    title = title_symbols(os.path.join(a.title, "main.exe"), work)
    step, counter, main_at = title["StepGdbTest"], title["GdbTestCounter"], title["main"]
    text_start = first_section(os.path.join(a.title, "main.exe"))

    iso, cfg, hdd = (os.path.join(work, n) for n in ("test.iso", "xemu.toml", "hdd.qcow2"))
    mkxiso.write_image(iso, {"default.xbe": open(os.path.join(a.title, "bin", "default.xbe"), "rb").read()})
    shutil.copyfile(a.hdd, hdd)
    if a.extension:
        write_extension(hdd, a.extension)
    open(cfg, "w").write(
        "[general]\nshow_welcome = false\nskip_boot_anim = true\n\n"
        "[sys]\nmem_limit = '64'\n\n[sys.files]\n"
        f"bootrom_path = '{os.path.abspath(a.mcpx_rom) if a.mcpx_rom else ''}'\nflashrom_path = '{os.path.abspath(a.bios)}'\n"
        f"eeprom_path = '{os.path.join(work, 'eeprom.bin')}'\ndvd_path = '{iso}'\nhdd_path = '{hdd}'\n")
    port = free_port()
    log = open(os.path.join(work, "xemu.log"), "w")
    proc = subprocess.Popen([a.xemu, "-config_path", cfg, "-device", "lpc47m157",
                             "-serial", f"tcp:127.0.0.1:{port},server=on,wait=on,nodelay=on"], stdout=log, stderr=subprocess.STDOUT)
    stub = bridge = None
    try:
        if a.bridge:  # gdb's side as on the Modxo rig: gdb_bridge.py between gdb and the serial port
            gdb_port, bridge_log = free_port(), os.path.join(work, "bridge.log")
            bridge = subprocess.Popen([sys.executable, "-I", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools", "gdb_bridge.py"),
                                       "--tcp", f"127.0.0.1:{port}", "--port", str(gdb_port), "--out", bridge_log],
                                      stdout=subprocess.DEVNULL, stderr=open(os.path.join(work, "bridge.err"), "w"))
            stub = Stub(gdb_port, bridge_log)
        else:
            stub = Stub(port)
        kernel_code = (extension_start_checks if a.extension else boot_stop_checks)(stub, kernel_map, step, report)

        # 4. The title breakpoint, once the title runs.
        stop = continue_to_stop(stub, 150)
        eip = stop_eip(stop)
        report("title breakpoint hit (StepGdbTest)", stop.startswith(b"T05") and b"swbreak" in stop and eip == step,
               f"{stop[:60]!r}, eip {eip and hex(eip)}")

        float_checks(stub, "float registers before the title's first float instruction (save area)", report)

        # 5. Threads, a call, a single step.
        threads = stub.command(b"qfThreadInfo")
        count = len(threads[1:].split(b",")) if threads.startswith(b"m") else 0
        report("thread list (qfThreadInfo)", count >= 2, f"{count} threads")
        stub.command(b"z0,%x,1" % step)

        # A function called from the debugger, as gdb `call` does it: a return address and an argument below the stack pointer, esp and eip moved, a
        # breakpoint at the return address (main's start: it does not run again), then every register put back.
        saved = stub.command(b"g")
        esp = int.from_bytes(bytes.fromhex(saved[32:40].decode()), "little")
        frame = (esp - 0x40) & ~15
        wrote = stub.command(b"M%x,8:%s" % (frame, (main_at.to_bytes(4, "little") + (100).to_bytes(4, "little")).hex().encode()))
        stub.command(b"Z0,%x,1" % main_at)
        moved = stub.command(b"P4=%s" % frame.to_bytes(4, "little").hex().encode()) + stub.command(b"P8=%s" % step.to_bytes(4, "little").hex().encode())
        stub.send(b"c")
        stop = stub.reply(10)
        eax = stub.command(b"p0")
        counted = stub.memory(counter, 4)
        stub.command(b"z0,%x,1" % main_at)
        put_back = stub.command(b"G" + saved)
        report("a call from the debugger (StepGdbTest(100) returns 200)",
               wrote == b"OK" and moved == b"OKOK" and stop_eip(stop) == main_at and eax == (200).to_bytes(4, "little").hex().encode()
               and counted == (100).to_bytes(4, "little") and put_back == b"OK", f"{wrote!r} {moved!r} {stop[:40]!r} eax {eax!r} counter {counted!r}")
        stop = stub.command(b"s")
        eip = stop_eip(stop)
        report("single step", stop.startswith(b"T05") and eip is not None and eip != step, f"eip {eip and hex(eip)}")

        # 5a. Memory search on the console (qSearch:memory, gdb's `find`): the XBE header's magic at the title's base, the counter's value (100) at its
        # address, and a range without it; patterns are escaped as gdb sends them.
        def search(start, length, pattern):
            escaped = b"".join(bytes([0x7D, c ^ 0x20]) if c in b"#$}*" else bytes([c]) for c in pattern)
            return stub.command(b"qSearch:memory:%x;%x;" % (start, length) + escaped)
        header, value, missing = search(0x10000, 0x1000, b"XBEH"), search(counter, 0x40, (100).to_bytes(4, "little")), \
            search(counter + 4, 0x100, (100).to_bytes(4, "little"))
        report("memory search (qSearch:memory)", header == b"1,10000" and value == b"1,%x" % counter and missing == b"0",
               f"{header!r} {value!r} {missing!r}")

        # The library list, and watchpoints in pieces.
        libraries = read_libraries(stub)
        wanted = [b'name="oxoskrnl.elf"><segment address="0x80010000"'] + ([b'name="oxos_gdb.xos"'] if a.extension else [])
        report("library list (qXfer:libraries:read)", all(w in libraries for w in wanted), libraries[:200].decode(errors="replace"))
        answer = stub.command(b"qRcmd," + b"symbols main.exe".hex().encode())
        libraries = read_libraries(stub)
        report("monitor symbols main.exe: the title in the library list at its first section", answer == b"OK" and
               b'name="main.exe"><segment address="0x%x"' % text_start in libraries, libraries[-160:].decode(errors="replace"))
        stub.command(b"qRcmd," + b"symbols".hex().encode())
        pieces = stub.command(b"Z2,%x,4" % (counter + 1))
        too_long = stub.command(b"Z2,%x,10" % (counter + 1))
        removed = stub.command(b"z2,%x,4" % (counter + 1))
        report("watchpoints in pieces (unaligned: three registers; 16 bytes unaligned: refused)", pieces == b"OK" and too_long.startswith(b"E")
               and removed == b"OK", f"{pieces!r} {too_long!r} {removed!r}")

        # 5b. Start the title again (vRun, no path): the console comes back with it, a library stop, and the breakpoint in the new launch.
        stub.command(b"Z0,%x,1" % step)
        answer = stub.command(b"vRun;")
        report("vRun: the stop reply", answer.startswith(b"T05"), answer[:40].decode(errors="replace"))
        stub.send(b"c")
        stop = stub.reply(120)
        report("the title started again: a library stop", stop.startswith(b"T05") and b"library:" in stop, stop[:60].decode(errors="replace"))
        stub.send(b"c")
        stop = stub.reply(60)
        eip = stop_eip(stop)
        report("the title breakpoint hit in the new launch", stop.startswith(b"T05") and b"swbreak" in stop and eip == step,
               f"{stop[:60]!r}, eip {eip and hex(eip)}")
        stub.command(b"z0,%x,1" % step)

        # 6. A write watchpoint.
        answer = stub.command(b"Z2,%x,4" % counter)
        report("write watchpoint set (Z2)", answer == b"OK", answer.decode(errors="replace"))
        stub.send(b"c")
        stop = stub.reply(30)
        watched = re.search(rb"watch:([0-9a-f]+)", stop)
        report("write watchpoint hit", stop.startswith(b"T05") and watched is not None and int(watched.group(1), 16) == counter,
               stop[:80].decode(errors="replace"))
        stub.command(b"z2,%x,4" % counter)

        # 7. The access violation, and the title's output.
        stub.send(b"c")
        stop = stub.reply(60)
        eip = stop_eip(stop)
        report("access violation: SIGSEGV in main", stop.startswith(b"T0b") and eip is not None and main_at <= eip < main_at + 0x200,
               f"{stop[:40]!r}, eip {eip and hex(eip)}")
        report("title's DbgPrint as O packets", any("gdb test: round 2 result 6" in line for line in stub.lines))  # the new launch's rounds
        float_checks(stub, "float registers of the FPU's owner (processor)", report)

        # 8. Detach: the bug check ends in the stub, which answers a debugger that comes afterwards (a release kernel prints no bug-check line: wait).
        answer = stub.command(b"D")
        report("detach", answer == b"OK", answer.decode(errors="replace"))
        if a.extension:
            stub.read(5.0)
        else:
            stub.wait_line("*** bug check", 30)
        stub.read(2.0)
        stop = stub.command(b"?")
        eip = stop_eip(stop)
        in_stub = True if a.extension else eip is not None and wait_after <= eip < wait_after + 0x40  # the extension's addresses are not in the map
        report("after the bug check: SIGABRT in WaitAfterBugCheck", stop.startswith(b"T06") and in_stub, f"{stop[:40]!r}, eip {eip and hex(eip)}")

        # 9. The kernel's code, as at the first stop.
        for address, code in kernel_code.items():
            report(f"kernel code at {address:#x} unchanged", stub.memory(address, len(code)) == code)
    except (TimeoutError, ConnectionError, OSError) as e:
        report("the session went on to the end", False, str(e))
    finally:
        if bridge:
            bridge.terminate()
            try:
                bridge.wait(5)
            except subprocess.TimeoutExpired:
                bridge.kill()
        proc.terminate()
        try:
            proc.wait(5)
        except subprocess.TimeoutExpired:
            proc.kill()
        log.close()
        if stub:
            open(os.path.join(work, "serial.log"), "w").write("\n".join(stub.lines) + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--bios", required=True, help="a GDB=1 GDB_WAIT=1 BIOS image (build/kernel/gdb_wait/bios.bin), or a release one with --extension")
    ap.add_argument("--map", required=True, help="its kernel's linker map (oxoskrnl.map)")
    ap.add_argument("--title", required=True, help="the gdb test title's build folder (build/tests/gdb_test_title_debug)")
    ap.add_argument("--hdd", required=True, help="a hard disk image with a dashboard (copied; without one the kernel stops at its no-disk error)")
    ap.add_argument("--extension", help="oxos_gdb.xos: put it on the disk, enabled, and debug through it (a release kernel)")
    ap.add_argument("--bridge", action="store_true", help="through tools/gdb_bridge.py, as gdb on the Modxo rig")
    ap.add_argument("--mcpx-rom")
    ap.add_argument("--xemu", default=shutil.which("xemu") or os.path.expanduser("~/.local/bin/xemu"))
    a = ap.parse_args()
    work = tempfile.mkdtemp(prefix="xk-gdb-")
    failures = []

    def report(name, ok, detail=""):
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}" + (f" ({detail})" if detail and not ok else ""), flush=True)
        if not ok:
            failures.append(name)

    print(f"gdb stub test: {os.path.basename(a.bios)}", flush=True)
    start = time.time()
    run(a, work, report)
    if failures:
        print(f"RESULT: FAIL (logs in {work})")
        sys.exit(1)
    shutil.rmtree(work, ignore_errors=True)
    print(f"RESULT: PASS ({time.time() - start:.0f} s)")


if __name__ == "__main__":
    main()
