# OXOS kernel extension SDK

Builds kernel extensions (`.xos` files) for the OXOS kernel, in C or C++
([spec/oxos_internal/34](../spec/oxos_internal/34_kernel_extensions.md)). MIT licence: an extension built with it may
use any licence; open source is preferred.

**Needs:** LLVM's `clang` and `lld-link` (18 or newer) and GNU make, on Windows, macOS or Linux. Not nxdk.

- Linux: `apt install clang lld make` (or your distribution's packages).
- macOS: `brew install llvm lld`, and the `make` of the Xcode command-line tools.
- Windows: LLVM from its installer, and `make` (`scoop install make` or `choco install make`).

**An extension** (copy [`examples/hello`](examples/hello) to start):

```make
NAME := hello
SOURCES := hello.c
include path/to/oxos_sdk/oxos_extension.mk
```

`make` builds `hello.xos` (add `LLVM=C:/LLVM/bin` when clang is not on the `PATH`). Copy it to
`C:\OXOS\extensions\` on the console and list it in `C:\OXOS\oxos_extensions.ini`:

```ini
[hello]
extension = hello.xos
```

| Folder | Contents |
|---|---|
| `include/oxos/` | the headers; an extension includes `<oxos/oxos_extension.h>` |
| `lib/` | prebuilt: `oxos_runtime.lib` (the SDK's helpers) and `xboxkrnl.lib` (the kernel's functions) |
| `src/` | the runtime's source; `make` in this folder rebuilds `lib/` |
| `examples/hello/` | the example to start from: every part of an extension, commented ([README](examples/hello/README.md)) |
