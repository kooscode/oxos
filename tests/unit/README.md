# Core runtime unit tests

GoogleTest tests for the kernel's core runtime (memory manager, dispatcher, object manager, executive objects, RTL),
built from the kernel's own sources as a 32-bit Linux program. How it works: `spec/oxos-internal/11-host-unit-tests.md`.

## Run

    tools/unit-tests.sh                          # build and run every test
    tools/unit-tests.sh -- --gtest_filter='Pool.*'
    tools/unit-tests.sh --coverage               # + llvm-cov report: tests/unit/build-cov/coverage.txt, coverage-html/
    OXOS_TEST_LOG=1 tools/unit-tests.sh          # also print the kernel log (XK_SERIAL) as it happens
    OXOS_STRESS_SECONDS=120 tools/unit-tests.sh -- --gtest_filter='Stress.*'   # longer runs under async interrupts
    OXOS_KERNEL_OPT=-O2 tools/unit-tests.sh      # kernel sources at the console build's optimisation

Needs either a local 32-bit toolchain (clang, `g++-multilib`, `googletest` sources in `/usr/src/googletest`, make)
or Docker: without the toolchain the script builds the image from `Dockerfile` here and runs inside it. The build is
the plain `Makefile` in this folder (`make -C tests/unit`; `COVERAGE=1`, `KERNEL_OPT=-O2` and `BUILD=dir` as the
script passes them); it compiles GoogleTest from those sources itself.

## Layout

| File | |
|---|---|
| `host/hostcpu.cpp` | fake CPU (privileged instructions, interrupt flag, interrupts, time, devices) and MMU |
| `host/host_asm.S` | context switch and thread start-up (ELF versions of `kernel/src/arch/swap.S`) |
| `host/boot.cpp` | the 2BL's state, kernel start-up, GoogleTest on a kernel thread |
| `host/stubs.cpp` | the few symbols outside the files under test |
| `host/checks.cpp` | page-database checker, thread spawn/join helpers |
| `kernel_test.h` | shared helpers: `CheckPfnDatabase`, `EXPECT_BUGCHECK`, `Spawn`/`Join`, `SleepMs` |
| `test_*.cpp` | the tests |
| `Makefile` | the build: the kernel sources into `liboxos_core.a`, the harness and tests, GoogleTest from its sources |
| `host/kernel_text.ld` | link script: `liboxos_core.a`'s code in a section of its own (asynchronous interrupts land only there) |

## Writing tests

- Tests run on a kernel thread at PASSIVE_LEVEL (priority 8) and may block, create threads (`Spawn`), raise IRQL and
  queue DPCs/APCs like kernel code.
- Time only moves when the CPU idles (every thread blocked) or a test calls `host::AdvanceTime(ms)`; interrupts are
  raised with `host::RaiseIrq(n)` and delivered as soon as the IRQL allows.
- Leave the kernel as you found it: free pages, pool, handles; run down queues the test thread used.
- A kernel page fault, an unhandled bug check or a deadlock ends the run with a report naming the test.
