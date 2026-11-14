# OXOS kernel extension SDK

Builds kernel extensions (`.xos` files) for the OXOS kernel, in C or C++. MIT licence: an extension built with it may use any licence; open source is preferred.
The full rules: `spec/oxos_internal/34_kernel_extensions.md` in the OXOS repository.

**Needs:** LLVM's `clang` and `lld-link` (18 or newer) and GNU make, on Windows, macOS or Linux. Not nxdk.

- Linux: `apt install clang lld make` (or your distribution's packages).
- macOS: `brew install llvm lld`, and the `make` of the Xcode command-line tools.
- Windows: LLVM from its installer, and `make` (`scoop install make` or `choco install make`).

**An extension** (copy [`examples/hello`](examples/hello) to start):

```make
NAME := hello
SOURCES := hello.cpp
include path/to/the/sdk/oxos_extension.mk
```

`make` builds `hello.xos` (add `LLVM=C:/LLVM/bin` when clang is not on the `PATH`). Copy it to `C:\OXOS\extensions\` on the console and list it in
`C:\OXOS\oxos_extensions.ini`:

```ini
[hello]
extension = hello.xos
```

| In the SDK | Contents |
|---|---|
| `include/oxos_sdk.h` | the one header an extension includes, C or C++ |
| `lib/oxos_sdk.lib` | the one library: the kernel's functions (its import library). The SDK's helpers are inline in the header |
| `oxos_extension.mk` | the build rules (or use your own: the flags are in it) |
| `examples/hello/` | the example to start from: every part of an extension, commented ([README](examples/hello/README.md)) |

**The SDK's source** (`sdk/` in the OXOS repository) has the header in parts (`include/oxos/`), the kernel's export list the library is made from
(`src/xboxkrnl.def`), and a `Makefile` that builds the SDK as above into `build/sdk/oxos_sdk/` (and with `VERSION=X.Y.Z` its release zip), then each example
with that package into `build/sdk/examples/NAME/`. Extensions are only built with the package, never from the source, so what ships is what is tested.
