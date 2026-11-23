# 49. Pause a title, and suspend it to disk to resume after a power-off (proposal)

Status: **proposal**, not implemented (user, 2026-11-23: "write a pause game up as a proposal spec along with a 'suspend/resume' feature where we can suspend
to disk, power off and when power comes back on, we could set everything back up the way it was and just resume the game"). Nothing here is decided until
the user says so.

Two features that build on each other:
- **Pause (§1):** a button combination freezes the running title in place, a toast says so, the same combination lets it go on. Everything stays in memory.
- **Suspend to disk (§2):** a paused title is written to the hard disk with the state of the hardware, the console powers off, and at the next power-on
  OXOS puts memory and hardware back as they were and the title goes on where it stopped. A hibernation of the whole console, not of one program: a title is
  its memory, the kernel's objects it owns, and the state of the GPU, the sound hardware and the USB devices it set up.

Pause is a small feature; suspend to disk is a research project whose outcome is not known (§2.6), to be proven in steps (§2.7) before anything is built.

## 1. Pause

### 1.1 What the user sees
- An in-game shortcut (spec 48) with a new action, `pause`, e.g. (commented out as shipped, the buttons the user's choice):
  ```ini
  ;[pause]
  ;buttons = back + white
  ;action = pause
  ;toast_message = PAUSED
  ```
- The first press freezes the title: the picture stays on screen, the sound stops, and a toast ("PAUSED") stays up. The same buttons again let the title go
  on and the toast goes. The title is not told; it simply does not run in between.

### 1.2 What the kernel does
Pause (a new export for extensions, `oxos_title_pause`, spec 34 §7.3's style: cdecl, the kernel's own function):
1. Every title thread suspended, as `titles::SwitchTitle` already does before a launch (spec 22 §9.3: threads started in title code, and the title's main
   thread). The suspend takes effect at each thread's next safe point (a suspend APC), so a thread inside a kernel call finishes it first.
2. The title's sound stopped where it is (§1.3).
3. Optionally the clocks the title reads frozen (§1.4).
Resume (`oxos_title_resume`): the clocks set going again, the sound restarted, the threads resumed, in that order. Both at passive level, from an extension's
run loop or callback (IGS's pad callback).

New toast calls for a toast that stays until taken down (spec 36): `Seconds = OXOS_TOAST_UNTIL_HIDDEN` and `oxos_toast_hide(ToastId)`.

### 1.3 Sound
Suspending threads does not stop the sound hardware: the APU goes on mixing its voices and the AC'97 controller goes on playing its buffers, so a paused title
would loop its last fraction of a second (a buzz) until resumed.
| Part | At pause | At resume |
|---|---|---|
| AC'97 PCM and S/PDIF output (CR 0xFEC0011B / 0xFEC0017B) | run bit cleared, the rest of the channel state kept (BDBAR, CIV, LVI) | the saved CR written back |
| APU (0xFE800000) | its output paused: to be found which register halts the voice processor or the global processor without losing their state (spec stock/25 §4 and the REA study of the stock audio bring-up name the global processor's reset and the counter mode; a reset loses state) | let run again |
| The codec | never touched: no reset, no register access (spec 46, and the 2026-11-23 sound loop: a hold and release of the codec's cold reset wedged it about one time in 13, and a codec register read with no codec answering hangs the console) | nothing |
A title's sound interrupt (APU, IRQ 5; AC'97, IRQ 6) may fire once more while it is being stopped; its routine runs in title code (§1.5).

### 1.4 Time
Games measure time with `KeTickCount`, `KeQueryPerformanceCounter` / `KeQueryInterruptTime` and the CPU's time-stamp counter. After a pause of two minutes
many see one frame two minutes long: physics jumps, timers fire at once, and network sessions (system link, online play) time out. Many games clamp the frame
time, so a pause that does nothing about time works for a good share of titles.
| Option | What | Cost and risk |
|---|---|---|
| A. Nothing | the title sees the jump | free; some titles misbehave after a long pause |
| B. Freeze the kernel's clocks | the clock interrupt stops advancing `KeTickCount` and the interrupt time while paused; the performance counter (the ACPI timer, spec stock/06) is read through an offset the pause adds to | the kernel's own timers (the run loop's 1 ms waits, the toast's time) must keep a time of their own meanwhile |
| C. B plus the time-stamp counter | the TSC written back at resume (WRMSR 0x10) minus the paused time | a title reading the TSC while its threads are suspended does not exist; the kernel's own uses of the TSC must allow for the jump back |
Proposed: A first (with the pause and the toast), then B if titles need it.

### 1.5 What still runs while paused
- The title's interrupt routines (USB, APU, AC'97, GPU), DPCs and timer callbacks: they are title code but run outside its threads. Usually harmless (a pad
  report recorded, a vertical blank counted). A title whose DPC waits for a thread's answer could pile up work; to be watched in testing.
- The GPU finishes the commands already queued, then idles; the frame on screen stays (the CRTC keeps scanning it out).
- The kernel, the extensions' run loop (IGS sees the pads at interrupt level, spec 22 §9, so the resume combination works), the toast service.

### 1.6 Tests
- xemu: a test title that counts frames and plays a tone; pause through the export: the frame count stops, the PCM position stops, resume continues both.
- Consoles: XDK titles and nxdk titles paused for 5 s and for 5 minutes; sound silent while paused, no buzz; resume without a crash; the capture card's
  sound level per second (the method of the 2026-11-23 sound loop).

## 2. Suspend to disk, resume after a power-off

### 2.1 What the user sees
- An in-game shortcut with an action `suspend` (or a choice in the pause toast): the title is paused (§1), a toast says "Suspending ...", the console writes
  for a few seconds and powers off.
- At the next power-on, OXOS finds the saved title and asks (one line, docs/cpp_style.md §6): "Resume <title name>?  A: resume  B: discard". A puts the
  console back and the title goes on, paused, with its toast; the pause combination lets it go. B discards the save and boots as usual.
- Only one title is saved at a time. Any normal boot that does not resume it discards it (§2.5).

### 2.2 What a saved title is
| State | Where it lives | How it is saved / put back |
|---|---|---|
| Memory | RAM, 64 or 128 MB: the title's, the kernel's (its objects, handles, file caches, the title's kernel threads and their stacks), the extensions' | every page, raw |
| CPU | the registers of the thread that suspends, MTRRs, PAT, control registers, IDT/GDT (in RAM) | the suspending thread's context saved; the other threads are suspended, their contexts are on their stacks in RAM |
| GPU (NV2A) | PFIFO (the command buffer pointers), PGRAPH (the 3D state: shaders, combiners, textures, tiling, surface setup), PCRTC/PRAMDAC (the mode), the encoder | the mode set again from the saved mode word (`AvSetDisplayMode`); PGRAPH through its context save/load (the hardware saves a channel's graphics state to instance memory when it switches channels: to be proven on the NV2A, §2.6); PFIFO's pointers read and written back with the FIFO idle |
| APU | the voice processor's and the two DSPs' (GP, EP) registers and their memories | read through their MMIO windows while stopped, written back before restarting: to be proven |
| AC'97 controller and codec | the controller's channel registers; the codec's power and volume registers | after the power-on the codec is in its power-on reset: released once (as the stock kernel's cold boot does, never re-asserted), its registers written from the save, the channels restored |
| USB | the host controllers' registers and the title's own USB stack (in RAM: the device addresses and configurations it set) | the devices are new after the power-on: the kernel resets the ports and brings each device to the address and configuration the title's stack recorded (the descriptors and endpoints are read from the saved schedules, spec 22 §9.2), then the controllers' registers |
| Disk and disc | the IDE controllers, the disk's and the DVD drive's own state, the file systems' caches (in RAM) | the disk unchanged since the save (§2.5); the same disc in the drive (its identity checked), a disc image still where it was |
| Network | the NIC; open connections | the NIC set up again; online and system-link sessions are lost |
| Chipset, timers, interrupt controllers, SMC | PIC masks, PIT, RTC, SMC state | set again by the boot, then the saved values where they matter (PIC masks) |

### 2.3 Suspend, step by step
1. Pause (§1) with the clocks frozen (§1.4 B/C), so the title wakes up with no gap.
2. Quiesce: the GPU idle (FIFO empty, PGRAPH idle) and its context saved; the APU stopped and its state read; the AC'97 channels stopped; the USB controllers'
   lists stopped; the IDE controllers idle; the NIC's receiver off. Interrupts off from here.
3. Write: the device state, then every RAM page, into a file prepared beforehand whose sectors are known (`C:\OXOS\cache\oxos_suspend.bin`, the RAM size plus
   a header; its sectors contiguous, found once through the file system), by a polled IDE writer that keeps to a small area of memory it does not change
   while writing. The header last: a magic, a checksum of the image, the kernel's build, the console's EEPROM serial, the RAM size, the title's ID and name,
   the disc's identity.
4. Power off through the SMC.
64 MB at the disk's 20-30 MB/s is about 3 s; 128 MB about 6 s.

### 2.4 Resume, step by step
1. Cold boot as usual up to the point where the disk is readable (boot-only code, spec 18). A valid header from the same kernel build and the same console
   offers the resume (§2.1).
2. Resume: a loader that sits outside the RAM it restores (boot-only code in the BIOS image's boot area, or a reserved region the image leaves out) reads
   the image back into RAM with the polled reader.
3. The hardware set back (§2.2's right-hand column), in the order: chipset and interrupt controllers, mode set and encoder, GPU context and FIFO, APU, AC'97
   and codec, USB (devices re-addressed), IDE, NIC.
4. The CPU state restored; execution returns into the suspending thread at the point after step 3 of §2.3, which un-quiesces and resumes (§1.2), the
   clocks continuing from where they stopped.
5. The header invalidated, so a later crash or power-off does not resume the same image twice.

### 2.5 The disk
The saved RAM holds the file systems' caches and open files as they were. If anything writes to the disk between suspend and resume (a normal boot, a
dashboard, an FTP upload), the restored caches no longer match it and a resume would corrupt the disk. So:
- Any boot that does not resume invalidates the image before mounting the disk read-write.
- The image is written only to its own pre-allocated file; nothing else is written while suspending.
- Safe boot (B held) offers no resume and invalidates the image.

### 2.6 Feasibility: what is not known
| Question | Why it matters | How to find out |
|---|---|---|
| Can the NV2A's graphics context be saved and loaded on demand (the channel-switch mechanism), and is that all of PGRAPH's state a title relies on? | the picture and every following frame | §2.7 step 3 |
| Can the APU's DSP memories and registers be read and written back without a reset? | sound after resume | §2.7 step 4 |
| Does the codec come up reliably from its power-on reset and accept the saved registers? | sound; spec 46 and the 2026-11-23 loop show how fragile its resets are | §2.7 step 4, with the capture card |
| Can the USB devices be brought back to the addresses and configurations the title's stack recorded? | pads after resume | §2.7 step 5 |
| How much of the stock-kernel-built title state depends on hardware not listed in §2.2? | anything missed breaks the resume | xemu save states list the device models; compare |
| Memory for the writer and the loader | the writer runs while titles run (resident, or an extension `oxos_suspend.xos` loaded only when the feature is on); the loader is boot-only | the page budget (15 960 free pages) |

### 2.7 Proving it, in steps (each a throwaway pilot, never committed)
1. **RAM only, no power-off:** pause, write the RAM image, read it back over the live RAM, resume. Proves the writer, the reader and the pause.
2. **RAM changed in between:** as 1, with memory overwritten between write and read-back (a pattern), then resume. Proves the restore really restores.
3. **GPU:** as 2, with a mode set and some drawing between save and restore; the GPU context saved and loaded. The make-or-break step.
4. **Sound:** the APU and the codec through a reset between save and restore; the capture card's level before and after.
5. **USB:** the ports reset between save and restore; the pads work after resume.
6. **Power-off:** the whole of §2.3 and §2.4 across a real power cycle, on each test console and encoder.

### 2.8 Tests (once built)
- The steps of §2.7 as repeatable tests where xemu can run them (xemu has its own save states; OXOS's suspend is tested on its own image in xemu where the
  device models allow it).
- Consoles: XDK and nxdk titles suspended in play and resumed after a power-off; sound, pads, picture; a declined resume leaves the disk consistent (a disk
  check after it).

## 3. Open
- The pause and suspend buttons, and whether suspend is its own shortcut or a choice in the pause toast.
- Whether pause freezes time (§1.4) from the start.
- Suspend to disk at all: decided after §2.7 step 3.
