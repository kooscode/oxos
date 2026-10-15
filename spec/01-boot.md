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

## Open questions
- What the start-up routine's repeated writes of return addresses to 0x800395EC are for (likely a
  boot-progress breadcrumb for debugging).
- The exact meaning of each of the 7 GDT entries, and the TSS layouts.
- Executive init at 0x80015724: Hal/Mm/Ob/Io/Ps phases, IDE, FATX, XBE launch. This is the next spec.

## Note: modded BIOS 2BL kernel patcher (found 2026-10-15)
The "Complex 4627" BIOS used as our packing base has a 2BL stub that copies a table of
`{u8 length, u32 address, bytes}` patches (0xFF ends it) onto the decompressed kernel before jumping to it. The
code is `call $+5; pop esi; cld; xor ecx,ecx; add esi,imm8`, and the table is at 2BL+0x3B2E with 12 patches. The
addresses are stock-kernel locations, so with our kernel they corrupt arbitrary code. `tools/mkbios.py` empties the
table when it finds the stub.
