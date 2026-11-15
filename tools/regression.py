#!/usr/bin/env python3
"""The full regression run: the host unit tests, then every xemu test phase, with one summary (tests/README.md, "Regression run").

usage: regression.py [--mcpx-rom ROM] [--pages N] [--only PHASE,...]

`make -C kernel regression` runs it. Each phase's output goes to build/regression/<phase>.log, and the summary (result, checks and time per phase) is printed
and saved as build/regression/summary.txt. Every phase runs, also after a failure. Exit status 1 when a phase fails, or when a title is given fewer free
pages than --pages (default 15960: the stock kernel's, on a 64 MB console).

  --mcpx-rom ROM   your MCPX boot ROM dump, for test_mcpx (skipped without one; never committed)
  --pages N        the free pages test_memory must report, at least
  --only A,B       run only these phases (names as in the summary; "unit_tests" for the host unit tests)
"""
import argparse, os, re, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build", "regression")

# The xemu phases, in the order they run: `make test` also runs test_dash, test_relaunch, test_fatal and test_udf, so they are not listed again.
XEMU_PHASES = ["test", "test_mcpx", "test_exfat", "test_lba48", "test_extensions", "test_format", "test_upload", "test_iso", "test_encoders",
               "test_controller", "test_memory"]


# Started from `make regression`, the environment carries make's command-line variables (MAKEFLAGS): MCPX_ROM would reach every phase and boot them all
# through the MCPX ROM instead of OXOS's own reset code. Each phase gets only what this script passes it.
MAKE_VARIABLES = ("MAKEFLAGS", "MFLAGS", "MAKEOVERRIDES", "MAKELEVEL", "MCPX_ROM", "ONLY")


def run_phase(name, command, log_path):
    """Run one phase; return (exit status, seconds, its output)."""
    start = time.monotonic()
    env = {key: value for key, value in os.environ.items() if key not in MAKE_VARIABLES}
    with open(log_path, "w") as log:
        proc = subprocess.run(command, cwd=ROOT, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
        log.write(proc.stdout)
    return proc.returncode, time.monotonic() - start, proc.stdout


def check_unit_tests(output):
    """The unit tests' own counts: (passed, failed)."""
    passed = re.search(r"\[  PASSED  \] (\d+) tests?", output)
    failed = re.search(r"\[  FAILED  \] (\d+) tests?", output)
    return int(passed.group(1)) if passed else 0, int(failed.group(1)) if failed else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--mcpx-rom", help="MCPX boot ROM dump for test_mcpx")
    parser.add_argument("--pages", type=int, default=15960, help="free pages test_memory must report, at least")
    parser.add_argument("--only", help="comma-separated phases to run")
    args = parser.parse_args()
    only = set(args.only.split(",")) if args.only else None
    os.makedirs(OUT, exist_ok=True)

    phases = [("unit_tests", [os.path.join(ROOT, "tools", "unit_tests.sh")])]
    for name in XEMU_PHASES:
        command = ["make", "-C", os.path.join(ROOT, "kernel"), name]
        if name == "test_mcpx":
            if not args.mcpx_rom:
                phases.append((name, None))
                continue
            command.append(f"MCPX_ROM={os.path.abspath(args.mcpx_rom)}")
        phases.append((name, command))

    rows, failed_any, total_start = [], False, time.monotonic()
    for name, command in phases:
        if only and name not in only:
            continue
        if command is None:
            rows.append((name, "skipped", "", 0.0, "no MCPX ROM (MCPX_ROM=...)"))
            print(f"{name}: skipped, no MCPX ROM", flush=True)
            continue
        print(f"{name}: running", flush=True)
        status, seconds, output = run_phase(name, command, os.path.join(OUT, f"{name}.log"))
        note = ""
        if name == "unit_tests":
            passed, failed = check_unit_tests(output)
            checks = f"{passed} tests"
            ok = status == 0 and passed > 0 and failed == 0
        else:
            passed, failed = output.count("[PASS]"), output.count("[FAIL]")
            checks = f"{passed} checks"
            ok = status == 0 and failed == 0 and "RESULT: FAIL" not in output
            if name == "test_memory":
                pages = [int(p) for p in re.findall(r"(\d+) available \(", output)]
                note = f"free pages {min(pages) if pages else 'not found'} (need {args.pages})"
                ok = ok and bool(pages) and min(pages) >= args.pages
        if failed:
            checks += f", {failed} failed"
        result = "PASS" if ok else "FAIL"
        failed_any |= not ok
        rows.append((name, result, checks, seconds, note))
        print(f"{name}: {result} ({checks}, {seconds:.0f} s){' ' + note if note else ''}", flush=True)

    total = time.monotonic() - total_start
    lines = [f"OXOS regression run, {time.strftime('%Y-%m-%d %H:%M')}, {total / 60:.1f} min: {'FAIL' if failed_any else 'PASS'}",
             f"{'phase':<17} {'result':<8} {'checks':<22} {'time':>7}  note"]
    for name, result, checks, seconds, note in rows:
        lines.append(f"{name:<17} {result:<8} {checks:<22} {seconds:>6.0f}s  {note}")
    lines.append(f"logs: {OUT}")
    summary = "\n".join(lines) + "\n"
    with open(os.path.join(OUT, "summary.txt"), "w") as f:
        f.write(summary)
    print("\n" + summary, end="")
    sys.exit(1 if failed_any else 0)


if __name__ == "__main__":
    main()
