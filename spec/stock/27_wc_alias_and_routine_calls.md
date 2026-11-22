# Spec 27 — Write-combined aliases in the memory manager, and how the kernel calls routines a title supplies (xboxkrnl 1.0.5838.1; 4627 compared)

Clean-room behavioural spec, research stage. Sources: Ghidra 12.1.4 analysis (REA) and objdump of the shipped binaries `5838-unpacked/xboxkrnl.exe` (primary;
sha256 20adec79555237362a6df1838f69a12a1f7a4b614b099b6fa49889e0e96c1dde) and `stock4627_retail_unpacked/xboxkrnl.exe` (sha256
6bf7aecdbea49ed67a44214366520b56649f6b395b75ff346d7dd3d7115c407f). Addresses are 5838 unless marked. No code is reproduced. It extends spec 09 (memory
manager: page database, PTE array at 0x83FF0000, protections), spec 06 (IRQL, interrupts, DPCs, timers), spec 08 (APCs) and spec 11 (I/O manager); what
those say is not repeated. **Status: implemented** (2026-11-22).

Background: a title calls `MmFreeContiguousMemory` with the write-combined alias (0xF3FAE000) of a contiguous allocation whose kernel address is
0x83FAE000, and one of its DPC routines returns without removing its four stdcall arguments. Part A is the address handling of the `Mm*` exports, part B how
each kernel caller of a title-supplied routine reacts to a routine that leaves its arguments on the stack ("cdecl return" below).

## 0. What was searched
Exports were found from the export table by ordinal (each ordinal checked against the routine's behaviour), then followed through their callees and
callers; every indirect call in .text 0x80012BA0–0x800357FF (63 sites) was listed and the ones that reach a caller-supplied routine were classified. Names
in brackets are descriptive, not symbols (the PDB is not available).
| Item | 5838 | 4627 |
|---|---|---|
| `MmClaimGpuInstanceMemory` (ord 168) | 0x8001E417 | 0x8001E261 |
| `MmFreeContiguousMemory` (171) | 0x8001E4E7 | 0x8001E331 |
| `MmGetPhysicalAddress` (173) | 0x8001DDD4 | 0x8001DC1E |
| `MmIsAddressValid` (174) | 0x8001E54A | 0x8001E394 |
| `MmLockUnlockBufferPages` (175) | 0x8001E19D | 0x8001DFE7 |
| `MmLockUnlockPhysicalPage` (176) | 0x8001E22C | 0x8001E076 |
| `MmMapIoSpace` (177) | 0x8001E081 | 0x8001DECB |
| `MmPersistContiguousMemory` (178) | 0x8001E021 | 0x8001DE6B |
| `MmQueryAddressProtect` (179) | 0x8001E69E | 0x8001E4E6 |
| `MmQueryAllocationSize` (180) | 0x8001DFE4 | 0x8001DE2E |
| `MmSetAddressProtect` (182) | 0x8001E962 | 0x8001E75B |
| [protection → PTE bits] / [PTE bits → protection] | 0x8001E622 / 0x8001E666 | 0x8001E46A / 0x8001E4AE |
| [insert a page into its colour's free list; flag: head or tail] | 0x8001FAD8 | 0x8001F899 |
| [I/O segment-array page lock] (from `NtReadFileScatter` 220 0x800177B2, `NtWriteFileGather` 237 0x80017B89) / [its unlock] (from `IofCompleteRequest`) | 0x8001E26A / 0x8001E36C | 0x8001E0B4 / 0x8001E1B6 |
| [page-directory set-up in MmInitSystem: 0xF0000000 and 0xF8000000 windows, 0x80000000 page tables] | 0x8001EAAA–0x8001EB52 | 0x8001E8A3–… |
| `KeRaiseIrqlToDpcLevel` (129) / `KfLowerIrql` (161) | 0x80013D04 / 0x80013D24 | 0x80014348 / 0x80014368 |
| `KeInsertQueueDpc` (119); DPC list head (PRCB) | 0x8001A5B7; 0x800370DC | 0x8001A3C3; — |
| [DPC drain loop] | 0x8001B283 | 0x8001B08F |
| [DISPATCH_LEVEL software interrupt: switch to the DPC stack, drain] | 0x80018EA0 | 0x80018D30 |
| [idle loops that drain] | 0x8001B21A, 0x8001B25C | 0x8001B03E, 0x8001B088 (call sites) |
| [timer expiration: calls expired timers' DPC routines directly] | 0x8001C2FD | 0x8001C10D |
| [kernel's own timer DPC routine, which calls the above] / [second caller] | 0x8001C48A / 0x8001A17F | 0x8001C29B / 0x8001A0D7 (call site) |
| `KeInitializeInterrupt` (109); dispatch template copied into the KINTERRUPT; latched / level dispatchers | 0x800198CB; 0x8001B0A6 (0x58 bytes); 0x8001B050 / 0x8001B080 | 0x800195F0; 0x8001AEB6; 0x8001AE60 / 0x8001AE90 |
| [per-vector begin-interrupt table] / [interrupt exit] | 0x80036A00 / 0x80013D78, 0x80013D88, 0x80013DA0 | 0x80035A2C / 0x800143BC, 0x800143CC |
| `KeSynchronizeExecution` (153) | 0x8001B020 | 0x8001AE30 |
| [kernel-mode APC delivery] / [user-mode APC delivery] | 0x8001BD2D / 0x8001BE1B | 0x8001BB3D / 0x8001BC2B |
| [APC_LEVEL software interrupt] | 0x80013B7C (delivery call 0x80013BC0) | delivery call 0x80014204 |
| [dispatcher unlock with APC check] / [thread switch for waits] (both deliver kernel APCs) | 0x80018D10 / 0x80018DA4 | 0x80018BA0 / … |
| `KeWaitForSingleObject` (159), `KeWaitForMultipleObjects` (158), `KeDelayExecutionThread` (99) | 0x80019ED9, 0x80019BD8, 0x80019A07 | 0x80019CDC, 0x800199DB, 0x8001980A |
| `IofCallDriver` (86) / `IofCompleteRequest` (87) | 0x80015AE9 / 0x8001660E | 0x80015959 / 0x8001647E |
| `HalRegisterShutdownNotification` (47) / `HalReturnToFirmware` (49); list head; quick reboot entry | 0x80015385 / 0x8001542D; 0x80036A40; 0x8001B18C | 0x800151F1 / 0x80015299 |
| `PsCreateSystemThreadEx` (255) / `PsSetCreateThreadNotifyRoutine` (257) / `PsTerminateSystemThread` (258); routine array (8 slots), count | 0x80021B56 / 0x80021CF6 / 0x80021DC0; 0x8003BCA0, 0x8003BCC0 | 0x8002190E / 0x80021AAE / 0x80021B78 |

**4627 against 5838:** every routine in the table was compared instruction by instruction with addresses masked out. The `Mm*` routines, the free-list
insert, the I/O segment lock/unlock, the DPC drain, the dispatch interrupt path, the interrupt dispatchers, `KeSynchronizeExecution`, `IofCallDriver`,
`HalReturnToFirmware`, the `Ps*` routines are identical. `IofCompleteRequest`, both APC delivery routines and the timer expiration differ only in register
allocation and instruction order (same stack handling, same frame pointer use). Everything below holds for both versions.

---------------------------------------------------------------------------------------------------------------------------------------------------------
# Part A — addresses in the 0xF0000000 window

## 1. The windows as the kernel builds them (MmInitSystem, 0x8001EAAA; 4627 0x8001E8A3)
| PDEs | VA range | Value written | Meaning |
|---|---|---|---|
| 0x200–0x20F | 0x80000000–0x83FFFFFF | 0x03FF0067 + i·0x1000 | the 16 page tables at physical 0x3FF0000 (spec 09 §2) |
| 0x210–0x23F | 0x84000000–0x8FFFFFFF | 0 | **not mapped** (64 MB kernel) |
| 0x3C0–0x3DF | 0xF0000000–0xF7FFFFFF | 0xF00000EB + i·0x400000 | 4 MB pages, VA = PA, flags present, writable, PWT, accessed, dirty, large |
| 0x3E0–0x3FE | 0xF8000000–0xFF7FFFFF | 0xF8000000 + i·0x400000, flags with PCD and PWT | uncached 4 MB pages, VA = PA |
| 0x3FF | 0xFFC00000– | 0 | cleared |

So a write-combined alias is the **physical** address 0xF0000000 + RAM offset used as a virtual address: 0xF3FAE000 is RAM page 0x3FAE seen through the
GPU's aperture, the same page as 0x83FAE000. Nothing in the memory manager records that a contiguous allocation has such an alias; titles (their graphics
library) form it themselves. `MmMapIoSpace` returns such addresses unchanged for PAGE_READWRITE|PAGE_WRITECOMBINE requests in 0xF0000000–0xF7FFFFFF
(spec 09 §4).

## 2. The address arithmetic the routines use (question 3)
There is no shared helper routine; five arithmetic patterns recur, written out in each routine.
| # | Pattern | Arithmetic | Used by |
|---|---|---|---|
| P1 | **Contiguous page number** | PFN = bits 12–27 of the address = (va >> 12) AND 0xFFFF. The top four bits are dropped, so 0x0xxxxxxx, 0x8xxxxxxx, 0xDxxxxxxx and 0xFxxxxxxx with the same bits 12–27 give the same PFN. | `MmFreeContiguousMemory` (always, no range test); the I/O segment lock (only for va in 0x80000000–0x8FFFFFFF, P3) |
| P2 | **PTE / PDE through the self-map** | PTE address = 0xC0000000 + ((va >> 10) AND 0x3FFFFC) = 0xC0000000 + 4·(va >> 12); PDE address = 0xC0300000 + ((va >> 20) AND 0xFFC) = 0xC0300000 + 4·(va >> 22) | all routines that take a virtual address except `MmFreeContiguousMemory` |
| P3 | **Kernel window test** | "va − 0x80000000 is at most 0x0FFFFFFF", i.e. va in 0x80000000–0x8FFFFFFF | `MmLockUnlockBufferPages` (skip the range), the I/O segment lock/unlock (use P1 instead of the PTE) |
| P4 | **Page database entry** | entry address = 0x83FF0000 + 4·PFN; "valid PFN" = PFN at most 0x3FFF | `MmFreeContiguousMemory` (no bound), `MmLockUnlockPhysicalPage`, `MmLockUnlockBufferPages`, the I/O segment lock/unlock (bounded) |
| P5 | **Kernel address of a PFN** | (PFN − 0x80000) shifted left 12 = 0x80000000 + PFN·0x1000 (modulo 2³²); used only for the TLB invalidation | `MmFreeContiguousMemory`, `MmClaimGpuInstanceMemory` |

**What P2 gives for a 0xF0000000-window address.** The PDE of such a VA is a 4 MB page, so the "PTE" P2 computes lies in the self-map page that the
processor reaches through PDE 0x300 and then the large PDE used as a 4 KB entry: the dword read is at **physical** 0xF0000000 + 4 MB·((va >> 22) − 0x3C0)
+ 4·((va >> 12) AND 0x3FF), i.e. ordinary RAM seen through the aperture. For 0xF3FAE000 that is physical 0xF3C00EB8 (RAM offset 0x03C00EB8, a page that
belongs to whoever owns RAM page 0x3C00). No fault occurs; the routine reads or writes an unrelated dword. Routines that look at the PDE first (bit 7 =
large page) avoid this; the others do not.

## 3. `MmFreeContiguousMemory(va)` (0x8001E4E7) — question 1
1. Raise IRQL to DISPATCH_LEVEL (0x80013D04).
2. PFN = P1(va). The low 12 bits of va are ignored; **no test of va at all** (not of the window, not for NULL, not for page alignment).
3. Loop, starting at that PFN:
   1. Read the page database entry 0x83FF0000 + 4·PFN (P4, **no bound**) and remember its bit 9 (last page of an allocation, spec 09 §2).
   2. Put the page on its colour's free list **at the tail** (0x8001FAD8, flag 0): the entry is overwritten with the free-list links whatever it held
      before (no check that it was present, contiguous, or locked); the colour region count (0x80036D78) and AvailablePages (0x8003BB6C) grow by 1.
   3. Invalidate the TLB entry of 0x80000000 + PFN·0x1000 (P5) — always the 0x80000000 alias, never the address the caller passed.
   4. Contiguous usage count (0x8003BB64, usage 9 of spec 09 §8) − 1; PFN + 1.
   5. Stop after the page whose remembered bit 9 was set.
4. Lower IRQL to the saved value. No return value, no status, **no bug check** anywhere in the routine.

| va passed | PFN | Outcome on stock |
|---|---|---|
| 0x80000000–0x83FFFFFF (the allocation's kernel address) | va − 0x80000000 >> 12 | frees the allocation (normal case) |
| **0xF0000000–0xF3FFFFFF (write-combined alias, e.g. 0xF3FAE000)** | **same PFN as the 0x80000000 alias (0x3FAE)** | **frees the allocation exactly as through its 0x80000000 address**; the TLB entry invalidated is the 0x80000000 one (the alias is a 4 MB page and is not invalidated) |
| 0x84000000–0x8FFFFFFF, 0xF4000000–0xF7FFFFFF, any va with bit 26 or 27 set | 0x4000–0xFFFF | step 3.1 reads at 0x84000000 or above, which is unmapped (§1): page fault at DISPATCH_LEVEL |
| anything else (title VM 0x00010000–0x7FFFFFFF, system PTE range 0xD0000000–0xEFFFFFFF, 0xF8000000+, NULL) | bits 12–27 of va | **silently frees unrelated physical pages**: the page named by bits 12–27 and the following ones until an entry happens to have bit 9 set (in a free or busy entry bit 9 is part of a link or a lock count), corrupting the free lists and the counters; NULL frees page 0 onwards |

So the stock kernel "accepts" the 0xF0000000 alias only because P1 drops the top four bits; it neither recognises the window nor rejects anything.

## 4. The other routines (question 2)
| Routine | How the address becomes a page / PTE | 0xF0000000-window address (e.g. 0xF3FAE000) | Other unrecognised addresses |
|---|---|---|---|
| `MmGetPhysicalAddress(va)` 0x8001DDD4 | PDE (P2); not present → 0. **Large (bit 7)** → (PDE AND 0xFFFFF000) + (va AND 0x3FFFFF). Else PTE (P2); not present → 0; else (PTE AND 0xFFFFF000) + (va AND 0xFFF). No IRQL change. | **accepted**: returns **0xF3FAE000**, the aperture address (VA = PA there), not the RAM address 0x03FAE000 | anything unmapped → 0 |
| `MmIsAddressValid(va)` 0x8001E54A | PDE not present → FALSE; large → TRUE; PTE not present → FALSE; PTE bit 7 clear → TRUE; PTE bit 7 set → FALSE only if bits 8–11 of (PTE address + 0x800) are all zero, else TRUE (a corner case for self-map addresses whose "PTE" is a large PDE; the kernel never sets bit 7 in a 4 KB PTE, spec 23 §0) | **TRUE** (large PDE present) | FALSE |
| `MmQueryAddressProtect(va)` 0x8001E69E | at DISPATCH_LEVEL. PDE not present → 0. Large PDE → convert the PDE. Else PTE: present → convert; not present → 0 if the PTE is 0 or va ≥ 0x7FFF0000, else convert. Conversion (0x8001E666): READWRITE 4 if bit 1 else READONLY 2; not present: with bit 9 → that \| PAGE_GUARD 0x100, without → NOACCESS 1; then bit 4 (PCD) → \| NOCACHE 0x200 (and stop), else bit 3 (PWT) → \| WRITECOMBINE 0x400 | **accepted**: PDE 0x3C0–0x3DF → **0x404** (READWRITE\|WRITECOMBINE); 0xF8000000+ → 0x204 | 0 |
| `MmQueryAllocationSize(va)` 0x8001DFE4 | at DISPATCH_LEVEL; PTE (P2) of va, count dwords from there until one has bit 9; return count·4096. No PDE test, no bound | **not recognised**: counts dwords of unrelated RAM at the aperture location of §2 until one has bit 9 → a meaningless size (no fault while the walk stays in mapped aperture pages) | any va with a present PDE: same kind of garbage; unmapped page table → page fault |
| `MmPersistContiguousMemory(va, size, persist)` 0x8001E021 | at DISPATCH_LEVEL; every PTE (P2) from va to va + size − 1: bit 10 ← persist (set or clear). No PDE test | **not recognised**: sets or clears bit 10 in dwords of unrelated RAM (silent corruption); the allocation's real entries (0x83FF0000 + 4·PFN) are **not** marked, so it is not kept across a quick reboot | same corruption / page fault |
| `MmSetAddressProtect(va, size, protect)` 0x8001E962 | protect → PTE bits (0x8001E622, spec 09 §3; invalid → return, nothing done); at DISPATCH_LEVEL each PTE (P2) in the range whose bits 0, 1, 3, 4 differ gets those four bits replaced and its VA invalidated in the TLB; afterwards a full cache flush (0x80018FB0) if protect has NOCACHE or WRITECOMBINE | **not recognised**: rewrites bits 0, 1, 3, 4 of unrelated RAM dwords; the 4 MB alias itself keeps its PDE flags | same corruption / page fault |
| `MmLockUnlockBufferPages(va, size, unlock)` 0x8001E19D | at DISPATCH_LEVEL. **Skipped entirely** when va is in 0x80000000–0x8FFFFFFF (P3) or the PDE of va has bit 7 (large; present bit not tested). Else for each PTE (P2) in the range: frame = PTE >> 12 (present bit not tested); if frame ≤ 0x3FFF the low word of its page database entry gains 2 (lock) or loses 2 (unlock) | **ignored** (large PDE) | a va whose PDE is not present (bit 7 clear) reads a PTE in an absent page table → page fault |
| `MmLockUnlockPhysicalPage(pa, unlock)` 0x8001E22C | at DISPATCH_LEVEL; PFN = pa >> 12 (no mask); read entry 0x83FF0000 + 4·PFN **first**, then: entry bit 0 clear and PFN ≤ 0x3FFF → low word ± 2; otherwise nothing | the argument is physical. **0xF3FAE000 (what `MmGetPhysicalAddress` returns for the alias)** gives PFN 0xF3FAE → the read at 0x842BFEB8 is unmapped → **page fault** | any pa ≥ 64 MB: page fault; pa < 64 MB of a present (contiguous, kernel) page: nothing |
| `MmClaimGpuInstanceMemory(size, *padding)` 0x8001E417 | takes no address. *padding = 0x10000; unless size = 0xFFFFFFFF: at DISPATCH_LEVEL frees PFNs from (0x3FF0 − old claim in pages) up to (0x3FF0 − new claim in pages) through the same free-list insert (**head**, flag 1), invalidating 0x80000000 + PFN·0x1000 (P5), contiguous count − 1 each; stores the new claim rounded up to 4 KB at 0x80036CC0. Returns 0x84000000 − padding = 0x83FF0000 | not applicable | not applicable |
| [I/O segment lock] 0x8001E26A / [unlock] 0x8001E36C (not exports; reached by `NtReadFileScatter` / `NtWriteFileGather` and `IofCompleteRequest`) | per segment address: in 0x80000000–0x8FFFFFFF (P3) → PFN by P1, no lock; else PFN = PTE (P2) >> 12 and, if ≤ 0x3FFF, lock count ± 2. When asked to, the lock also maps all segment pages contiguously in the system PTE range (each PTE = PFN·4096 \| 0x63) and records that address | not recognised: PTE read from unrelated RAM (§2) → a random PFN locked and mapped in place of the buffer | — |

**Summary:** only `MmGetPhysicalAddress`, `MmIsAddressValid` and `MmQueryAddressProtect` understand the 0xF0000000 window (they look at the 4 MB PDE);
`MmLockUnlockBufferPages` skips it; `MmFreeContiguousMemory` handles it correctly by accident of P1. `MmQueryAllocationSize`, `MmPersistContiguousMemory`,
`MmSetAddressProtect` and the I/O segment lock read or write unrelated memory for it, and `MmLockUnlockPhysicalPage` faults on the aperture physical address.
None of the routines bug-checks on a bad address itself; the only failures are page faults from unbounded reads.

---------------------------------------------------------------------------------------------------------------------------------------------------------
# Part B — routines supplied by a title that return without removing their arguments (question 4)

Terms: a "cdecl return" leaves the routine's arguments on the stack, so the stack pointer is lower by 4 × (argument count) than the caller expects. A
caller **survives** if it neither returns through a displaced return address nor restores state from displaced stack slots. In the reference two things
decide it: whether the caller keeps a frame pointer and its epilogue resets the stack pointer from it, and what the caller pops (relative to the stack
pointer) before that reset. The callee-saved registers EBX, ESI, EDI are popped before the frame pointer reset in every frame-pointer routine below, so they
come back holding leftover arguments.

## 5. Answers per caller
| # | Caller (5838) | Routine and arguments | Survives? | Why |
|---|---|---|---|---|
| 1 | **DPC drain loop** 0x8001B283 (for `KeInsertQueueDpc`; entered from the DISPATCH_LEVEL interrupt 0x80018EA0 on the DPC stack and from the idle loops) | DeferredRoutine(Dpc, DeferredContext, SystemArgument1, SystemArgument2) | **No** | no frame pointer; it records the stack pointer at its start (0x800370E4, "DPC routine active"), then for every queued DPC pushes 4 arguments and calls, never resetting the stack pointer; each cdecl routine lowers it by 16. When the list is empty it returns through the displaced stack: the return address taken is the **Dpc pointer** (the last argument pushed), so execution continues inside the KDPC object. The dispatch path's own stack restore (it saves the thread stack pointer on the DPC stack and reloads it after the drain) is never reached. |
| 1a | **Timer expiration** 0x8001C2FD (expired timers that carry a DPC — `KeSetTimer`/`KeSetTimerEx` with a Dpc; these DPCs are **not** queued but called directly, up to 16 per batch, at DISPATCH_LEVEL) | DeferredRoutine(Dpc, DeferredContext, current interrupt time low, high) | **Yes** | keeps a frame pointer; all its state (the batch array of {Dpc, routine, context} at frame − 0xDC, the count, the time) is frame-relative and the loop counters are registers, so the drift does not matter; its epilogue pops EBX/ESI/EDI from the displaced stack (wrong values) and then resets the stack pointer from the frame pointer, so its return is correct. Its callers (the kernel's own timer DPC routine 0x8001C48A — a 4-argument routine; 4627 0x8001C29B, whose address the kernel passes at 0x8001CD49 — and 0x8001A17F, not identified) pop their own EBX/ESI/EDI from their correct stack straight afterwards, so the wrong registers never escape, and the timer DPC returns to the drain loop with its 16 bytes properly removed. |
| 2 | **Interrupt dispatch** (code copied from 0x8001B0A6 into each KINTERRUPT, continuing at 0x8001B050 latched / 0x8001B080 level-sensitive) | ServiceRoutine(Interrupt, ServiceContext) | **No** | the begin-interrupt routine leaves the previous IRQL in a stack slot; after the service routine the dispatcher takes that slot off the stack (stack-pointer relative). With a cdecl return it takes the **Interrupt pointer** instead, and the exit path (0x80013D88 / 0x80013D78 → 0x80013DA0) stores it as the processor's IRQL (PCR 0x800370B0) and uses it as a table index for pending software interrupts; it also reloads the exception list (PCR 0x8003708C) and EAX from stack-pointer-relative slots that are now 8 bytes off. Only after that is the stack pointer reset from the trap frame's frame pointer, so the return to the interrupted code happens with a corrupt IRQL and exception list. |
| 3 | **Kernel-mode APC delivery** 0x8001BD2D | KernelRoutine(Apc, &NormalRoutine, &NormalContext, &SystemArgument1, &SystemArgument2); NormalRoutine(NormalContext, SystemArgument1, SystemArgument2) (kernel-mode normal APCs, IRQL lowered to PASSIVE for it) | **Yes for the stack**, registers corrupted | keeps a frame pointer, all locals frame-relative; epilogue pops EDI/ESI/EBX from the displaced stack (they receive leftover arguments, e.g. EDI = Apc pointer after a cdecl KernelRoutine) and then resets the stack pointer, so it returns correctly. Callers: the APC_LEVEL software interrupt (0x80013BC0) restores every register from its trap frame → **no effect**; the dispatcher unlock path (0x80018D10) restores its registers from its own save area → **no effect**; the wait thread switch (0x80018DA4) holds the waiting thread's **wait status in EDI** across the delivery and returns it → **the wait returns a wrong status** (the leftover argument). |
| 3a | **User-mode APC delivery** 0x8001BE1B (alertable user-mode waits: `KeWaitForSingleObject`, `KeWaitForMultipleObjects`, `KeDelayExecutionThread`; this is how a title's `NtQueueApcThread` routine and I/O completion routines run) | KernelRoutine (the kernel's own) with 5 arguments, then NormalRoutine(NormalContext, SystemArgument1, SystemArgument2) | **Yes for the stack**, result corrupted | same frame-pointer epilogue as #3: EDI/ESI/EBX come back as NormalContext / SystemArgument1 / SystemArgument2 of the last cdecl routine. All three wait routines keep the status 0xC0 (STATUS_USER_APC) in EBX or EDI across the call and return that register, so **the wait returns a leftover argument instead of 0xC0**; their own epilogues then restore the caller's registers correctly. |
| 4 | **`IofCallDriver`** 0x80015AE9 | MajorFunction[n](DeviceObject, Irp) | **No** | no frame pointer: it saves ESI, pushes the two arguments, calls, then pops ESI and returns, all stack-pointer relative. With a cdecl return ESI receives the DeviceObject and the return address taken is the **Irp pointer**. |
| 5 | **`IofCompleteRequest`** 0x8001660E | CompletionRoutine(DeviceObject, Irp, Context) for each stack location with a routine | **No** | no frame pointer: its saved registers and the priority-boost argument are stack-pointer relative; after a cdecl return the later read of the priority boost is 12 bytes off and the final register pops and return use displaced slots (return through a saved register value). Also true when the routine returns STATUS_MORE_PROCESSING_REQUIRED (0xC0000016), which goes to the same epilogue. |
| 6 | **`KeSynchronizeExecution`** 0x8001B020 | SynchronizeRoutine(SynchronizeContext), with the interrupt's IRQL raised | **Yes**, EBX corrupted | keeps a frame pointer; the result is moved out before the epilogue; EBX is popped from the displaced slot (**the caller's EBX becomes SynchronizeContext**), then the stack pointer is reset from the frame pointer and the return is correct. |
| 7 | **Shutdown notifications** in `HalReturnToFirmware` 0x8001542D (registrations from `HalRegisterShutdownNotification`, list 0x80036A40, run only for the quick-reboot routine 2; reset and power-off routines reset the machine first, 0x8001540C, and never run them) | NotificationRoutine(Registration) | **Yes** | the loop keeps its state in registers and never reads the stack; after the last routine the quick-reboot entry (0x8001B18C) **loads a fixed stack pointer (0x8003A5F0)** and never returns, so the drift is discarded. |
| 8 | **Thread create notifications** in `PsCreateSystemThreadEx` 0x80021B56 (routines from `PsSetCreateThreadNotifyRoutine`, 8 slots at 0x8003BCA0) | NotifyRoutine(Thread, ThreadId, TRUE) | **Yes**, ESI/EBX corrupted | keeps a frame pointer, everything after the loop is frame-relative and the status is moved out first; the epilogue pops ESI and EBX from displaced slots (**the caller's ESI = Thread object, EBX = ThreadId** after one cdecl routine), then resets the stack pointer and returns correctly. |
| 8a | **Thread exit notifications** in `PsTerminateSystemThread` 0x80021DC0 | NotifyRoutine(Thread, ThreadId, FALSE) | **Yes** (no return) | no frame pointer, loop state in registers; afterwards it reads its ExitStatus argument stack-pointer relative, so after cdecl routines the **thread's exit status (thread + 0x120) gets a wrong value** (with one routine: the saved ESI). The routine never returns, so nothing else is affected. |

**For the title (question B).** A DPC routine that returns cdecl survives on stock only when the kernel calls it from the timer expiration (#1a: a DPC
attached to a timer through `KeSetTimer`/`KeSetTimerEx`). The same routine queued with `KeInsertQueueDpc` would crash the stock kernel at the end of the
drain (#1). A kernel that calls timer DPCs through its DPC queue, or any DPC from code that does not reset its stack pointer from a frame pointer, breaks such
a title.

## 6. Details that matter for a re-implementation
| Item | Value / behaviour |
|---|---|
| Timer DPC arguments | SystemArgument1/2 = the interrupt time (low, high) read once at the start of the expiration pass (0x8003B880 / 0x8003B884, re-read until the high halves agree) — not the DPC's own SystemArgument fields; the DPC is not marked inserted and not put on the DPC list |
| Timer DPC batch | up to 16 expired timers are unlinked (periodic ones re-inserted with their period × −10000), their {Dpc, DeferredRoutine, DeferredContext} copied to a frame array, the dispatcher lock released to DISPATCH_LEVEL, then the routines called in order; if the batch was full the pass repeats |
| Queued DPC | `KeInsertQueueDpc` stores SystemArgument1/2 in the KDPC (+0x14, +0x18), marks it inserted (+0x2), appends it to 0x800370DC and requests the DISPATCH_LEVEL software interrupt; the drain clears "inserted" before calling, with interrupts enabled |
| KINTERRUPT fields used by the dispatcher | +0x00 ServiceRoutine, +0x04 ServiceContext, +0x08 bus level (vector − 0x30), +0x0C IRQL, +0x12 mode (latched → 0x8001B050), +0x18 dispatch code (0x58 bytes) |
| Shutdown registration | {+0 routine, +4 priority, +8 list entry}; inserted in descending priority; the routine gets the registration's address |

---------------------------------------------------------------------------------------------------------------------------------------------------------
## 7. What stays unknown
- What the reference does after the page faults of §3 and §4 (fault at DISPATCH_LEVEL in kernel mode): the trap 14 path and the bug-check code it ends in
  were not traced here (spec 14).
- What RAM the 0xF0000000–0xF7FFFFFF aperture shows above 64 MB on a retail console (0xF4000000–0xF7FFFFFF); §2's "unrelated RAM" holds for the first 64 MB.
- The memory type that PWT alone selects in the 0xF0000000 window depends on the PAT/MTRR values the 2BL leaves (spec 23 §0, §5.1); not re-checked here.
- `MmIsAddressValid`'s bit-7 corner case (§4) was not related to any caller; it does not affect 0x80000000 or 0xF0000000 addresses.
- Which DPC the title actually uses (queued or timer) is not something the kernel binaries can show; §5 gives the outcome of both.
- Kernel callers of caller-supplied routines not listed in the question were classified only as far as needed to find the DPC paths (e.g. the system thread
  start routine call 0x80021B3F, the object-type procedures, the file-system callbacks); they were not studied.
