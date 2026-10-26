#!/usr/bin/env python3
"""Boot the console N times through the Modxo rig and classify each boot (README.md in this folder).

Needs a running `console.py log --out LOG`: it owns the Pico's serial port and the power button. For netboot
kernels, also tools/netboot/netboot.py, serving SERVE (a file or symlink this tool can repoint with --image).

Each run: switch the console off (if on), on, then watch LOG's COM1 lines:
  PASS    the --pass text was printed (default "boot: title launcher", an OXOS kernel starting the dashboard)
  FREEZE  the kernel banner came, then no COM1 line for --stall seconds
  NOBOOT  no kernel banner ("OXOS kernel") within --boot seconds (BIOS, netboot or wiring trouble)
then switch it off. One line per run, with the kernel's last COM1 line, and a summary.

  bootrun.py --log LOG [--runs N] [--image IMG --serve SERVE] [--flags 0xNN --flags-file F] [--pass TEXT]
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import console  # noqa: E402  (the logger's command interface)


class Log:
    """The lines appended to the logger's file since this object was made."""

    def __init__(self, path):
        self.path = path
        self.pos = os.path.getsize(path)

    def new_lines(self):
        with open(self.path, "rb") as f:
            f.seek(self.pos)
            data = f.read()
        cut = data.rfind(b"\n") + 1
        self.pos += cut
        return data[:cut].decode("latin-1").splitlines()


def one_run(args):
    if not console.switch(args.port, "off"):
        return "NOPOWER", 0, "the console did not switch off"
    time.sleep(2)
    log = Log(args.log)
    if not console.switch(args.port, "on"):
        return "NOPOWER", 0, "the console did not switch on"
    t0 = time.time()
    started, last_t, last_uart, result = None, t0, "", None
    while result is None:
        time.sleep(0.5)
        for line in log.new_lines():
            parts = line.split(" ", 2)
            if len(parts) < 3 or parts[1] != "uart":
                continue
            text = parts[2]
            if args.banner in text:
                started = time.time()
            if started:
                if not text.startswith(("sample ", "t ")):  # trace and sampler lines say nothing about progress
                    last_uart = text
                last_t = time.time()
                if args.pass_text in text:
                    result = "PASS"
        now = time.time()
        if result is None and not started and now - t0 > args.boot:
            result = "NOBOOT"
        elif result is None and started and now - last_t > args.stall:
            result = "FREEZE"
    if result == "PASS":
        time.sleep(args.after)
    elapsed = (last_t - started) if started else 0
    console.switch(args.port, "off")
    return result, elapsed, last_uart


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--log", required=True, help="the file a running `console.py log --out` appends to")
    p.add_argument("--runs", type=int, default=1)
    p.add_argument("--port", type=int, default=console.PORT, help="the logger's command port")
    p.add_argument("--image", help="netboot: point SERVE at this image first")
    p.add_argument("--serve", help="netboot: the path netboot.py serves (a symlink)")
    p.add_argument("--flags", help="netboot: write this experiment-flags value to --flags-file first")
    p.add_argument("--flags-file", help="netboot: the file netboot.py --flags-file reads")
    p.add_argument("--banner", default="OXOS kernel", help="text that marks the kernel's start (default: %(default)s)")
    p.add_argument("--pass", dest="pass_text", default="boot: title launcher",
                   help="text that marks a good boot (default: %(default)s)")
    p.add_argument("--boot", type=float, default=45, help="seconds to the banner before NOBOOT (default %(default)s)")
    p.add_argument("--stall", type=float, default=10, help="seconds without a line before FREEZE (default %(default)s)")
    p.add_argument("--after", type=float, default=0, help="seconds to stay on after a PASS")
    args = p.parse_args()
    if console.state(args.port) is None:
        sys.exit("no logger answers on 127.0.0.1:%d (run: console.py log --out %s)" % (args.port, args.log))
    if args.image:
        if not args.serve:
            sys.exit("--image needs --serve")
        if not os.path.isfile(args.image):
            sys.exit("no such image: " + args.image)
        if os.path.lexists(args.serve):
            os.remove(args.serve)
        os.symlink(os.path.abspath(args.image), args.serve)
    if args.flags:
        if not args.flags_file:
            sys.exit("--flags needs --flags-file")
        with open(args.flags_file, "w") as f:
            f.write(args.flags + "\n")
    counts = {}
    for i in range(args.runs):
        result, elapsed, last = one_run(args)
        counts[result] = counts.get(result, 0) + 1
        print("%s run %d/%d: %-7s kernel+%5.1fs | %s" % (time.strftime("%H:%M:%S"), i + 1, args.runs, result,
                                                          elapsed, last[:110]), flush=True)
        if result == "NOPOWER":
            break
    print("summary:", " ".join("%s=%d" % kv for kv in sorted(counts.items())), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
