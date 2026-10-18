# Cerbios spec 05: drive setup and the second hard disk

Studied in Cerbios 3.1.0 Beta: the extension's IDE start-up and device creation, the kernel's IDE probe, SET FEATURES
and SMC tray code. Behaviour and formats only.

## 1. DRIVESETUP (record +0x08, 0–3, clamped to 3)
| Value | Master (IDE device 0) | Slave (IDE device 1) | `\Device\CdRom0` |
|---|---|---|---|
| 0 | hard disk 0 | DVD drive, retail driver | the real DVD drive |
| 1 | hard disk 0 | DVD drive, retail driver | the real DVD drive |
| 2 | hard disk 0 | not probed (no DVD drive) | a placeholder device, no drive (§4) |
| 3 | hard disk 0 | **hard disk 1** | a placeholder device, no drive (§4) |

0 and 1 take the same path in the code studied (1 is the safe-mode value and the fallback, see below). The public
notes call 0 "HDD + DVD" and 1 "HDD only (legacy)", and in xemu a console with 1 never read the test disc, so the
difference lies in code not yet traced (spec 00, public information). Virtual disc
images (spec 07) can stand in for the DVD in every mode.

## 2. Probing the slave
- At cold boot (not on a quick reboot), unless DRIVESETUP is 2, the kernel sends **IDENTIFY PACKET DEVICE (0xA1)**
  to device 1 and keeps bit 15 of word 0 (the ATAPI bit) as a "slave is a DVD drive" flag. With DRIVESETUP 2 the
  flag is forced off.
- The flag also gates the SMC tray handling: tray events are only processed while a real DVD drive is present and no
  virtual DVD is mounted.

## 3. DRIVESETUP 3: two hard disks
- **Spin-up reboot:** on the first cold boot with DRIVESETUP 3, the extension writes **0x14 to SMC register 0x1B**
  (the scratch register; 0x10 = "second-disk pass", 0x04 = skip the boot animation) and **reboots once**
  (HalReturnToFirmware, reboot). On that second boot the scratch bit 0x10 gives boot flag 0x100 (spec 06 §1), and
  the boot continues. While that flag is set, the kernel's slave-ready check selects device 1 (0xB0 to the drive
  register) and treats it as ready when the status has BSY clear and DRDY set.
- If disk 1 does not become ready (the kernel polls it up to 900 times), DRIVESETUP falls back to 1 instead of a
  fatal error (disk 0 not becoming ready is fatal error 7). The kernel's device-present check selects each device
  (0xA0 / 0xB0 to the drive register) and counts it as present when status bit 5 is clear.
- If the slave turns out to be a DVD drive (ATAPI flag set), DRIVESETUP is changed to 1 and the boot proceeds as a
  normal one-disk console.
- Otherwise the slave is set up exactly like the master, as disk 1:
  - device `\Device\RealHD1` (sector size 512, size from IDENTIFY, 48-bit rules of spec 03 §1 per drive);
  - its own 512-byte partition table, with the same defaults, automatic F:/G: and on-disk override (spec 03 §2–3),
    read from `\Device\RealHD1\Partition0`;
  - the symbolic link `\Device\Harddisk1` → `\Device\RealHD1` (disk 0 likewise gets `\Device\Harddisk0` →
    `\Device\RealHD0`), so `\Device\Harddisk1\PartitionN` paths work;
  - the transfer mode is set with SET FEATURES from **UDMAMODESLAVE** (the master's from UDMAMODEMASTER). This is
    done only on a cold boot, when not in safe mode, with cerbios.ini loading enabled.
- Partition creation per disk is skipped on disk 1 whenever the slave is a DVD drive.

## 4. The placeholder CD device (DRIVESETUP 2 and 3)
- Instead of the retail DVD driver, a device `\Device\RealCD0` is created with its own driver object (read and
  device-control handlers only). Unless a virtual DVD is mounted, `\??\CdRom0:` and `\Device\CdRom0` are linked to it.
  It reports no media, so titles see an empty drive.
- With DRIVESETUP 0/1 on a quick reboot, the DVD device object is recreated with the retail driver and the same
  links (the hardware was initialised on the cold boot).

## 5. Paths that name disk 1
The path translator (spec 09 §1) maps `HDD1-<letter>:\` to `\Device\Harddisk1\Partition<n>\` with the letter table
of spec 03 §4 (C 2, E 1, F 6, G 7, H 8, I 9, J–N 10–14, X 3, Y 4, Z 5); `HDD0-` maps to disk 0. DASHPATH,
BOOTANIMPATH and the screenshot/TDATA settings (spec 13) can therefore point at the second disk.

## 6. The partition-table query
The 0xCAFEBABE device control (spec 03 §5) works on either disk device and returns that disk's table.

## For OXOS (notes, not Cerbios behaviour)
- Keep slave handling out of the default path: probe with IDENTIFY PACKET and keep the retail DVD path when the slave
  is ATAPI.
- The spin-up reboot is a workaround for slaves that are not ready in time; a wait-for-ready loop with a timeout
  would avoid the extra reboot.

## Open
- What DRIVESETUP 0 does differently from 1 elsewhere (no other reader found in the extension or the kernel's
  start-up).
- The placeholder CD driver's exact replies (status codes for reads and media checks).
