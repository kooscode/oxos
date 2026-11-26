# 50. Modern controllers in every title: a virtual USB controller, with controller drivers as a kernel extension (proposal)

Status: **proposal**, not implemented (user, 2026-11-26: "make this a kernel extension that works for ALL titles", then "update the relevant spec with this plan
to use kernel extensions as a xbox controller driver for xbox one and other controllers"). Nothing here is built; the steps of §8 start with a pilot in its
own worktree.

A wired Xbox One / Series controller (and later an Xbox 360 controller and other pads) plugged into an Xbox controller port, with a passive adapter cable, works
in **every title**, however it was built (the XDK in any version, nxdk, other homebrew), as an original Xbox controller. Real Xbox controllers, memory units
and headsets keep working next to it.

## 1. Why the title's own USB code is the obstacle
Every title carries its own USB stack: XDK titles link the XDK's USB and input libraries (section `XPP`: the host controller driver, the XID class driver,
`XInputGetState`, ...), nxdk titles nxdk's (`lib/usb/libusbohci` with its XID driver). Both drive the console's two OHCI host controllers directly and accept
only XID devices (class 0x58). A modern pad is another device class (GIP: vendor class 0xFF, subclass 0x47, protocol 0xD0), so no title sets it up, and the
kernel never sees a title's USB traffic. A pad that a title never opens cannot be reached by rewriting its reports either.

So OXOS gives every title a virtual USB controller: a kernel extension owns the real OHCI controllers, drives the modern pads itself, and shows the title
a controller with original Xbox pads on it.

## 2. How it works
The kernel leaves the OHCI register space unmapped, so each access by the title's USB driver page-faults and OXOS answers it as a fake controller; once that
controller is set up, a kernel extension's 1 ms timer serves the transfers between the title's driver and the fake controller.

| What the title does | How it reaches OXOS | What OXOS does |
|---|---|---|
| Reads or writes an OHCI register (0xFED00000 and 0xFED08000, 4 KB each) | page fault: the register page is not present in the title's view | decodes the instruction (`mov` to and from memory, and the read-modify-write forms `or`, `and`, `test` with a memory operand), reads or updates the virtual controller's register state, steps past the instruction. A few µs, never waits |
| Queues a setup transfer for a new device | its control list in RAM, walked by the 1 ms virtual-frame timer | answers as a Duke: device, configuration and XID descriptors, set address |
| Queues an interrupt-IN transfer (a report) | its periodic list, when the endpoint's interval is due | fills it with the modern pad's latest state as a 20-byte Duke report, completes it, links it onto the done list |
| Queues an interrupt-OUT transfer (rumble) | the same | hands the two motor values to the controller driver |
| Waits for its interrupt | the extension calls the title's interrupt routine | after the frame's done list and `DoneHead` (HCCA +0x84) are written back |

The setup the title performs, step by step:

| Step | The title | The virtual controller |
|---|---|---|
| 1 | reads `HcRevision` | OHCI 1.0 |
| 2 | writes `HcCommandStatus` (HostControllerReset) | registers back to their reset values |
| 3 | writes `HcHCCA`, the list heads, then `HcControl` (operational, lists on) | stores the pointers, starts the virtual frames |
| 4 | reads `HcRhPortStatus` | "connected" where a pad (real or modern) is plugged in |
| 5 | writes the port's reset bit | reset done about 10 ms later, with the interrupt |
| 6 | queues GET_DESCRIPTOR for address 0, writes "control list filled" | answers it in the next frame: data into the title's buffer, completion code, done list, interrupt |
| 7 | its interrupt routine reads `HcInterruptStatus`, writes it back to clear it | "done head written"; cleared |
| 8 | set address, configuration, XID descriptors: steps 6-7 again | the Duke's answers |
| 9 | polls reports and sends rumble | §2's table |

Every frame the virtual controller also advances the frame number in the HCCA, and `HcFmNumber`/`HcFmRemaining` read to match. Plug and unplug set the
virtual root port's "connect status change" and raise the interrupt, so the title enumerates or drops the device as it would a real one.

**How titles reach the registers.** nxdk's USB code uses the physical address as a pointer (`USBH` = 0xFED00000, `third_party/nxdk/lib/usb/libusbohci_xbox/
usbh_config_xbox.h`), so it relies on the kernel's own mapping of that range: the page the trap needs is the kernel's, and the trap must make that one page
not-present for the title while the extension reaches the real controller through a mapping of its own. How the XDK's USB library reaches them (the same
mapping, or `MmMapIoSpace`) and connects IRQ 1 and IRQ 9 (`HalGetInterruptVector`, `KeConnectInterrupt`) is the first research item (§8, step 0).

## 3. Where the work runs
The console has one CPU, so nothing here may busy-wait, and none of it runs on the extensions' run loop (spec/oxos_internal/34 §4.3: one passive thread, 1 ms
waits, no timing guarantee).

| Path | Runs in | Budget | Does |
|---|---|---|---|
| Register trap | the kernel's page-fault handler, in the title's context, at whatever IRQL the title was at (often inside its own USB interrupt routine) | a few µs, never waits | the register accesses of §2 |
| Virtual frame | a kernel timer DPC every 1 ms (DISPATCH_LEVEL) | tens of µs | walks the title's lists, copies a few hundred bytes, writes the done list, then calls the title's interrupt routine at its own device IRQL |
| Real controller | the extension's interrupt routine for IRQ 1/9, then a DPC | tens of µs | the extension's own host driver and the controller drivers: enumeration as a state machine (never blocking), reports, rumble |
| Run loop (as today) | the extensions' passive thread | ms | settings, logging, pad-changed events: nothing time-critical |

Shared state between these is touched with interrupts off, which is enough on a single-CPU console.

## 4. The pieces
| Piece | Where | Starting point |
|---|---|---|
| Page-fault hook: a range check at the top of the page-fault handler that hands a registered range to its callback | kernel, resident (a few dozen bytes: titles keep their pages, CLAUDE.md "Resident memory") | the one trap dispatcher, `core/exceptions.cpp` `KiDispatchTrap` |
| The instruction decoder and the virtual OHCI (registers, list walker, done list, frame counter, the title's interrupt) | the extension | the OHCI 1.0a specification (public); xemu's OHCI model only as a description of the hardware |
| A host driver for the real controller (its own HCCA, lists and buffers), with hubs | the extension | OXOS's boot-time driver (`hardware/usb/host_controller.cpp`, `devices.cpp`) |
| Controller drivers: GIP (Xbox One / Series: the start-up handshake, input, rumble) first; Xbox 360 wired and the 360 wireless receiver next; HID pads (PlayStation, Switch Pro, generic) later | the extension, one source file each | GIP: a working driver already exists in OG-AmpJack (github.com/Roasted-Codes/OG-AmpJack, proven on the console: handshake, input, wake, rumble); its protocol files (`src/core`, ported from xone) are **GPL-2.0-or-later**, so OXOS (GPL-2.0) can reuse them, keeping their copyright headers; the rest of OG-AmpJack is GPL-3.0-or-later and is not taken 360: the USB descriptors and reports of the pads themselves (observed), the Linux xpad driver (GPL-2.0) as a description |
| Duke emulation: a Duke's descriptors, the report and rumble translation | the extension | descriptors captured from our own controllers |
| Pass-through of real Xbox devices: pads (interrupt), memory units (bulk), headsets (isochronous) | the extension | forwards the title's transfers to the real device unchanged |

Translation into a Duke report (defaults; `oxos_controllers.ini` may change them, §6):

| Modern pad | Duke |
|---|---|
| A, B, X, Y | the analog buttons A, B, X, Y: 0 or 0xFF |
| LB, RB | WHITE, BLACK: 0 or 0xFF |
| LT, RT | LT, RT (0-0xFF, scaled from the pad's 10 bits) |
| sticks, stick clicks | sticks (16-bit signed), LEFT and RIGHT THUMB |
| D-pad, Menu, View | D-pad, START, BACK |
| Xbox button, Share | not mapped (free for OXOS's in-game shortcuts, spec/oxos_internal/48) |
| rumble (Duke: two motors) | the pad's two main motors; the trigger motors stay off |

## 5. What the kernel and the SDK need
Spec/oxos_internal/34's extensions can already use kernel timers, DPCs, interrupts and `MmMapIoSpace` (`oxos_kernel.h`) and hook any export. New:

| # | Addition | Why |
|---|---|---|
| 1 | **MMIO trap:** register a physical range with read and write callbacks, and remove it | the core: the kernel makes the title's view of the range not-present and calls the callbacks from the page-fault handler |
| 2 | **Physical memory read and write**, safe at any IRQL | the title's lists and buffers are often in its own memory, not mapped at 0x80000000; the kernel already reads them through one reserved page for the pad snoop (spec/oxos_internal/22 §9.2) |
| 3 | **Taking a device's interrupt from the title**, and calling the title's interrupt routine at its IRQL | the extension needs the real IRQ 1/9; the title's `KeConnectInterrupt` for them is kept aside |
| 4 | **DMA memory where no title claims memory** | the real controller's HCCA and descriptors (spec/oxos_internal/34 §8.2's placement rules) |
| 5 | **Device ownership:** while an extension owns a controller, the kernel leaves it alone | the quick reboot's reset of both OHCI (spec/oxos_internal/25 §5) and the done-list pad snoop (spec/oxos_internal/22 §9) skip an owned controller. Real devices then stay set up across title launches |
| 6 | **Pad state fed by the extension** (`oxos_pad_report(port, state)` into spec/oxos_internal/34 §7.3's path) | the in-game shortcuts, toasts and pad-changed events keep working when the snoop no longer sees the real reports |
| 7 | **Rules for these callbacks** in the SDK docs | trap callbacks run at any IRQL inside the title's instruction stream and take µs; timer DPCs at DISPATCH_LEVEL, bounded; no waits, no FPU (spec/oxos_internal/34 §8) |

Items 1 and 2 are the real kernel work; 3-6 are small; 7 is documentation. The same seven would serve a virtual network adapter later (the NIC has the same
shape: registers, rings in RAM, an interrupt).

## 6. The extension
- `C:\OXOS\extensions\oxos_controllers.xos`, listed in `oxos_extensions.ini` with `enabled = yes` (off unless installed, as every extension: titles lose
  pages only when it is on).
- Settings in `C:\OXOS\extensions\oxos_controllers.ini`: which drivers are on, the button mapping, which ports the virtual controller takes over.
- Safe mode (B at boot, spec/oxos_internal/34 §3.3) loads no extensions, so a title always gets the real controllers back.

## 7. Limits
- Wired pads, and the Xbox 360 wireless receiver. Not the Xbox wireless adapter for One/Series pads (it loads its own firmware and uses its own radio
  protocol), not Bluetooth.
- A pad appears as a Duke: no extra buttons, no trigger motors, no headset on a modern pad's audio jack.
- Stock kernels and other BIOSes do not have it: it is an OXOS kernel extension.

## 8. Steps
| Step | What | Done when |
|---|---|---|
| 0. Research | REA: how the stock kernel and an XDK title's USB library reach the OHCI registers and connect IRQ 1 and IRQ 9; whether the stock kernel touches the controllers after the cold boot | a research spec answers both, for 4627 and 5838 |
| 1. Pilot (own worktree) | the trap, the virtual OHCI, one virtual Duke on port 1 fed by a Series pad through the GIP driver; no pass-through (real controllers unavailable in that mode) | Halo 2, an nxdk app (the FTP server) and the controller test title read the Series pad on the console |
| 2. Pass-through of real pads | real Duke and S pads forwarded on the other ports | a real pad and a Series pad play together |
| 3. Memory units and headsets | bulk and isochronous pass-through | a save to a memory unit inside a title; headset audio |
| 4. More controllers | Xbox 360 wired, the 360 wireless receiver; then HID pads | each plays in the pilot's titles |
| 5. Release | the kernel interface of §5 in the SDK (`tools/gen_oxos_sdk.py`), `test_extensions` coverage, the extension and its ini in the release zip | the regression passes, 15 960 free pages kept with the extension off |

**Tests:**
- xemu: the controller test title (`tests/controller_test_title`) through the virtual controller with xemu's own pads, for the virtual OHCI's register and list
  behaviour.
- The console (the Modxo rig: serial log, capture card) for the real controllers and a Series pad.
- Titles of every kind: XDK titles of several library versions, nxdk apps, homebrew.

## 9. Open questions
- How faithful the virtual OHCI must be: frame timing, port reset durations and status changes that some titles' drivers may check.
- Whether a modern pad is shown as a Duke with its internal hub and two memory-unit slots, as a real Duke is, or as the XID device alone.
- Port numbering when a modern pad and real pads share a controller: which root ports the extension virtualises.
- The page the trap needs is the kernel's own mapping of the OHCI range (§2): the extension's own access then goes through a second mapping, and nothing else
  in the kernel may touch the controller through the first.
- Rumble strength mapping, and whether the trigger motors should carry anything.
