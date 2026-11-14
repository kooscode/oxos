# hello: an example kernel extension

`hello.cpp` is a complete OXOS kernel extension in about a hundred lines of commented C++. Start a new extension by
copying this folder. It shows each part an extension can have:

| Part | In `hello.cpp` | When it runs |
|---|---|---|
| `Load` | greets, counts launches in its state page, reads its settings, registers an event, hooks `NtOpenFile` | before each title starts; the title waits for it |
| `Main` | logs a heartbeat every `heartbeat` seconds | on its own thread while the title runs |
| `Unload` | logs how many files the title opened | when the title starts another title or the dashboard |
| a hook | counts the title's `NtOpenFile` calls and passes each one on to the kernel | at each call the title makes |
| an event | logs the title's name and ID | when a title has been loaded, before it runs |
| the state page | keeps the launch count from one title to the next | from the cold boot until the next one |

## Build

Needs LLVM's `clang` and `lld-link` (18 or newer) and GNU make, on Windows, macOS or Linux (see
[the SDK's README](../../README.md)):

```
make
```

This writes `hello.xos` here. Add `LLVM=C:/LLVM/bin` (the folder that holds `clang`) when it is not on the `PATH`.

## Install

1. Copy `hello.xos` to `C:\OXOS\extensions\` on the console's hard disk (the OXOS File Manager or FTP).
2. Copy `oxos_extensions.ini` to `C:\OXOS\oxos_extensions.ini`, or add its `[hello]` section to yours.
3. Start any title: the extension loads before it, and at every launch after it.

To switch it off, set `enabled = no` in its section, or hold **B** at boot (safe mode: no extensions until the next
cold boot).

## See what it does

Its messages go to the debug serial port of a kernel built with `SERIAL=1` (a debug kit, or a modchip's serial
port):

```
[hello] Hello from OXOS 0.4.7! Launch 1 since the cold boot.
[hello] Title FFFE0000 "Xbox Dashboard" is starting.
[hello] 10 s into this title, 42 files opened.
[hello] Goodbye: 57 files opened by this title.
[hello] Hello from OXOS 0.4.7! Launch 2 since the cold boot.
[hello] The last title opened 57 files.
```

## Things to know

- **Each launch starts afresh.** The kernel reloads every extension from the disk before each title, so global
  variables start from their initial values every time. Only the 4 KB state page carries over, until the next cold
  boot.
- **Hooks see the title's calls only.** `NextNtOpenFile` is the kernel's function, or the hook of an extension listed
  after this one in `oxos_extensions.ini`. The kernel's own calls are not hooked.
- **Hooks run on the title's threads**, possibly several at once: shared data needs atomic operations or a lock.
- **No floating point** in kernel code: the kernel does not save those registers for extensions (the SDK's build
  rules turn it off).
- The full rules are in the specification: `spec/oxos_internal/34_kernel_extensions.md` in the OXOS repository.
