# 41. Homebrew on a real console: build, deploy and debug from any PC (proposal)

Status: **proposal**, not implemented (user, 2026-11-15: "full remote debugging - full NXDK support release that will allow nxdk remote debugging, etc. from
any OS (Mac, Linux, windows) - so home brew developers can build, deploy, debug all in one place"). Nothing here is decided until the user says so.

Today an nxdk developer can debug at source level only in xemu (`tools/debug_app.py`, VS Code through `tools/vscode/`), and copies a build to a console by
hand (the File Manager or the FTP server). On a real console there is no debugger: the README lists it under "Not yet". This proposal makes OXOS a development
console: from Linux, macOS or Windows, one command or one key in VS Code builds the app, sends it to the console, starts it, shows its log, and stops in the
debugger at a breakpoint, all over the network.

## 1. What the developer gets
| Step | How |
|---|---|
| **Build** | nxdk as today (`make`), on any of the three systems; the OXOS SDK already builds the same way there (sdk/README.md) |
| **Deploy and run** | `oxos run APP` sends the app's `bin/` to the console (`E:\Apps\<name>\` by default) and starts it; no FTP client, no File Manager |
| **Log** | the app's `DbgPrint` / `OutputDebugString` / `printf` to the debug output appear in the PC's terminal as they happen, with the kernel's own log lines |
| **Debug** | `oxos debug APP` (or F5 in VS Code) connects gdb or lldb: breakpoints, stepping, variables, call stacks, threads, memory, at source level |
| **Crash** | an exception in the app stops it in the debugger instead of the error screen (when a debugger is connected; spec 29's screen otherwise) |
| **More** | `oxos screenshot` (spec 40), `oxos reboot`, `oxos ls` / `get` / `put`, `oxos launch PATH` |

## 2. How it could work
- **On the console: a debug stub.** A small debugger that speaks the **GDB remote protocol** (documented by GDB; gdb and lldb both use it, so every system
  has a client) over TCP. It handles the processor's debug exceptions (int 1, int 3), sets software breakpoints (int 3) and the four hardware ones (DR0–DR3),
  reads and writes registers and memory, lists threads, and stops the title's threads while the debugger looks at them. OXOS writes its own; no Microsoft
  debug monitor (xbdm) is involved.
- **Where it lives:** a kernel extension, `oxos_debugger.xos`, loaded only on a development console (`oxos_extensions.ini`, or sent with netboot, spec 34
  §10). It needs from the kernel: the exceptions first (a hook before the error screen), stopping and resuming threads, and a connection to the PC (below).
- **On the PC: one tool, `oxos`,** in Python (standard library only, like `tools/fetch.py`), for Linux, macOS and Windows: deploy, run, log, screenshot, and
  starting gdb or lldb with the app's symbols (`tools/debug_app.py` already converts nxdk's debug information for gdb). VS Code gets a launch configuration that
  uses it (`tools/vscode/`), as it does for xemu today.
- **Deploy and log** use the same connection: a small command channel (file transfer, launch, reboot, log stream), replacing the FTP server for development.

## 3. The hard part: the connection while a title runs
OXOS's network stack runs only before the first title (spec 31): once a title starts, the network card is the title's. Choices, to settle first:
1. **The kernel keeps a network stack for the debugger** while the title runs, when the title does not use the network itself. nxdk apps that use the network
   (lwIP and nxdk's own network driver) would then conflict with it: the two would have to share the card, or the debugger would not be available to them.
2. **The serial port** (COM1): works for every title, but only on consoles with one (a debug kit, or a Modxo with its serial port, as on the OXOS test
   rig, tools/modxo); slow, but enough for the debugger and the log.
3. **USB** (a USB network adapter or a USB serial adapter on a controller port), owned by the debugger instead of the title: no conflict with the title's
   network, but a USB stack while the title runs.

A likely first step: the serial port for the debugger (it already carries the kernel's log on the Modxo rig), the network for deploy and run before the app
starts, then the kernel's network stack shared with titles that do not use the network.

## 4. Things to settle before building it
- The connection (§3), and what the debugger costs titles in memory (spec 18) when it is loaded, and nothing when it is not.
- Stopping a title: which threads stop at a breakpoint (all of the title's, as gdb expects), what keeps running (DPCs, the audio and video interrupts).
- Symbols: nxdk builds with `DEBUG=y`; how the tool finds them for an app on the console.
- Security: a debugger on the network can do anything to the console. It is off unless installed, and only answers on the local network; a password is open.
- Which gdb and lldb versions, and VS Code's extensions (C/C++, CodeLLDB), are tested on each system.

## 5. Tests
- xemu: the stub against gdb, scripted (`gdb -batch`): connect, a breakpoint in a test app, step, read a variable, continue, an exception caught; on all
  three host systems in CI where possible.
- Console: the same session on the Modxo rig, over the chosen connection.
