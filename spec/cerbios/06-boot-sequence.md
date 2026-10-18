# Cerbios spec 06: boot flags, start-up order, transfer modes

Studied in Cerbios 3.1.0 Beta: the kernel entry, the quick-reboot path, the extension's phase-0/phase-1 init.
Behaviour and formats only. Safe mode itself is in spec 01 §4.

## 1. Boot flags (kernel dword, cold boot)
- **From the boot command line** (string passed to the kernel entry, up to 64 characters, substring match):
  `SHADOW` → 0x01, `/HDBOOT` → 0x02, `/CDBOOT` → 0x04, `/DBBOOT` → 0x80.
- **From SMC register 0x1B** (the scratch register), read during start-up:

  | Scratch bit | Boot flag | Used for |
  |---|---|---|
  | 0x02 | 0x020 | |
  | 0x04 | 0x010 | skip the boot animation (spec 09 §2) |
  | 0x08 | 0x080 | |
  | 0x10 | 0x100 | second pass of the two-disk boot (spec 05 §3) |
  | 0x20 | 0x800 | safe mode (spec 01 §4) |
- Flag 0x08 records that SMC reset-on-eject has been switched off (SMC register 0x19 = 1); the title loader keeps
  it in step with the register (spec 13 §6).
- On a reboot request (HalReturnToFirmware, reboot) with flag 0x100 set, the scratch register is re-read and kept.

## 2. Start-up order
Cold boot and quick reboot run the same code; steps marked *cold* are skipped on a quick reboot (the kernel's
in-place restart between titles).

**Phase 0** (extension, before interrupts are enabled): print `Welcome to Cerbios v3.1.0`; file-system cache size 16
pages; MTRR default type write-back (MSR 0x2FF low byte = 6); object manager directories; create the phase-1 thread.
Before that, on a cold boot, the kernel loads CPU microcode for CPUID signatures 0x6B1, 0x6B4, 0x686 and 0x68A
(spec 12 §4).

**Phase 1** (extension, in the thread that ends by starting the title):
1. Install the kernel's interrupt handlers for IRQ 0, 8, 11 and 12; read the CPU identification.
2. *cold:* SMC and system set-up.
3. Reset the EEPROM user settings 0x0A, 0x0B and 0x0C to 0 if they are non-zero (the parental-control settings:
   games, password, movies), so parental control is always off.
4. *cold:* safe-mode check (spec 01 §4).
5. IDE: disk 0 device and partition table (spec 03, 05).
6. cerbios.ini, if not in safe mode and LoadConfig is set (spec 01 §2), then the clamps (spec 01 §3).
7. Slave drive / DRIVESETUP (spec 05).
8. Clocks, if not in safe mode and OVERCLOCKING is set (spec 12 §1); then the CPU speed measurement (spec 12 §3).
9. Fan, if OVERRIDEFAN (spec 11 §1).
10. *cold:* RTC, if RTCENABLE (spec 11 §4).
11. Virtual disc devices (spec 07).
12. *cold:* AV pack check, if AVCHECK (spec 14 §4).
13. DVD device (spec 05 §4).
14. *cold, not safe mode, LoadConfig set:* UDMA modes (§3).
15. Front LED from FRONTLED (spec 11 §2).
16. DEBUG: load the debug monitor (spec 14 §1).
17. Choose and load the XBE to start (spec 09).
18. *cold:* final SMC write (register 0x1A = 1), release the INIT section.
19. Move the launch-data page and the saved AV framebuffer out of the physical range the extension used.
20. In-app LCD thread, if INAPPLCDENABLE (spec 11 §3).
21. Graphics hooks, if ENABLESCREENSHOTS or INAPPLCDENABLE (spec 13 §5).
22. Title patches, if APPLYTITLEPATCHES (spec 13 §7).
23. *cold:* free the saved AV framebuffer.

The thread then removes the extension (spec 00) and jumps to the XBE entry point.

## 3. UDMA transfer modes
UDMAMODEMASTER and UDMAMODESLAVE (record +0xD8/+0xD9, default 2, safe mode 2) are sent to device 0 and, in
DRIVESETUP 3, device 1 with SET FEATURES (set transfer mode). A disk that fails at the chosen mode leads to a fatal
error on the next accesses; the error screen for that case offers safe mode, and safe mode restores mode 2
(spec 14 §3).

## Open
- Meaning of boot flags 0x20 and 0x80 beyond the command-line names.
- What SMC register 0x1A = 1 signals.
