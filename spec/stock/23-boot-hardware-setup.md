# Spec 23 — CPU and chipset set-up by the kernel, entry to first title (xboxkrnl 1.0.5838.1; 4627 and EvoX M8 compared)

Clean-room behavioural spec. Sources: disassembly (objdump) and Ghidra 12.1.4 analysis (REA) of the shipped binaries
`5838-unpacked/xboxkrnl.exe` (primary; sha256 20adec79…), `xboxkrnl-4627.exe`, `evoxm8_unpacked/xboxkrnl.exe` and
`evoxm8_unpacked/3c900_evox_loader.bin` (disassembled at VMA 0x400000). Addresses are 5838 unless marked. No code is
reproduced. Where an earlier spec already catalogues a register sequence it is referenced, not repeated.
**Status: reference only.**

Code ranges used for the searches (Ghidra function entries; the rest of INIT is data such as the microcode blobs at
0x80061E54/0x80062654, the boot animation's assets and the boot sound): .text 0x80012BA0–0x800357FF;
INIT 0x8003C3E0–0x80048FFF, 0x8004D574–0x80052CFF, 0x80054750–0x80061E23, 0x80062E24–0x800631B4,
0x800A7A98–0x800A8C91. Every `out`, `wrmsr`, `mov crN,…`, `wbinvd`, `invd`, `invlpg`, immediate-address MMIO store
and call of the PCI helper inside these ranges was enumerated; byte patterns that look like such instructions
outside them are data (checked: 0x8006356B, 0x80065871, 0x80067A22, 0x800687CC, 0x8006B55E, 0x8007257C …).

## 0. What the stock kernel never does (5838 and 4627)
| Item | Finding |
|---|---|
| `mov cr4` | none. CR4 stays as the 2BL left it (0x610: PSE, OSFXSR, OSXMMEXCPT). **PGE and MCE are never set.** |
| `wrmsr` | exactly two sites: microcode trigger MSR 0x79 (0x80061E21) and IA32_APIC_BASE 0x1B (0x80061D52, through the helper 0x80031CF4). **No MTRR, PAT, MCG_CTL, MCi_CTL/STATUS or IA32_DEBUGCTL (0x1D9) write.** (4627: 0x80062905 and 0x80030E44, same two MSRs.) |
| `mov cr0` | only FPU bits (MP, EM, TS) in the lazy-FPU code (§6.1). CD/NW are never touched. |
| `mov cr3` | only reloads of the current value (0x80018FAB, 0x80018FB3, 0x8001FF1F). The page directory never moves: it is the 2BL's (CR3 = 0xF000), edited in place through the self-map at 0xC0300000. |
| `invd` | none. |
| `wbinvd` | two code sites: the TLB+cache flush helper 0x80018FB0 (§5.5) and the graphics library at 0x80041863 (§2.7). |
| Global pages | no PTE/PDE has bit 8 set; CR4.PGE is off, so every CR3 reload flushes the whole TLB. |
| PAT bit in PTE/PDE | never set (bit 7 of a 4 KB PTE, bit 12 of a 4 MB PDE). Only PWT/PCD select the type (§5.1). |
| Locked instructions | essentially none: three `lock inc` of one counter (0x8003B8A0) in trap code; the interlocked exports use interrupt masking. |
| Port 0xCF8/0xCFC outside the PCI helper | none (all accesses go through `HalReadWritePCISpace`, ord 46, 0x8001530C). |
| Writes to NV2A PBUS 0x1200–0x1264 | none (§4). |

## 1. Processor start-up (cold boot)
| # | Address | Write | Value / effect |
|---|---|---|---|
| 1 | 0x800548EC (PE entry) | memory | 16-byte EEPROM key → 0x8003C140, next 16 bytes → 0x8003BFF8; zero 0x6000 bytes at 0x80400000 (spec 01 §1) |
| 2 | 0x80054FD4 | ESP | 0x8003A5F0 |
| 3 | 0x80055080 | memory | GDT image at 0x8003C298 (7 entries, STICKY section) byte fix-up (spec 01 §2.1) |
| 4 | 0x80054FEE | `lgdt` | base 0x8003C298, limit 0x37; far jump to CS 0x08; DS/ES/SS = 0x10, FS = 0x20; TR = 0x18 |
| 5 | 0x80055013–0x8005501D | memory | current CR3 copied into the three TSSs (0x8003C17C, 0x8003C1E4, 0x8003C24C) |
| 6 | 0x80061DF4 | `cpuid` 1, `wrmsr` 0x79 | signature 0x686 → blob 0x80061E54, 0x68A → blob 0x80062654, EDX = 0; other steppings: nothing (4627: 0x800628D8, `wrmsr` at 0x80062905, blobs 0x80062938 / 0x80063138) |
| 7 | 0x8001C748 | — | quick-reboot integrity check of the flash `.data` copy (no hardware write; EvoX replaces it with `ret`) |
| 8 | 0x8001C7E2, 0x80055042 | `lidt` | IDT at 0x80036A50 (in `.data`), limit 0x1FF, after its own fix-up |
| 9 | 0x8001C792 | memory | PCR/PRCB (spec 01 §2.5) |
| 10 | 0x80018CD9 → §1.1 | ports | debugger init (cold only) |
| 11 | 0x80013CF4 | memory | IRQL → HIGH_LEVEL (0x1F), then kernel init 0x8001C815 → executive init (§2) |

GDT as stored (after fix-up): 0x08 code, base 0, **limit 0xBFFFFFFF** (G=1, D=1, type 0x9B); 0x10 data, base 0,
limit 0xFFFFFFFF (type 0x93); 0x18/0x28/0x30 TSSs at 0x8003C160 / 0x8003C1C8 / 0x8003C230 (limit 0x68); 0x20 FS at
0x8003708C (the PCR). The 0xBFFFFFFF CS limit is therefore the stock value from entry until §2.8.

### 1.1 Debugger init, cold boot (0x80018CD9 → 0x80061D98, 0x80054FAA)
1. 0x80CF pulse (§2.0).
2. Super-I/O probe at index port 0x2E / data 0x2F, with `jmp $+2` between accesses: 0x2E ← 0x55 (enter
   configuration); index 0x26 must read 0x2E and index 0x27 must read 0; else stop (retail consoles have no Super-I/O).
3. If present: LDN (0x07) ← 4, activate (0x30) ← 1, base 0x60/0x61 ← 0x03/0xF8, exit 0x2E ← 0xAA; then COM1:
   0x3FB ← 0x83, 0x3F9 ← 0, 0x3F8 ← 6 (divisor 6), 0x3FB ← 3 (8N1), 0x3FC ← 3, 0x3F9 ← 0. Result stored at 0x8003C15C.

## 2. Executive init to the title (0x800158B4 onwards)
### 2.0 The 0x80CF pulse (0x8003183D)
Between most steps: unless the quick-reboot flag (0x8003B87C) is set or boot-flag bit 0 (0x8003C138) is set,
port 0x80CF ← 5, stall 5 µs, port 0x80CF ← 4, stall 5 µs. The stall (`KeStallExecutionProcessor`, 0x80015530)
starts with a `cpuid` (serializing) and then spins a calibrated count (0x8003BFF0 loops per µs); its loop is aligned
to 16 bytes with `lea` filler.

### 2.1 HAL phase 0, cold (0x80061D38; quick reboot: 0x8003186C = pulse + PIC only) — same order as spec 02 §3
| # | Address | Write | Value |
|---|---|---|---|
| 1 | 0x80061D3A | port 0x61 | 0x08 |
| 2 | 0x80061D52 | MSR 0x1B | read, bit 11 (APIC global enable) cleared, written back |
| 3 | 0x80013F98 | ports 0x20/0x21, 0xA0/0xA1 | table at 0x80013BD0: master 0x11, 0x30, 0x04, 0x01, mask 0xFF; slave 0x11, 0x38, 0x02, 0x01, mask 0xFF (`jmp $+2` between bytes); **returns with IF set** |
| 4 | 0x800548A0 | CMOS 0x70/0x71 | reg A ← 0x2D; reg B ← (B & 1) \| 2; read C, D (interrupts masked around it) |
| 5 | 0x8006319C | PIT 0x43, 0x40 | 0x34; count 0x0465 (low, high) |
| 6 | 0x800319CC | CMOS | reg B again, read C; flag 0x80036EED ← 1 |
| 7 | 0x80062E24 | PCI | §3.1 |
| 8 | 0x80054750 | ACPI I/O (16-bit RMW) | 0x8004 \|= 1; 0x8022 \|= 2; 0x8002 \|= 1; 0x8028 \|= 1 |
Pulses between every step. (4627: 0x8006281F, PCI 0x80063908, ACPI 0x80054EB0.)

### 2.2 MmInitSystem (0x8001EA91; 4627 0x8001E88A, identical values) — details in §5
1. PDE 0x000–0x1FF ← 0; PDE 0x3FF ← 0.
2. PDE 0x3C0–0x3DF ← 0xF00000EB + n × 4 MB (0xF0000000–0xF7FFFFFF, 4 MB, PWT).
3. PDE 0x3E0–0x3FE ← 0xF80000FB + n × 4 MB (0xF8000000–**0xFFBFFFFF**, 31 entries, PCD|PWT). Specs 02 §4 and 09 say
   30 entries ending at 0xFF7FFFFF; both kernels include PDE 0x3FE (loop bound 0xC0300FF8 inclusive).
4. PCI read 0:0 reg 0x87 (byte). **Note:** `HalReadWritePCISpace` brackets every call with `cli` … `sti`, so it
   always returns with interrupts enabled.
5. Cold: page database / kernel-image PTEs (0x800550B5, §5.2). Quick: re-adopt persisted pages (0x8001FCAB).
6. PDE 0x200–0x20F ← 0x03FF0067 + n × 0x1000 (page tables at physical 0x03FF0000); PDE 0x210–0x23F ← 0.
7. CR3 reload (0x80018FA8) — the only TLB flush of the cold page-directory switch; no `invlpg`, no `wbinvd`.

### 2.3 Hardware detection (0x80054921, cold) — reads only
`MmMapIoSpace(0xFFF00078, 4, RW|NOCACHE)` (returns the physical address directly: inside the 0xF8000000 window, no
flush), reads that byte, port 0x80CE, NV2A 0xFD101000, PCI 0:1 reg 0x08, NV2A 0xFD000000 (spec 02 §5, spec 17 §1).

### 2.4 Phase 1 to the title (cold), hardware groups in order
| Stage | Hardware written | Where catalogued |
|---|---|---|
| RTC read, EEPROM/SMC (SMBus 0xC000–0xC009) | SMBus host registers | spec 12 |
| IDE / DVD (ports 0x1F0–0x1F7, 0x3F6, bus master 0xFF60–0xFF64) | §6.3 | spec 04 |
| AV pack / encoder / `AvSetDisplayMode` (GPIO 0x80D3, 0x80D6/0x80D8, NV2A display) | | specs 17–22 |
| Boot animation graphics device | NV2A PBUS 0x1804, **0x180C ← 0xF800**, PCRTC, PTIMER, PMC, PFB, PFIFO … (0x80045DBA–0x80045F60) | spec 22 §7 (5838 addresses: 0x80045DC8, 0x80045E2E, 0x80045E38) |
| Push-buffer jump | dword at virtual 0x80000000 (physical 0) ← jump command, then **`wbinvd`** (0x80041863) so the GPU fetches it | §2.7 |
| Boot sound (INIT 0x8004D574–0x80052C3F) | APU 0xFE800000–0xFE85FFFF (voice processor, global processor, DMA bases) and AC'97 0xFEC00000 (0x12C, 0x130, 0x17C), all by immediate address; APU 0xFE801000 ← 0xFFFFFFFF on teardown (0x80052907, 0x80052A39) | not catalogued register by register |
| Field parity GPIO | port 0x80C0 (0x800463BA) | spec 22 §7 step 10 |
| SMC "booted" | SMBus 0x20 cmd 0x1A ← 1 | spec 03 §5 |
| INIT discard (0x8001EB79, called at 0x80015A60) | §2.8 | |
| Title entry | `call [0x00010128]` (0x80015A6A) | spec 05 |

### 2.7 Push-buffer jump and its flush (0x800414C0, graphics device creation)
The library writes the GPU's start-of-push-buffer jump into physical page 0 through the cached 0x80000000 mapping
(write-back) and executes `wbinvd` immediately afterwards (4627: 0x80041FC3). This is the only place in the boot path
where the kernel flushes caches for a device's sake without changing a mapping.

### 2.8 INIT discard and code-segment limit (0x8001EB79; 5838 only)
1. Free every page of the last section (INIT) from its page-rounded start (0x8003D000) to its end; one free per page,
   then CR3 reload (0x80018FA8). The page 0x8003C000, shared with STICKY, is kept.
2. **5838 only:** rewrite GDT entry 0x08 (0x8003C2A0/0x8003C2A4, interrupts masked) as base 0, limit 0x8003C
   pages (G=1 → **limit 0x8003CFFF**), type 0x9B, D=1; reload CS with a far jump (0x8001FF28). From then on an
   instruction fetch above 0x8003CFFF (pool, stacks, discarded INIT, 0xF0000000+) faults with #GP; titles run at
   0x00010000+ and are unaffected. 4627 has no such step (CS stays 0xBFFFFFFF).
3. EvoX M8 disables both: 0x8001FF28 becomes `ret 8`, and the call at 0x80015A60 jumps to EvoX code (0x800B2540) that
   copies 0x7FFC bytes of its own code to 0x8003C3C4, XOR-decodes 0x200 bytes at 0x8003EFC0 and jumps to
   0x8003C410 — code written by plain stores and executed at once, with no serializing instruction other than the
   jump. INIT is not freed. EvoX's CS limit therefore stays 0xBFFFFFFF (matches the reference dump).

### 2.9 Quick reboot (0x8001B18C)
Interrupts off, ESP 0x8003A5F0, then 0x8001ECFB: PTEs of pages 0x3FE0–0x3FEF ← frame \| 0x47B; PDE 0x000–0x1FF and
0x340–0x3BF ← 0; PDE 0x200–0x23F ← 0xE3 + n × 4 MB (back to the 2BL's large pages at 0x80000000); PDE 0x3FF ←
0xFFC000E3 (flash, no PCD/PWT); CR3 reload. Then `.data` is re-copied from flash (header 0x28–0x34) and start-up
resumes at the IDT step (spec 02 §1). The 4 KB tables are rebuilt by §2.2.

## 3. PCI configuration (item 2)
### 3.1 Every PCI write (only `HalReadWritePCISpace`; callers 0x80062E24 ×29, MmInit ×1 read, detection ×1 read)
"Full" = the whole 256-byte space read, edited, written back as 64 dword cycles (every register rewritten with its
current value; write-one-to-clear status bits get cleared).
| # | Dev | Access | Value |
|---|---|---|---|
| 1 | 0:1 | 0x6A byte | 0x03 |
| 2 | 0:1 | 0x6C | 0x0E065491 |
| 3 | 0:1 | 0x64 | 0x00000B0C |
| 4 | 0:1 | 0x81 byte | \|= 0x08 |
| 5 | 0:9 (IDE) | full | command \|= 5; prog-if &= 0xFA; 0x20 ← 0xFF61; 0x50 ← 2; 0x58 ← 0x20202020; 0x60 ← 0xC0C0C0C0 |
| 6 | 0:4 (NIC) | full | command \|= 7; 0x10 ← 0xFEF00000; 0x14 ← 0xE001; 0x3C ← 4; 0x3D ← 0 |
| 7 | 0:2 (USB0) | full; then 0x50 | command \|= 7; 0x10 ← 0xFED00000; 0x3C ← 1; 0x3D ← 0; then 0x50 ← 0x0F |
| 8 | 0:3 (USB1) | full; then 0x50 | command \|= 7; 0x10 ← 0xFED08000; 0x3C ← 9; 0x3D ← 0; then 0x50 ← 0x30 |
| 9 | 0:6 (AC'97) | full | command \|= 7; 0x10 ← 0xD001; 0x14 ← 0xD201; 0x18 ← 0xFEC00000; 0x3C ← 6; 0x3D ← 0 |
| 10 | 0:5 (APU) | full | command \|= 7; 0x10 ← 0xFE800000; 0x3C ← 5; 0x3D ← 0 |
| 11 | 0:1 | 0x8C RMW | bit 26 cleared, bit 27 set |
| — | port 0x80CD | out | 0x08 |
| 12 | 0:6 | 0x4C RMW | upper half \|= 0x0101 |
| 13 | 0:0x1E (AGP bridge) | full | command \|= 7; 0x18/0x19/0x1A ← 0/1/1; 0x20 ← 0xFD00; 0x22 ← 0xFE70; 0x24 ← 0xF000; 0x26 ← 0xF3F0 \| ((HwFlags & 8) << 7) |
| 14 | 0:0 | 0x48 | 0x00000114 |
| 15 | 1:0 (NV2A) | full | command \|= 7; 0x3C ← 3; 0x3D ← 1 |
| 16 | 1:0 | 0x4C | 0x00000114 |

### 3.2 Answers
- **Cache-line size (0x0C) and latency timer (0x0D):** no configuration-cycle write sets them for any device; the
  "full" write-backs (rows 5–10, 13, 15) rewrite them with the values read.
- **NV2A latency timer 0xF8:** set by the kernel's graphics library when the boot animation creates its device:
  NV2A MMIO 0xFD00180C (PBUS mirror of PCI dword 0x0C) ← 0x0000F800, i.e. cache-line 0, **latency 0xF8**, at
  0x80045E38 (4627: 0x80046598), right after 0xFD001830 ← 0 and after 0xFD001804 \|= 4 (bus master). Every title's
  graphics library repeats it. The HAL write-back of 1:0 (row 15) runs earlier, at HAL phase 0.
- **Host bridge 0:0 regs 0x94 and 0xA4, ISA bridge 0:1 reg 0xBC:** never written by the 5838 or 4627 kernel (none
  of the 31 helper calls targets them; no other 0xCF8 access), nor by EvoX's added code (its port helpers at
  0x800B18B8–0x800B18E2 serve SMBus 0xC000 only), nor by the retail 5838 / EvoX X-code tables or the EvoX loader. A
  difference there between two runs on one boot chain must come from hardware-maintained (status) bits or from
  OXOS's own writes.

## 4. NV2A PBUS 0x1200–0x1264 and PFB (item 3)
- **The 5838 kernel contains no reference to 0x1240 or any of 0x1200–0x1264 except two reads of 0x1218** (bit 8,
  graphics library: 0x80047BA9, 0x80048EBD; spec 22 §7 step 6). Neither the constant 0x1240 nor 0xFD001240 occurs
  anywhere in the image. 4627: same. EvoX's added code: none.
- The pad registers 0x122C/0x123C/0x1230/**0x1240**/0x1234/0x1244/0x1238/0x1248/0x1214 are written only by a 2BL
  (spec 21 §3). The EvoX loader is such a 2BL replacement: calibration measurement at 0x400857–0x400939
  (0x1220 ← 0x000F0F0F, selector 0x1228 ← 0x40004…0x40007 and 0x40000…0x40003, reads 0x121C, 0x1220 ← 0) and the
  nine-register write list at 0x400C88–0x400D4D (same order as spec 21 §3); a second copy at 0x40291D.
- So 0x1240 = 0x8B8B8B8B in a reference run can only have been written before the kernel ran (by whatever 2BL/loader
  ran in that chain); the kernel never changes it afterwards. 0xFFFFFFFF in the OXOS run means nothing in OXOS's
  chain wrote the pad registers (or wrote all-ones).
- PFB: the kernel never writes 0x100200/0x100204 except the graphics library's save/restore (spec 22 §6.3); it clears
  0x100214 bit 0. AV and display-mode code touch no PBUS/PFB register (spec 22 §1).

## 5. Memory types and paging (item 4)
### 5.1 Effective types
PAT (set by the 2BL, never changed by the kernel) = 0x0007010600070106: entry 0 WB, **1 WC**, 2 UC-, 3 UC.
PWT → WC, PCD → UC-, PCD|PWT → UC. MTRRs (2BL): default UC; 0–64 MB WB (5838 2BL: 128 MB); flash top 512 KB WP.
| Mapping | Entry | Bits | Type |
|---|---|---|---|
| 0x80000000–0x83FFFFFF RAM, 4 KB PTEs at 0x83FF0000 (the PTE array is also the page database) | per page | see 5.2 | WB unless allocated with NOCACHE/WC |
| PDE 0x200–0x20F | 0x03FF0067 + n·4 KB | P, RW, **U**, A, D | — |
| 0xF0000000–0xF7FFFFFF | PDE 0x3C0–0x3DF, 4 MB | 0xEB: P RW PWT A D PS | **WC** (the GPU's view of RAM / AGP window) |
| 0xF8000000–0xFFBFFFFF | PDE 0x3E0–0x3FE, 4 MB | 0xFB: P RW PWT PCD A D PS | UC (MMIO; flash mirror at 0xFF000000) |
| flash 0xFFC00000 | PDE 0x3FF | cleared by MmInit; quick reboot sets 0xFFC000E3; a 5838 helper (0x8001E780) maps flash at 0xC0C00000 with PDE 0x303 ← 0xFFC000FB (UC) once, to read the AV blob | — |
| 0xD0000000–0xEFFFFFFF | system PTEs (pool, stacks, MmMapIoSpace) | per request | |

### 5.2 Kernel image and fixed pages (0x800550B5, cold)
| Pages (frame) | PTE | Note |
|---|---|---|
| 0x00 | 0x663 | page 0 (end-of-allocation bit 9 + bit 10) |
| 0x0F (old page directory) | 0xF663 | |
| 0x10 … end of image | frame \| 0x463 (P, RW, A, D, bit 10 = persist) | kernel image, `.data`, BSS |
| 0x10 … 0x35 | **RW cleared** | PE header page through the last page wholly inside `.text` (VirtualSize 0x264F0 counted from 0x10000): **`.text` is read-only**; CR0.WP is set (0x80010031), so a kernel store into `.text` faults |
| 0x3FE0–0x3FEF | frame \| 0x7B | GPU instance memory, UC; no flush when this type is set at cold boot |
| 0x3FF0–0x3FFF | frame \| 0x463 | the page tables, WB |
4627: identical constants (0x8005585C–0x800558F3). No large page maps the kernel after MmInit; no G bit anywhere.
Stacks (`MmCreateKernelStack`, via 0x8001E7BB) are 4 KB PTEs, P|RW|A|D (0x63) plus a not-present guard entry.

### 5.3 What shares a page / line with code
| Page | Code | Written data | Separation |
|---|---|---|---|
| 0x80035000 | last .text function 0x80035740 | none (rest is read-only data) | — |
| 0x80036000 | none (`.text` tail 0x80036000–0x800367AF is read-only data) | IDE PRD table IDEXPRDT 0x800367C0, `.data` from 0x800368E0 (IDT at 0x80036A50) | page is RW |
| **0x8003C000** | INIT code from 0x8003C3E0 (executed at boot; INIT section starts 0x8003C3C0) | STICKY up to 0x8003C3A3: TSSs 0x8003C160–0x8003C297, GDT 0x8003C298 (fixed up at boot, rewritten in §2.8), keys 0x8003C140, flags | never the same 32- or 64-byte line (last data line 0x8003C380–0x8003C3BF, code from 0x8003C3C0) |
The stock image never places written data and executed code in one 32-byte line. EvoX keeps the same layout but marks
STICKY executable and stretches it to 0x800443C0 (its resident code region, rewritten by §2.8 step 3).

### 5.4 TLB and cache flushes
| Helper / site | Operation | Called from |
|---|---|---|
| 0x80018FA8 | CR3 reload | MmInit after the PDE rewrite (0x8001EB54); INIT discard (0x8001EBFC); quick-reboot PD restore (0x8001ED5F); `NtFreeVirtualMemory` when a page table was freed (0x8001F794) |
| 0x80018FB0 | **CR3 reload, then `wbinvd`** | `MmAllocateContiguousMemoryEx` (0x8001DFBB), `MmMapIoSpace` (0x8001E13C), system-memory/stack mapper 0x8001E7BB (0x8001E88E), `MmSetAddressProtect` (0x8001E9D2), `NtProtectVirtualMemory` (0x8001F09B), `NtAllocateVirtualMemory` (0x8001F49E) — each **only when the protection includes PAGE_NOCACHE or PAGE_WRITECOMBINE (mask 0x600)**, and always **after** the new PTEs are written; page relocation 0x8001FD74 (0x8001FEB6) when the moved page's PTE had PWT |
| `invlpg` per page | single-page invalidation | `MmSetAddressProtect` (each PTE whose P/RW/PWT/PCD bits change; mask 0x1B), `MmFreeContiguousMemory` (0x8001E527), unmap of system memory / I/O space / stacks (0x8001E904, 0x8001E930), `MmClaimGpuInstanceMemory` (0x8001E49F), `NtProtectVirtualMemory` (0x8001F085), `NtFreeVirtualMemory` (0x8001F722), page copy 0x8001FED4/0x8001FF17 (CR3 reload instead when the page is a page table), file cache 0x8001D56F |
| 0x80041863 | `wbinvd` | §2.7 |
- Special cases of `MmMapIoSpace`: RW|NOCACHE inside 0xF8000000–0xFFBFFFFF returns the address with no flush;
  RW|WRITECOMBINE inside 0xF0000000–0xF7FFFFFF returns the address **after** the CR3+`wbinvd` flush.
- `MmPersistContiguousMemory` only toggles PTE bit 10: no TLB or cache flush.
- Freeing a NOCACHE/WC allocation flushes nothing (no stale lines can exist for an uncached page).
- Cold page-directory switch (§2.2): no `wbinvd`, no `invlpg`; one CR3 reload after all PDEs are written.
- No MTRR is ever changed, so no MTRR-change flush sequence exists in the kernel.

## 6. CPU control state (item 5)
### 6.1 CR0.MP
The kernel keeps MP and TS together: thread field NpxState (+0x30) is 0 (FPU state loaded) or **0x0A = TS|MP**
(not loaded; set at thread creation, 0x8001C56B).
- Context switch (0x80018F30): new CR0 = (CR0 & ~0x0E) \| new thread's NpxState \| FXSAVE-area word +0x200; written
  only if it differs (0x80018F97).
- FPU state saved / thread gives up the FPU (0x8001B7BA, 0x8001BAD2, 0x8001BBB9): CR0 \|= 0x0A.
- #NM handler (0x8001B71D) and `KeSaveFloatingPointState` (0x8001AD15): clear MP, EM, TS (& ~0x0E), restore, then
  OR in the +0x200 word.
So the reference's CR0 = 0x8001003B is simply "current thread does not own the FPU". With MP=1, TS=1 a `wait`/`fwait`
also raises #NM. OXOS toggles TS only (kernel/src/ke/npx.cpp).

### 6.2 Machine checks
Never enabled: no CR4 write, no MSR 0x17B or 0x400–0x413 access (5838, 4627, EvoX).

### 6.3 IA32_DEBUGCTL = 1
Not written by any of the three kernels or by EvoX's added code (no `wrmsr` other than §0). The EvoX loader does not
write it either (its `wrmsr` targets are 0x2FF, 0x200–0x20F, 0x277). Origin of the value 1 in the reference dump:
**open** (boot chain, the probe, or hardware); check DEBUGCTL at kernel entry in both runs.

### 6.4 Code-segment limit
0xBFFFFFFF from the GDT image; 5838 retail shrinks it to 0x8003CFFF before the title (§2.8). Limit checks only
fault; they do not change caching or timing. A 0xFFFFFFFF limit (OXOS) is harmless as far as the hardware is
concerned; the retail shrink would turn a stray jump into kernel data into an immediate #GP.

## 7. Cache-sensitive paths (item 6)
- **Interrupt dispatch** (0x80015074–0x80015148): specific EOI to the 8259s *before* the handler (master 0x60 \| irq,
  or slave 0x60 \| (irq−8) then master 0x62), spurious check by OCW3 0x0B and an ISR read with `jmp $+2` between;
  mask updates in 0x21/0xA1, ELCR 0x4D0/0x4D1 in `HalEnableSystemInterrupt`. No serializing instruction, no delay.
- **I/O delays:** `jmp $+2` between consecutive PIC, CMOS, PIT and Super-I/O accesses only; no port-0x80 delays.
- **IDE bus-master DMA** (0x80024BF1, 0x80024520): PRD entries are written with plain stores into IDEXPRDT
  (0x800367C0, write-back memory); table address (physical) → 0xFF64, status ← 6 (0xFF62), task file, command
  0xC8/0xCA, then 0xFF60 ← 9 (read) or 1 (write). No `wbinvd`, no locked op, no fence: coherency relies on bus-master
  snooping. Completion stops the engine with 0xFF60 ← 0.
- **Display-mode set-up:** only the waits of spec 22 §8.1 (vertical-blank polling, stalls via 0x80015530 with its
  leading `cpuid`).
- **Context switch:** the only serializing event is the CR0 write when MP/TS change.
- **Graphics device creation:** the single `wbinvd` of §2.7.
- **Self-modifying code:** the stock kernel never writes code it later executes; EvoX does (§2.8 step 3).

## 8. Code layout (item 7)
Measured over the code ranges above (96 147 instructions) and OXOS `exp-nv` `.text` code part
(0x80010220–0x80040774, 71 812 instructions).
| Property | Stock 5838 | OXOS exp-nv |
|---|---|---|
| Multi-byte NOP 0F 1F, `cmov`, `ud2` | none | none |
| `tzcnt` (F3 0F BC; executes as `bsf` on a Pentium III) | none | 1 (0x8001534D) |
| Padding between functions | mostly none: 603 of 1597 entries at odd addresses, 394 16-byte aligned; gaps filled with 0x90 or 0xCC runs (0–8 bytes) | 772 of 834 entries 16-byte aligned; 0x90 runs (10 838 single-byte NOPs) |
| Loop alignment filler | `lea esp,[esp+0]` / `lea ebx,[ebx+0]` multi-byte forms (65) and `mov edi,edi` (53) | 0x90 runs |
| `lock` prefix | 3 real | 67 |
| SSE / x87 | SSE and x87 in the INIT graphics/animation code | none |
| `pause` | none in .text | 3 |
| Read-only data in code pages | `.text` mixes code with read-only tables at its start and end | `.rdata` merged after code; trap/IRQ stubs at 0x80048CA0/0x80048D20 sit after `.rdata` |
| Written data next to code | §5.3: only page 0x8003C000, never one line | `.data` starts 0x8004AD60, two pages after the last code |

## 9. EvoX M8 against retail 5838 (hardware-relevant patches only)
`.text` differences: 0x80013D04 (jump to EvoX code), 0x8001598D (EEPROM-key retry routine 0x800B1466 instead of
0x800549C8), 0x80015A60 (§2.8), 0x8001C748 → `ret`, 0x8001FF28 → `ret 8`, AV/XBE area 0x8002EF6D–0x8002F70D and
0x800308E5/0x800308FA, path strings. Added code 0x800A8C92–0x800BA530: SMBus helpers and an own IDE PIO path (ports
0x1F2–0x1F7); no PCI, MSR, CR or PBUS write. Section STICKY is made executable and extended over the first 32 KB of
INIT. The EvoX loader (a 2BL replacement) sets MTRRs with caches disabled (CR0 \|= CD\|NW, `wbinvd`, MTRR writes:
default UC enabled, 0–64 MB WB, 0xFFF80000 512 KB WP, **0x80000000 16 KB write-through**, others 0), then PAT
0x0007010600070106 between two `wbinvd`s, CR4 \|= 0x610, page directory at 0xF000 as spec 01 §4, CR0 \|= 0x80010020,
and performs the NV2A pad calibration (§4).

## Open questions
1. Origin of PBUS 0x1240 = 0x8B8B8B8B and IA32_DEBUGCTL = 1 in the reference run: neither is written by the
   kernel; dump both at kernel entry under each chain.
2. Host 0x94/0xA4 and ISA 0xBC: never written by the kernels, X-codes or EvoX loader; which bits are status
   (hardware-set, write-one-to-clear) and what sets them at run time.
3. NV2A latency timer 0 in the OXOS dump although OXOS has the 0x180C write: whether that write runs in the measured
   build and whether a later write-back of 1:0's space (or a reset) undoes it.
4. Exact semantics of PBUS 0x1830 (zeroed by the library) and whether the 0xF8 latency timer affects NV2A DMA
   behaviour on the internal bus.
5. What physical 0xF0000000–0xF7FFFFFF decodes to (GPU view of RAM through the AGP window is inferred from spec 22
   §6.2); the stock kernel keeps a WC alias of RAM there permanently.
6. The 16 KB write-through MTRR at physical 0x80000000 in the EvoX loader (no RAM there; purpose unknown).
7. The boot sound's APU/AC'97 register sequence (INIT 0x8004D574–0x80052C3F) is not catalogued.
8. Whether the 0x3FE0–0x3FEF UC PTEs set at cold boot without a cache flush could leave stale lines from the 2BL's
   write-back use of that range (the stock kernel does not flush there).
