#!/usr/bin/env python3
"""Debug the kernel or an nxdk app on a real console at source level: the Modxo rig, gdb over the Modxo's serial port (spec/oxos_internal/51).

usage: debug_console.py [APP] [--release] [--console SERIAL] [--mcpx16] [--name NAME] [--to ADDRESS]
       debug_console.py --stop | --check [--console SERIAL]
       debug_console.py --remove [--console SERIAL] [--mcpx16] [--to ADDRESS]

APP is the app's folder, built with `make DEBUG=y` (main.exe and bin/); without APP the console boots its dashboard (the kernel alone). Two ways:
  - a GDB=1 kernel (the default): build/kernel/gdb_wait (GDB=1 GDB_WAIT=1 DEBUG=1) by netboot, with APP/bin as the title; the kernel waits for gdb at boot.
  - --release: the release kernel by netboot with oxos_gdb.xos, the debugger as a kernel extension, as a developer with a stock OXOS has it. The first time
    (and whenever oxos_gdb.xos changed) the console boots the OXOS FTP server once: C:\\OXOS\\oxos_extensions.ini is saved here and then gets
    [oxos_gdb] enabled = yes, and oxos_gdb.xos and its oxos_gdb.ini (wait_for_gdb = yes, title = NAME) go to C:\\OXOS\\extensions\\. The title waits for
    gdb at its start. --remove boots the FTP server again and puts the saved oxos_extensions.ini back (oxos_gdb.xos and its ini are deleted).
The console: --console SERIAL (the Modxo's Pico, as `tools/modxo/console.py` lists them), else $OXOS_CONSOLE, else the only one connected. --mcpx16 (or
$OXOS_CONSOLE_MCPX16=1): a 1.6 console, whose kernels are their own build (MCPX16=1). Only one console may be on at a time (they share the HDMI capture):
the others must be off. Needs the console's logger running with its gdb bridge:
    python3 -I tools/modxo/console.py log SERIAL --gdb 2159
Steps: build; APP/.oxos/main.elf (main.exe's debug info in the form gdb reads best) and OUT/console.gdb (what gdb runs once connected: with APP,
`monitor title NAME`, so the title's breakpoints stay out of other XBEs such as the boot video player); serve by netboot; power-cycle the console; wait
for the debugger ("gdb: stub ready"); print "OXOS debug: ready": gdb attaches to localhost:2159. OUT is APP/.oxos, or build/debug_console for the kernel
alone. --stop switches the console off and stops the netboot server. --check only says whether the debugger runs on the console (no power cycle).
"""
import argparse, ftplib, hashlib, io, json, os, re, signal, socket, subprocess, sys, time

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
sys.path.insert(0, os.path.join(root, "tools", "modxo"))
import console  # noqa: E402  (the Modxo rig: the Picos and their loggers)

kernel = os.path.join(root, "kernel")
CONSOLE = [sys.executable, "-I", os.path.join(root, "tools/modxo/console.py")]
NETBOOT_PORT = 5141
STATE_DIR = os.path.join(root, "build", "debug_console")
FTP_UP_WITHIN = 45  # seconds from power-on: a console whose network card did not come up (the 1.6's sometimes does not) is power-cycled again
FTP_TRIES = 4


def say(text):
    print(f"OXOS debug: {text}", flush=True)


def fail(text):
    say(f"error: {text}")
    sys.exit(1)


def ask_logger(port, command, timeout=2.0):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    s.sendto(command, ("127.0.0.1", port))
    try:
        return s.recv(2048).decode("latin-1")
    except socket.timeout:
        return None


def pick_console(serial):
    """The console's Pico serial number: --console, $OXOS_CONSOLE, or the only one connected."""
    serial = serial or os.environ.get("OXOS_CONSOLE", "")
    connected = console.picos()
    if serial:
        if serial not in connected:
            fail(f"no Modxo with serial number {serial} is connected (tools/modxo/console.py lists them)")
        return serial
    if len(connected) != 1:
        fail("several Modxos are connected: name one with --console SERIAL or $OXOS_CONSOLE (tools/modxo/console.py lists them)" if connected
             else "no Modxo is connected")
    return next(iter(connected))


def logger_port(serial):
    port = console.loggers().get(serial)
    if not port:
        fail(f"no logger for {serial}: run `python3 -I tools/modxo/console.py log {serial} --gdb 2159`")
    return port


def check_others_off(serial):
    """Only one console on at a time: they share the HDMI capture card, which shows the one switched on last."""
    for other, port in console.loggers().items():
        if other and other != serial and console.state(port) == "on":
            fail(f"the console of Modxo {other} is on: switch it off first (only one console at a time)")


def switch(serial, want):
    return subprocess.run(CONSOLE + [want, serial], timeout=90).returncode == 0


def lan_broadcast(console_ip="192.168.1.222"):
    """The broadcast address of the PC's network that reaches the console's netboot address (kernel/Makefile NETBOOT_IP): with several networks
    (Docker, a VPN) 255.255.255.255 goes out the wrong one. None when `ip` cannot tell."""
    try:
        route = subprocess.run(["ip", "-o", "route", "get", console_ip], capture_output=True, text=True, timeout=5).stdout.split()
        dev = route[route.index("dev") + 1]
        addr = subprocess.run(["ip", "-o", "-4", "addr", "show", "dev", dev], capture_output=True, text=True, timeout=5).stdout.split()
        return addr[addr.index("brd") + 1]
    except (OSError, ValueError, IndexError, subprocess.SubprocessError):
        return None


def netboot_port_free():
    """No other netboot server (or MCPX dump receiver) on UDP 5141: two answering the console mix their replies and it boots from flash."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.bind(("", NETBOOT_PORT))  # without SO_REUSEADDR: fails while any socket holds the port
        return True
    except OSError:
        return False
    finally:
        s.close()


def stop_netboot(pid_file):
    try:
        pid = int(open(pid_file).read())
        os.kill(pid, signal.SIGTERM)
    except (OSError, ValueError):
        pass
    try:
        os.remove(pid_file)
    except OSError:
        pass


def serve(image, title, pid_file, log, to):
    """netboot.py serving image (and title: [folder, name]) in the background, in place of any server this tool started before."""
    stop_netboot(pid_file)
    for _ in range(20):  # the stopped server's socket goes within moments
        if netboot_port_free():
            break
        time.sleep(0.1)
    else:
        fail(f"another program serves netboot on UDP {NETBOOT_PORT} (a netboot.py, an MCPX dump receiver): stop it first")
    extra = ["--title", title[0], "--title-name", title[1]] if title else []
    server = subprocess.Popen([sys.executable, "-I", os.path.join(root, "tools/netboot/netboot.py"), image, *extra, "--to", to],
                              stdout=open(log, "w"), stderr=subprocess.STDOUT, start_new_session=True)
    open(pid_file, "w").write(str(server.pid))


def build_kernel(build, *flags):
    """build/kernel/BUILD/bios.bin with the make flags given; its path."""
    target = f"../build/kernel/{build}/bios.bin"
    if subprocess.run(["make", "-s", "-C", kernel, f"BUILD=../build/kernel/{build}", "SELFTEST=0", *flags, target]).returncode:
        fail(f"building the kernel ({build}) failed")
    return os.path.join(kernel, target)


def wait_for_debugger(port, seconds):
    deadline = time.time() + seconds
    while ask_logger(port, b"G") != "ready":
        if time.time() > deadline:
            fail(f"the debugger did not start within {seconds} s (see the logger)")
        time.sleep(0.5)


# --- The release kernel's debugger (oxos_gdb.xos), installed over FTP

def ftp_session(serial, port, mcpx16, pid_file, out, to):
    """Boots the console into the OXOS FTP server (a SERIAL=1 kernel: its log has the server's address) and returns a logged-in ftplib.FTP."""
    image = build_kernel("debug_console_serial_1_6" if mcpx16 else "debug_console_serial", "SERIAL=1", *(["MCPX16=1"] if mcpx16 else []))
    if subprocess.run(["make", "-s", "-C", kernel, "oxos_ftp"]).returncode:
        fail("building the OXOS FTP server failed")
    serve(image, [os.path.join(root, "build/apps/oxos_ftp/bin"), "ftp"], pid_file, os.path.join(out, "netboot_ftp.log"), to)
    for attempt in range(1, FTP_TRIES + 1):
        switch(serial, "off")
        time.sleep(3)
        if not switch(serial, "on"):
            fail("the console did not switch on")
        started = time.time()
        while time.time() - started < FTP_UP_WITHIN:
            line = ask_logger(port, b"Fftp://") or ""
            stamp = line.split(" ", 1)[0]
            address = re.search(r"ftp://([0-9.]+)/", line)
            if address and stamp >= time.strftime("%H:%M:%S", time.localtime(started)):
                try:
                    ftp = ftplib.FTP(address.group(1), timeout=20)
                    ftp.login("xbox", "xbox")
                    return ftp
                except OSError:
                    pass
            time.sleep(1)
        say(f"the FTP server did not come up within {FTP_UP_WITHIN} s (try {attempt} of {FTP_TRIES}): power-cycling the console")
    fail("the console's FTP server never came up")


def read_file(ftp, path):
    data = io.BytesIO()
    ftp.retrbinary("RETR " + path, data.write)
    return data.getvalue()


def enable_section(ini, section, extension):
    """ini (oxos_extensions.ini's text) with [section] enabled = yes: the section added when it is not there."""
    lines, inside, done = ini.splitlines(), False, False
    newline = "\r\n" if "\r\n" in ini else "\n"
    for i, line in enumerate(lines):
        name = line.strip()
        if name.startswith("["):
            inside = name.lower() == f"[{section}]"
        elif inside and re.match(r"\s*enabled\s*=", line):
            lines[i] = "enabled = yes"
            done = True
    if not done:
        lines += ["", f"[{section}]", f"extension = {extension}", "enabled = yes"]
    return newline.join(lines) + newline


def install_extension(serial, port, mcpx16, name, pid_file, out, to):
    """oxos_gdb.xos and its ini on the console, enabled; the console's oxos_extensions.ini saved first (once: the saved one is the user's)."""
    xos = os.path.join(root, "build/kernel/extensions/oxos_gdb/oxos_gdb.xos")
    if subprocess.run(["make", "-s", "-C", kernel, "extensions"]).returncode:
        fail("building the extensions failed")
    record_path = os.path.join(STATE_DIR, f"console_{serial}.json")
    record = json.load(open(record_path)) if os.path.exists(record_path) else {}
    digest = hashlib.sha256(open(xos, "rb").read()).hexdigest()
    if record.get("xos") == digest and record.get("title") == name:
        return
    say("putting oxos_gdb.xos on the console (through the OXOS FTP server)")
    ftp = ftp_session(serial, port, mcpx16, pid_file, out, to)
    saved = os.path.join(STATE_DIR, f"console_{serial}_oxos_extensions.ini")
    ini = read_file(ftp, "/C/OXOS/oxos_extensions.ini").decode("latin-1")
    if not os.path.exists(saved):
        open(saved, "w", encoding="latin-1", newline="").write(ini)
    ftp.storbinary("STOR /C/OXOS/oxos_extensions.ini", io.BytesIO(enable_section(ini, "oxos_gdb", "oxos_gdb.xos").encode("latin-1")))
    ftp.storbinary("STOR /C/OXOS/extensions/oxos_gdb.xos", open(xos, "rb"))
    ftp.storbinary("STOR /C/OXOS/extensions/oxos_gdb.ini", io.BytesIO(f"wait_for_gdb = yes\r\ntitle = {name}\r\n".encode()))
    ftp.quit()
    switch(serial, "off")
    json.dump({"xos": digest, "title": name}, open(record_path, "w"))


def remove_extension(serial, port, mcpx16, pid_file, to):
    """The console's own oxos_extensions.ini back, oxos_gdb.xos and its ini deleted."""
    saved = os.path.join(STATE_DIR, f"console_{serial}_oxos_extensions.ini")
    if not os.path.exists(saved):
        fail("nothing to remove: this tool did not put oxos_gdb.xos on that console")
    ftp = ftp_session(serial, port, mcpx16, pid_file, STATE_DIR, to)
    original = open(saved, encoding="latin-1", newline="").read().encode("latin-1")
    ftp.storbinary("STOR /C/OXOS/oxos_extensions.ini", io.BytesIO(original))
    if read_file(ftp, "/C/OXOS/oxos_extensions.ini") != original:
        fail("oxos_extensions.ini did not read back as saved: it is still in " + saved)
    for file in ("oxos_gdb.xos", "oxos_gdb.ini"):
        try:
            ftp.delete("/C/OXOS/extensions/" + file)
        except ftplib.error_perm:
            pass
    ftp.quit()
    switch(serial, "off")
    stop_netboot(pid_file)
    os.remove(saved)
    try:
        os.remove(os.path.join(STATE_DIR, f"console_{serial}.json"))
    except OSError:
        pass
    say("removed: the console's own oxos_extensions.ini is back")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("app", nargs="?", help="the nxdk app's folder (none: the kernel alone, the console boots its dashboard)")
    ap.add_argument("--release", action="store_true", help="the release kernel with oxos_gdb.xos (installed over FTP the first time)")
    ap.add_argument("--console", help="the console's Modxo (its Pico's serial number; default $OXOS_CONSOLE, or the only one connected)")
    ap.add_argument("--mcpx16", action="store_true", default=os.environ.get("OXOS_CONSOLE_MCPX16") == "1", help="a 1.6 console (its own kernel build)")
    ap.add_argument("--name", help="the title's folder name on E: (default: the app folder's name)")
    ap.add_argument("--to", help="netboot: the broadcast address of the console's network (default: the PC's network that reaches 192.168.1.222)")
    ap.add_argument("--stop", action="store_true", help="switch the console off and stop serving")
    ap.add_argument("--check", action="store_true", help="only check that the debugger runs on the console (no power cycle)")
    ap.add_argument("--remove", action="store_true", help="--release's oxos_gdb.xos off the console again, its oxos_extensions.ini as it was")
    a = ap.parse_args()
    serial = pick_console(a.console)
    port = logger_port(serial)
    os.makedirs(STATE_DIR, exist_ok=True)
    pid_file = os.path.join(STATE_DIR, "netboot.pid")  # one for every session: one console at a time, one server
    to = a.to or lan_broadcast() or "255.255.255.255"
    if a.check:
        state = ask_logger(port, b"G")
        if state == "ready":
            say("ready: the debugger runs on the console; gdb on localhost:2159")
            return
        if state == "waiting":
            fail("the debugger has not started on the console since it was switched on: start it first (Terminal > Run Task > "
                 "\"oxos: start kernel on console\" or an app's \"start ... on the console\" task, or a Console (Run) configuration)")
        fail(f"no logger with a gdb bridge: run `python3 -I tools/modxo/console.py log {serial} --gdb 2159`")
    if a.stop:
        switch(serial, "off")
        stop_netboot(pid_file)
        return
    if ask_logger(port, b"G") not in ("ready", "waiting"):
        fail(f"no logger with a gdb bridge: run `python3 -I tools/modxo/console.py log {serial} --gdb 2159`")
    check_others_off(serial)
    if a.remove:
        remove_extension(serial, port, a.mcpx16, pid_file, to)
        return
    say("starting")
    app = os.path.abspath(a.app) if a.app else None
    out = os.path.join(app, ".oxos") if app else STATE_DIR
    os.makedirs(out, exist_ok=True)
    suffix = "_1_6" if a.mcpx16 else ""
    if a.release:
        image = build_kernel("release" + suffix, *(["MCPX16=1"] if a.mcpx16 else []))
        symbols = [os.path.join(os.path.dirname(image), "oxoskrnl.elf")]  # names only: a release kernel has no source lines
    else:
        image = build_kernel("gdb_wait" + suffix, "DEBUG=1", "GDB=1", "GDB_WAIT=1", *(["MCPX16=1"] if a.mcpx16 else []))
        symbols = [os.path.join(os.path.dirname(image), "oxoskrnl.elf")]
    title = None
    commands = "# the kernel alone: no title filter\n"
    name = None
    if app:
        bin_dir = os.path.join(app, "bin")
        if not os.path.isfile(os.path.join(bin_dir, "default.xbe")):
            fail(f"no {bin_dir}/default.xbe: build the app first (make DEBUG=y)")
        exe, elf = os.path.join(app, "main.exe"), os.path.join(out, "main.elf")
        if subprocess.run(["objcopy", "-O", "elf32-i386", exe, elf]).returncode:
            fail(f"converting {exe} for gdb failed")
        symbols.insert(0, elf)
        name = a.name or re.sub(r"[^A-Za-z0-9_-]", "_", os.path.basename(app))[:15]  # netboot: 1-15 characters
        title = [bin_dir, name]
        commands = f"monitor title {name}\n"
    elif a.release:
        fail("--release needs an APP: the extension debugs titles")
    if a.release:
        install_extension(serial, port, a.mcpx16, name, pid_file, out, to)
    # What gdb runs once connected (VS Code: postRemoteConnectCommands): the library list's files from this build (the kernel's oxoskrnl.elf, oxos_gdb.xos),
    # then the title filter.
    libraries = [os.path.dirname(image), os.path.join(root, "build", "kernel", "extensions", "oxos_gdb")]
    with open(os.path.join(out, "console.gdb"), "w") as f:
        f.write(f"set sysroot\nset solib-search-path {':'.join(libraries)}\nsharedlibrary\n" + commands)
    serve(image, title, pid_file, os.path.join(out, "netboot.log"), to)
    say("serving the " + ("release kernel" if a.release else "GDB kernel") + (f" and {title[0]} as E:\\{title[1]}" if title else "")
        + "; power-cycling the console")
    switch(serial, "off")
    time.sleep(3)
    if not switch(serial, "on"):
        fail("the console did not switch on")
    wait_for_debugger(port, 90)
    say(f"symbols: {' and '.join(symbols)}; once connected: source {os.path.join(out, 'console.gdb')}")
    say("ready: gdb on localhost:2159 (" + ("the title waits at its start)" if a.release else "the console waits at boot)"))


if __name__ == "__main__":
    main()
