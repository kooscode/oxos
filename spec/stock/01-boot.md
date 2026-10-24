# Spec 01 — Kernel entry and processor bring-up (xboxkrnl 1.0.4627.1)

Clean-room behavioural spec derived with REA + Ghidra 12.1.4 from
sha256 6bf7aecd…c407f. It describes *what* must happen and the layouts
that must match. It intentionally contains no decompiled code.
Names marked (inferred) are not symbols in the binary.

Evidence: entry `ev_…` from analyze_function 0x8005504c; startup
`ev_fb149042…`; callees batch `ev_c280adc6…`.

## 1. Entry contract — PE entry point (RVA 0x4504c)
- Called by the 2BL as `cdecl entry(arg0, const uint8_t* keys)`; returns 0 (never in practice).
- `keys[0..15]`  -> 16-byte **EEPROM key** (exported data, ordinal 321 `XboxEEPROMKey`).
- `keys[16..31]` -> 16-byte second key stored immediately before `XboxHDKey` (likely the CERT key; inferred).
- Zero-fill **0x6000 bytes at VA 0x80400000**: this is exactly the size of the 2BL image
  (24576 bytes), so the kernel erases the boot loader from RAM. A compatible kernel should do the same.
- Then jump to processor start-up (`KiSystemStartup`, inferred, 0x80055718). Never returns.

## 2. Processor start-up sequence (order matters)
1. **GDT**: 7 descriptors (limit 0x37) at a fixed table in `.data`. The image stores each descriptor
   with its high base/flag bytes rotated; a fix-up pass restores the standard x86 descriptor byte
   order before `lgdt`. Load **TR = selector 0x18**.
2. Copy current **CR3** into the CR3 field of three TSSs (main, plus two fault TSSs: double fault and NMI; inferred).
3. **CPU microcode**: `cpuid(1).eax` == 0x686 or 0x68A -> write the matching embedded update blob
   address to MSR 0x79 (IA32_BIOS_UPDT_TRIG). Other steppings: skip. (Optional for emulators,
   which ignore it. On real hardware the stock Xbox CPU is family 6 model 8, a Coppermine-based Pentium III.)
4. **IDT**: 64 gates (limit 0x1FF). Like the GDT, the image stores them in a linker-friendly
   format; a fix-up pass swaps the two 16-bit halves of each entry into gate format, then `lidt`.
5. **PCR/PRCB init** (`KiInitializePcr`, inferred, 0x8001c5da). The single processor control region
   lives at **0x80035BDC**, and FS must point to it. Fields set:
   - NtTib.ExceptionList = 0xFFFFFFFF; NtTib.StackBase/StackLimit = initial (boot/idle) stack
   - SelfPcr -> PCR, Prcb -> PRCB at PCR+0x28
   - Prcb.CurrentThread -> **idle thread object 0x80035A90**
   - Idle thread: initial stack 0x80039840, stack limit 0x80036840 (0x3000 bytes)
   Games and statically linked XDK libraries read `fs:[...]` directly, so these offsets are ABI.
6. **Debugger init** (`KdInitSystem(TRUE)`, inferred): one-time init. Sets `KdDebuggerEnabled` (ord 88) from a
   probe; if enabled, does further debugger setup.
7. Raise IRQL to HIGH_LEVEL (0x1F) via `KfRaiseIrql`.
8. **Kernel init** (`KiInitializeKernel`, inferred, 0x8001c65d):
   - init DPC list head; lower to APC_LEVEL; misc init
   - init the idle process and the system process (base priority 0 / 8; quanta 0x7F / 0x3C)
   - init idle thread object (stack 0x3000 bytes, owner = system process)
   - idle thread NPX save area (0x210 bytes at stack top) zeroed; **FPU CW = 0x027F**, **MXCSR = 0x1F80**
   - call executive init (**0x80015724**, under SEH; next spec)
   - allocate the DPC stack: `MmCreateKernelStack(0x3000)`; on failure `KeBugCheckEx(0x4D /*NO_PAGES_AVAILABLE*/,1,0,0,0)`
   - set idle thread priority 0; if no next thread was scheduled, set a flag; end at HIGH_LEVEL
9. Back in start-up: PCR IRQL byte = 0x10 (the field written is inferred); `KfLowerIrql(DISPATCH_LEVEL)`; enter the idle loop.

## 3. Idle loop (0x8001b025, `KiIdleLoop`, inferred)
Runs forever at DISPATCH_LEVEL:
- If the DPC list is non-empty: clear the DISPATCH software interrupt and drain the DPCs.
- If `Prcb.NextThread` is set: take it, clear it, make it current, and swap context.
**Observed at runtime:** under xemu with the stock BIOS, EIP was 0x8001b02f (inside this loop) with
EBX = 0x80035BDC (PCR), and KeTickCount rose by 1 per ms. A compatible kernel should show the same
signs when idle.

## 4. State at the kernel's entry point (observed, stock 2BL)
Observed with a hardware breakpoint on our `KernelEntry` under the stock BIOS in xemu; the open image (§5)
reproduces it:
- Flat 32-bit protected mode: CS = 0x08, DS/ES/SS = 0x10, FS = GS = 0, interrupts off; the GDT/IDT registers
  still point at the boot ROM's table (0xFFFFFFD8; the kernel loads its own at once).
- CR0 = 0x80010031 (PG, WP, NE, ET, PE), CR4 = 0x610 (PSE, OSFXSR, OSXMMEXCPT), CR3 = 0xF000; FPU at reset
  defaults (FCW 0x37F, MXCSR 0x1F80).
- Page directory at 0xF000, 4 MB pages: entries 0x000-0x03F map 0-256 MB 1:1 and entries 0x200-0x23F map the same
  at 0x80000000 (flags 0xE3); entry 0x300 = the directory itself (0xF063); 0x3F4-0x3F7 = NV2A registers
  0xFD000000-0xFDFFFFFF uncached (0xFB); 0x3FF = flash 0xFFC00000 (0xE3).
- MTRRs (recorded by the kernel at entry): default type uncached, enabled (0x800); range 0 = 0-64 MB write-back;
  range 1 = 0xFFF80000-0xFFFFFFFF (512 KB of flash) write-protected; the others unused.
- Stack 0x803FFFE8 = {return address, arg0 = 0x80400004, keys = 0x0040006C}. The 2BL itself occupies
  0x80400000-0x80405FFF, which the kernel clears.

### 4.1 State at the end of start-up (observed on hardware, 2026-10-24)
Read with the machine-state probe (`tools/statedump`) injected into a reference kernel (5838-based EvoX M8) on the
no-DVD 1.4 console, just before the title starts: CR0 = 0x8001003B (adds **MP** and TS to the entry value; TS is the
lazy-FPU arm of spec 07), CR4 = 0x610 (**no MCE**), microcode revision 1 (§2 step 3), PAT 0x0007010600070106,
MTRRs as §4, IA32_DEBUGCTL = 1, the CS descriptor's limit 0xBFFFFFFF (granular). Where the reference sets CR0.MP:
**open** (the per-thread CR0 word at FXSAVE area +0x200, spec 07, is OR-ed into CR0 at context switches).
PCI configuration spaces, NV2A PFB registers, the ACPI/GPIO block and every other listed MSR matched OXOS's.

## 5. The open BIOS image (ours)
`make bios` (IMAGE=open) builds a 1 MB image with no Microsoft parts that boots with or without an MCPX boot ROM.
Each 256 KB bank (`tools/mkbios.py`, `build_open`):

| offset | contents |
|---|---|
| 0x00000 | MCPX header and X-code table (`kernel/boot/xcodes.S`): XboxDev/cromwell's `boot_rom/Xcodes.h` (GPL-2.0), fetched at build time |
| 0x01000 | our 2BL (`kernel/boot/start.S`, `bl.cpp`), entered at 0xFFFC1000 |
| 0x06000 | payload header {'OXKP', kernel offset, packed size, image size, key-block offset, key-block size, splash offset, splash size} |
| 0x06040 | key block, then the flat kernel image as a raw deflate stream |
| … | a copy of the boot splash when it fits (0, 0 in the header otherwise; spec/oxos-internal/00-boot-splash.md) |
| … | the kernel's `.data` initializer (quick reboot), ending at 0x3FE00 |
| 0x3FE00 | our reset code (`kernel/boot/reset.S`) |

Bank 1 holds the splash instead (spec/oxos-internal/00-boot-splash.md). `make bios-256k` writes one bank alone, for
256 KB flash chips and modchip banks.

- **With an MCPX ROM** (a console, or xemu given a ROM dump): the ROM runs the X-codes. Their last two write
  `mov eax, 0xFFFC1000; jmp eax` to RAM address 0 (Cromwell's "overflow trick"). The ROM's check of the
  Microsoft 2BL then fails; it hides itself, and execution runs on from address 0 into our 2BL. No MCPX key is
  involved, so the same image serves MCPX 1.0 and 1.1 (console revisions 1.0-1.5). `MCPX16=1` selects
  Cromwell's header and X-code values for the 1.6's MCPX.
- **Without a ROM** (xemu with an empty `bootrom_path`): the CPU starts at 0xFFFFFFF0, in our reset code. It
  enters flat protected mode, interprets the same X-code table and jumps to 0xFFFC1000. On hardware the MCPX ROM
  hides this range, so the code never runs there.
- **2BL:**
  - answers the SMC's start-up challenge first: reads SMC registers 0x1C-0x1F and writes the answer to 0x20 and
    0x21 (the algorithm as in XboxDev/cromwell's `boot_rom/2bPicResponseAction.c`). On a console the SMC resets the
    machine and flashes the LED red and green when no answer comes within a few seconds; xemu does not check it.
    Added 2026-10-21 after the first hardware run (a 1.4 console) ended that way;
  - loads its own GDT, sets the MTRRs as in §4 and enables the caches;
  - copies its C part to 0x400000;
  - inflates the kernel to 0x10000 (`xk/inflate.h`, shared with the PNG decoder);
  - builds the §4 page directory and sets CR4, CR3 and CR0 as in §4;
  - calls `KernelEntry('OXOS', keys)`.
- **Key block:** 32 zero bytes (no EEPROM or certificate key), then 'OXBK', a count, and per EEPROM key
  version (Cromwell's versions 9-12) the SHA-1 states after the HMAC inner and outer pad blocks. These come from
  Cromwell's `drivers/ide/BootHddKey.c`. The kernel keeps them across quick reboots, and `ExInitEeprom` tries
  each one until the security section authenticates (`ExpEepromKeySource` = 2 + its index). The certificate key
  stays zero, so titles' LAN and signature keys differ from a stock console's.

## Open questions
- What the start-up routine's repeated writes of return addresses to 0x800395EC are for (likely a
  boot-progress breadcrumb for debugging).
- The exact meaning of each of the 7 GDT entries, and the TSS layouts.
- Executive init at 0x80015724: Hal/Mm/Ob/Io/Ps phases, IDE, FATX, XBE launch. This is the next spec.

## Note: modded BIOS 2BL kernel patcher (found 2026-10-15)
The modded BIOS used as our packing base has a 2BL stub that copies a table of
`{u8 length, u32 address, bytes}` patches (0xFF ends it) onto the decompressed kernel before jumping to it. The
code is `call $+5; pop esi; cld; xor ecx,ecx; add esi,imm8`, and the table is at 2BL+0x3B2E with 12 patches. The
addresses are stock-kernel locations, so with our kernel they corrupt arbitrary code. `tools/mkbios.py` empties the
table when it finds the stub.
