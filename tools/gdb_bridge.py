#!/usr/bin/env python3
"""gdb to OXOS's GDB stub (a GDB=1 kernel's own, or oxos_gdb.xos on any kernel) over the console's serial port; the log is printed here.

usage: gdb_bridge.py --tcp HOST:PORT [--port 2159] [--out FILE]       xemu's serial port as a TCP server (-serial tcp:HOST:PORT,server=on,wait=off)
       gdb_bridge.py --serial DEV [--port 2159] [--out FILE]          a real UART (a USB serial adapter on a debug kit's or a SuperIO's COM1)
The Modxo rig does the same inside tools/modxo/console.py (log --gdb PORT), which owns that port.

gdb connects to localhost:PORT (`target remote localhost:2159`). The serial line carries the kernel's log as text and the stub's RSP packets; this splits them:
packets go to gdb, `O` packets (the log while gdb waits for the target) are printed as log lines and also passed to gdb, the rest is log text. gdb's bytes go
to the console only once the stub has said "gdb: stub ready" since the kernel last started (oxos_gdb.xos says it at every launch); until then they wait
here, and a gdb that connects to a console without the debugger is told so and let go after 15 s instead of waiting for ever.
"""
import argparse
import select
import socket
import sys
import time

READY_TEXT = "gdb: stub ready"   # the stub's line once it reads the serial port
BANNER_TEXT = "OXOS kernel ("    # every boot's first line: the stub is not there yet
NO_STUB_WAIT = 15                # seconds a newly connected gdb waits for a stub before the bridge closes its connection


def checksum_ok(packet):
    """packet: b'$...#xx'."""
    body, cs = packet[1:-3], packet[-2:]
    try:
        return sum(body) & 0xFF == int(cs, 16)
    except ValueError:
        return False


class RspSplitter:
    """Splits the serial stream into log text and RSP packets (with their acks)."""

    def __init__(self, on_line, on_packet, on_ack):
        self.on_line = on_line      # a complete log line (str), from text or an O packet
        self.on_packet = on_packet  # a whole packet b'$...#xx' that is not an O packet, or an O packet (passed on too)
        self.on_ack = on_ack        # b'+' or b'-'
        self.text = bytearray()     # the log line being collected
        self.packet = None          # the packet being collected (bytearray), or None
        self.last_text = time.time()

    def feed(self, data):
        for b in data:
            if self.packet is not None:
                self.packet.append(b)
                if b in (0x0A, 0x0D) or len(self.packet) > 9000:  # not a packet after all: text
                    self.text += self.packet
                    self.packet = None
                    if b == 0x0A:
                        self._line()
                elif len(self.packet) >= 4 and self.packet[-3] == ord("#"):
                    self._packet(bytes(self.packet))
                    self.packet = None
            elif b == ord("$"):
                self.packet = bytearray(b"$")
            elif b in (ord("+"), ord("-")) and not self.text:
                self.on_ack(bytes([b]))
            elif b == 0x0A:
                self._line()
            else:
                self.text.append(b)
                self.last_text = time.time()

    def flush_partial(self, after=0.5):
        """A line without its newline, after `after` seconds without text."""
        if self.text and time.time() - self.last_text > after:
            self.on_line(self.text.decode("latin-1").rstrip("\r") + "  [partial line]")
            self.text.clear()

    def _line(self):
        self.on_line(self.text.decode("latin-1").rstrip("\r"))
        self.text.clear()

    def _packet(self, packet):
        if not checksum_ok(packet):
            self.on_line("[gdb] bad packet from the console: %r" % packet[:60])
            return
        body = packet[1:-3]
        if body[:1] == b"O" and len(body) > 1:
            try:
                for line in bytes.fromhex(body[1:].decode()).decode("latin-1").splitlines():
                    self.on_line(line)
            except ValueError:
                pass
        self.on_packet(packet)


class GdbServer:
    """The TCP side: one gdb at a time; its bytes go to the serial port while the stub is ready."""

    def __init__(self, port, write_serial, emit):
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", port))
        self.listener.listen(1)
        self.listener.setblocking(False)
        self.client = None
        self.write_serial = write_serial
        self.emit = emit
        self.ready = False          # the stub has said it reads the serial port since the kernel last started
        self.held = bytearray()     # gdb's bytes waiting for the stub
        self.no_stub_deadline = None  # a gdb connected while no stub was there: its connection is closed at this time unless one comes
        emit("[gdb] waiting for gdb on localhost:%d" % port)

    def sockets(self):
        return [self.listener] + ([self.client] if self.client else [])

    def poll(self, readable):
        if self.listener in readable:
            client, _ = self.listener.accept()
            if self.client:
                client.close()  # one at a time
            else:
                self.client = client
                self.client.setblocking(False)
                # No Nagle: an ack and the reply after it are two small writes, and gdb's delayed ACK would hold the reply back 40 ms
                self.client.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                self.emit("[gdb] gdb connected")
                if not self.ready:
                    self.emit("[gdb] the debugger has not started on the console since it was switched on: waiting %d s for it" % NO_STUB_WAIT)
                    self.no_stub_deadline = time.time() + NO_STUB_WAIT
        if self.client and self.client in readable:
            try:
                data = self.client.recv(65536)
            except (BlockingIOError, InterruptedError):
                data = None
            except OSError:
                data = b""
            if data == b"":
                self.emit("[gdb] gdb disconnected: detaching the stub")
                self.client.close()
                self.client = None
                self.held.clear()
                if self.ready:  # a gdb that went away without detaching leaves its breakpoints in: detach for it
                    self.write_serial(b"$D#44")
            elif data:
                self.held += data
        if self.ready:
            self.no_stub_deadline = None
        elif self.client and self.no_stub_deadline and time.time() > self.no_stub_deadline:
            # A console without the debugger (neither a GDB=1 kernel nor oxos_gdb.xos): gdb would wait for ever on an attach that cannot work
            self.emit("[gdb] the console is not running the debugger: closing gdb's connection (start it first: tools/debug_console.py, or a "
                      "Console (Run) configuration)")
            self.client.close()
            self.client = None
            self.held.clear()
            self.no_stub_deadline = None
        if self.ready and self.held:
            self.write_serial(bytes(self.held))
            self.held.clear()

    def to_gdb(self, data):
        if self.client:
            try:
                self.client.sendall(data)
            except OSError:
                pass
        elif data.startswith(b"$") and self.ready:
            self.write_serial(b"+")  # no gdb: ack for it, so the stub does not wait to send it again

    def on_line(self, line):
        """Every log line: the kernel starting over, and the stub becoming ready."""
        if BANNER_TEXT in line:
            self.ready = False
        if READY_TEXT in line:
            self.ready = True

    def console_off(self):
        self.ready = False


def stamp():
    t = time.time()
    return time.strftime("%H:%M:%S", time.localtime(t)) + ".%03d" % int((t % 1) * 1000)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--tcp", help="HOST:PORT of a serial port served over TCP (xemu)")
    src.add_argument("--serial", help="a serial device (115200 8N1)")
    ap.add_argument("--port", type=int, default=2159, help="the TCP port gdb connects to (default %(default)s)")
    ap.add_argument("--out", help="also append the log lines to this file")
    ap.add_argument("--ready", action="store_true", help="the stub is already running (it started before this bridge)")
    args = ap.parse_args()
    out = open(args.out, "a", buffering=1) if args.out else None

    def emit(line):
        line = stamp() + " " + line
        print(line, flush=True)
        if out:
            out.write(line + "\n")

    if args.tcp:
        host, port = args.tcp.rsplit(":", 1)
        deadline = time.time() + 60
        while True:
            try:
                link = socket.create_connection((host, int(port)), timeout=5)
                break
            except OSError:
                if time.time() > deadline:
                    sys.exit("no serial port at %s" % args.tcp)
                time.sleep(0.5)
        link.setblocking(False)
        link.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        read_link = lambda: link.recv(65536)
        write_link = link.sendall
        link_fd = link
    else:
        import serial
        link = serial.Serial(args.serial, 115200, timeout=0)
        read_link = lambda: link.read(65536)
        write_link = link.write
        link_fd = link.fileno()

    server = GdbServer(args.port, write_link, emit)
    server.ready = args.ready

    def on_line(line):
        server.on_line(line)
        emit("uart " + line)

    splitter = RspSplitter(on_line, server.to_gdb, server.to_gdb)
    while True:
        readable, _, _ = select.select([link_fd] + server.sockets(), [], [], 0.1)
        if link_fd in readable:
            try:
                data = read_link()
            except (BlockingIOError, InterruptedError):
                data = None
            if data == b"":
                emit("[gdb] the serial link closed")
                return 1
            if data:
                splitter.feed(data)
        server.poll(readable)
        splitter.flush_partial()


if __name__ == "__main__":
    sys.exit(main())
