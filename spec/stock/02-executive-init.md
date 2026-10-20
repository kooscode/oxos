# Spec 02 — Executive init, memory map, cold vs quick reboot (xboxkrnl 1.0.4627.1)

Clean-room behavioural spec (no decompiled code). Evidence: batch_decompile `ev_3d981308…`
(0x80015724) and `ev_d44468df…` (its callees), plus objdump of 0x8001af9c and the PE header.
Names marked (inferred) are not symbols in the binary.

## 1. Two boot modes: cold boot and quick reboot
A byte flag (`0x8003A87C`, inferred name `KiQuickReboot`) separates them.
- **Cold boot**: entry from the 2BL (spec 01). Flag = 0.
- **Quick reboot**: `HalReturnToFirmware` with the quick-reboot routine jumps to 0x8001af9c, which:
  1. disables interrupts and resets the stack to the boot stack
  2. calls a pre-reset hook (0x8001eabc; purpose not yet established)
  3. **re-copies initialized `.data` from flash**, zeroes the rest of `.data`/BSS (sizes below)
  4. sets flag = 1, then repeats the processor bring-up from spec 01 §2.4 onward (IDT, PCR, Kd,
     kernel init) and goes idle. Cold-only hardware and memory init is skipped.
  This is how titles are launched (and how they return to the dashboard): **the kernel restarts in
  place** while persisted memory (`MmPersistContiguousMemory`, `LaunchDataPage`) survives.

### Header fields the quick reboot depends on (ABI for our image builder)
These sit in the unused DOS-header bytes of the kernel PE:
| file off | value (4627) | meaning |
|---|---|---|
| 0x28 | 0x4BCC | bytes to zero after the copy (rest of `.data`, i.e. BSS) |
| 0x2C | 0x0A00 | initialized `.data` size to copy |
| 0x30 | 0xFFFF9400 | **source address in flash** (= bank offset 0x39400, top-of-4GB mapping) |
| 0x34 | 0x80035900 | destination (= `.data` VA) |
**Hardware note:** a custom BIOS layout (for example 512 KB / 1 MB banks) must keep this copy of
`.data` in flash and point 0x30 at its mapped address. Otherwise quick reboot (every title launch)
reads garbage.

## 2. Executive init order (0x80015724, runs under SEH from KiInitializeKernel)
1. HAL phase 0: cold → full hardware init (§3); quick → only the interrupt-controller step.
2. MmInitSystem (§4).
3. Cold only: detect hardware and fill `XboxHardwareInfo` (§5).
4. ObInitSystem. On failure: `KeBugCheck(0x5E /*OBJECT_INITIALIZATION_FAILED*/)`.
   - Reset the handle table (`ObpObjectHandleTable`, ord 245)
   - Create the root directory, then **`\??`**, **`\Device`**, **`\Win32NamedObjects`**.
5. PsInitSystem. On failure: `KeBugCheck(0x60 /*PROCESS_INITIALIZATION_FAILED*/)`.
   - Init a DPC (0x8003AC80, routine 0x80021b34; purpose not yet established)
   - `PsCreateSystemThread(…, StartRoutine = 0x8001576F, …)` then `NtClose` the handle.
   - **Phase-1 init runs in that system thread** (spec 03: I/O, IDE, FATX, XBE launch).

## 3. HAL phase 0, cold path (0x8006281f, INIT)
Between each step a "pulse" routine runs (0x80030980). On cold boot, unless bit 0 of 0x8003B138 is set,
it writes 5 then 4 to **I/O port 0x80CF** with 5 µs stalls. Port 0x80CF is in the MCPX I/O
range starting at 0x8000. Purpose unknown: possibly a progress or watchdog signal.
Steps in order:
1. `out 0x61, 0x08`: system control port B (bit 3 = disable IOCHK NMI).
2. **Disable the local APIC**: clear bit 11 of MSR 0x1B (IA32_APIC_BASE). The Xbox uses the 8259 PIC only.
3. **8259 PIC init** (0x800145dc; this step also runs on quick reboot). With interrupts disabled, for
   each of master (0x20) and slave (0xA0): ICW1 = 0x11; ICW2 = vector base (**master 0x30, slave
   0x38**); ICW3 = 0x04 (master: slave on IRQ2) / 0x02 (slave id); ICW4 = 0x01; then OCW1 = 0xFF
   (all masked). **It returns with interrupts enabled** (it forces IF on in the restored flags).
   IRQ n → vector 0x30+n, so the 64-entry IDT covers vectors 0x00–0x3F.
4. **CMOS RTC init** (0x80055000), done with interrupts disabled: reg A = 0x2D (32.768 kHz base, rate field 0xD);
   reg B = (old bit 0) | 0x02 (24-hour, BCD, no RTC interrupts); read C and D to clear them.
5. **PIT channel 0** (0x80063cb8): control 0x34 (lobyte/hibyte, mode 2); count **0x0465 = 1125**. With
   the Xbox's 1.125 MHz PIT clock this gives a **1 ms tick** (KeTickCount was observed rising ~1/ms in xemu).
6. RTC again (0x80030b10): same reg B setting, read C, then set an "RTC initialized" flag (0x800362FD).
7. **PCI/MCPX/NV2A configuration** (0x80063908), using `HalReadWritePCISpace` read-modify-write of
   the 256-byte config spaces. Bus 0 device numbers:
   - **dev 1 (MCPX ISA/LPC bridge)**: reg 0x6A = 0x03; 0x6C = 0x0E065491; 0x64 = 0x00000B0C; 0x81 |= 0x08;
     later reg 0x8C: set bit 27, clear bit 26; then `out 0x80CD, 0x08`.
   - **dev 9 (IDE)**: command |= I/O+bus-master; programming interface &= ~0x05 (both channels in
     legacy mode); **BAR4 = 0xFF61 (bus-master registers at I/O 0xFF60)**; 0x50 = 2;
     0x58 = 0x20202020; 0x60 = 0xC0C0C0C0 (timing / UDMA set-up).
   - **dev 4 (NIC)**: command |= 7; BAR0 = 0xFEF00000; BAR1 = I/O 0xE000; interrupt line 4.
   - **dev 2 (USB0 OHCI)**: BAR0 = 0xFED00000; interrupt line 1; reg 0x50 = 0x0F.
   - **dev 3 (USB1 OHCI)**: BAR0 = 0xFED08000; interrupt line 9; reg 0x50 = 0x30.
   - **dev 6 (AC'97)**: BAR0 = I/O 0xD000, BAR1 = I/O 0xD200, BAR2 = 0xFEC00000; interrupt line 6; later the 16-bit reg **0x4E** |= 0x0101 (read as a dword at 0x4C, high half modified).
   - **dev 5 (APU)**: BAR0 = 0xFE800000; interrupt line 5.
   - **dev 0x1E (AGP bridge)**: command |= 7; buses primary 0 / secondary 1 / subordinate 1;
     memory window 0xFD000000–0xFE7FFFFF; prefetch base 0xF000, limit 0xF3F0 | ((HwFlags & 8) << 7).
   - dev 0 reg 0x48 = 0x114. **Bus 1 dev 0 (NV2A)**: command |= 7; interrupt line 3, pin 1; reg 0x4C = 0x114.
8. **ACPI PM block at I/O 0x8000** (0x80054eb0): set bit 0 of 0x8004 (PM1 control, SCI enable), bit 1 of 0x8022, bit 0 of
   0x8002 (PM1 enable, timer), and bit 0 of 0x8028.
CMOS accesses are bracketed by "save flags + cli / restore flags" helpers (0x80015710 / 0x8001571c).
**Hardware note:** xemu accepts most PCI setups. Real hardware needs these values, so they belong
in milestone 1 even though xemu does not check them.

## 4. Memory manager init (0x8001e88a) — address-space layout (ABI)
Classic NT x86 self-mapped paging: **page tables at 0xC0000000, page directory at 0xC0300000**.
- User half (PDE 0–0x1FF, 0–2 GB): cleared.
- **0xF0000000–0xF7FFFFFF**: 32 × 4 MB large pages identity-mapped to physical 0xF0000000.
  Flags: present, write, PWT, accessed, dirty, PS (GPU/AGP area).
- **0xF8000000–0xFF7FFFFF**: 30 × 4 MB large pages identity-mapped, **uncached** (PCD|PWT). This
  covers the MMIO: NV2A at 0xFD000000, MCPX/USB/NIC, flash at 0xFF000000.
- PDE for 0xFFC00000: cleared.
- Reads PCI bus 0, device 0, config byte 0x87 (host bridge). Probably the RAM size or type; to be confirmed.
- Physical memory init: cold → full init (0x800557f4); quick → **re-adopt persisted memory** (0x8001fa6c).
- **0x80000000–0x83FFFFFF (64 MB)**: 16 PDEs pointing at page tables in physical memory starting at
  0x03FF0000, so all of RAM is visible at 0x80000000. PDEs for 0x84000000–0x8FFFFFFF are cleared.
  **128 MB consoles** (devkits, RAM-upgraded retail units) will need those extra PDEs. Check how
  this kernel handles them before deciding what ours does.
- Then: further init (0x80018e38), pool init (0x80012dc2), a lookaside/zone init (0x8001f745),
  and **`FscSetCacheSize(16)`**: the initial file-system cache is 16 pages.

## 5. Hardware detection → `XboxHardwareInfo` (ord 322, 0x8003B150), cold only
`struct { ULONG Flags; UCHAR GpuRevision; UCHAR McpRevision; … }`
- Map 4 bytes of physical **0xFFF00078**. If the byte == 0x80 → Flags |= 0x10; additionally if port
  **0x80CE bit 5** is set → Flags |= 0x30. (Meaning of 0x10/0x20 to be established; likely a
  devkit/debug-hardware indicator.)
- NV2A register **0xFD101000 bits 25–26** (strap): value 0 → Flags |= 0x40. The assembly then compares the
  *masked* value with 1 for Flags |= 0x80, which can never match: **0x80 is never set** (confirmed, 0x800550c1).
- The 0xFFF00078 test sets 0x10, and 0x80CE bit 5 adds 0x20 (so 0x30 together). **Flag 0x20 selects the Focus
  FS454 video encoder tables** (spec 17 §1); without it the kernel uses the Conexant CX25871 tables.
- **McpRevision** = PCI bus 0, device 1, config byte 0x08 (MCPX revision ID).
- **GpuRevision** = low byte of NV2A register 0xFD000000 (boot ID).
- **Ours, encoder flags:** OXOS's BIOS image has 0x70 at flash offset 0x78, so 0x20 would never be set from it; the
  encoder bits come from a bus probe instead (spec/oxos-internal/04-tv-encoders.md §2): Focus → 0x20, Xcalibur → 0x400
  (the 5838 kernel's flag, spec 19 §1.1), as the reference reports them on those consoles.
- Ours (milestone 6c) matches otherwise; it runs right after MM init (it needs `MmMapIoSpace`). In xemu: flags 0x41, GPU 0xA3,
  MCPX 0xB2.
- If McpRevision < 0xB4 → Flags |= 0x01 (probably marks early, 1.0-era hardware; meaning not confirmed).
Titles read `XboxHardwareInfo`, so these bits must match on each console revision.
- It is filled at cold boot only and **survives quick reboots**: 0x8003B150 lies past the `.data`/BSS range the quick
  reboot re-copies and zeroes (§1), so a title launched by another title sees the same values. Titles depend on it:
  Half-Life 2's USB library (in `hl2_xbox.xbe`, launched by its `default.xbe`) sizes its endpoint descriptors by
  Flags bit 0; with the flags zeroed it ran out of them on the second hub and never enumerated the controller.
  Ours keeps it in the persistent section.
- The rest of the reference's `STICKY` section (0x8003AEE0-0x8003C870) is kept the same way where ours does not
  rebuild it on every boot: the cold-boot AV pack (0x8003AFEC, compared on AV-pack insertion), the SMC "title loaded"
  flag (0x8003AFE8, set by the loader and never cleared) and the PHY-initialized flag (0x8003B340). Keys, boot flags,
  console region, LaunchDataPage and the AV mode are persistent too; the EEPROM shadow, disk identity, partition
  objects and encoder type are re-read at every boot in ours. The SMC scratch copy (0x80035A14) is in `.data`: it is
  lost on a quick reboot in both.

## 6. Our implementation (milestone 5d-2)
- Variables that must survive a quick reboot (EEPROM key, HD key, cert key, LaunchDataPage, console region,
  AV mode state, the quick-reboot flag/count, a copy of the 2BL boot keys) are placed in a `.persist` section
  (`XK_PERSIST`) after `.data`/`.bss`, so the header-driven re-copy/zero leaves them alone.
- `src/arch/quick.S` `KiQuickReboot`: interrupts off, switch to a persisted stack, quiesce devices (PICs masked,
  IDE bus master, OHCI, NV2A interrupts, NIC DMA stopped), re-copy `.data` from flash, zero the rest, set the
  flag, re-enter `KernelEntry` with the persisted boot-key copy.
- Quick path: HAL phase 0 skips hardware detection, `MmInitSystem(true)` keeps PTE-array entries that are
  PRESENT|PERSIST above the image (`MmPersistedPages`) and clears the system-PTE window; self-tests are skipped.
- Tested by `make test-relaunch` (spec 05 §9).

## Open questions
- The pre-reset hook 0x8001eabc; the 0x80CF pulse; the 0x8003B138 skip-pulse bit (spec 03 §boot flags).
- Meaning of XboxHardwareInfo flags 0x10/0x20/0x40/0x80/0x01 (check against several console revisions).
- PCI 0:0 reg 0x87 and 128 MB RAM handling.
- Phase-1 system thread 0x8001576F → spec 03.
