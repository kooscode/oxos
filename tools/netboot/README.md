# netboot: boot OXOS kernels over the network

Flash a console once, then boot every new kernel build from your PC over Ethernet. No reflashing between builds.
At each power-on the console's boot loader (our 2BL) asks the local network for a boot image. If `netboot.py` is
running on a PC, the console downloads the image (256 KiB, well under a second on 100 Mbit/s) and boots the kernel
inside it. If no PC answers, it boots the kernel in its own flash, exactly as a normal OXOS image does.

Design and protocol: [spec/oxos-internal/09-netboot.md](../../spec/oxos-internal/09-netboot.md).

## 1. Flash the netboot image (once)

```sh
make -C kernel netboot          # -> kernel/build/netboot/bios-256k.bin (256 KiB) and bios.bin (1 MiB)
```

Flash `bios-256k.bin` or `bios.bin` to your modchip like any OXOS image. Its own kernel is a serial-log build
(`SERIAL=1`), so even a boot without a PC is visible on the debug serial port (on the Modxo rig:
`tools/modxo/console.py log`).

The console uses the address `NETBOOT_IP` (default `192.168.1.222`) in its packets. It needs no DHCP and does not
answer ARP; both sides use UDP broadcasts. If your LAN uses another subnet, build with
`make -C kernel netboot NETBOOT_IP=10.0.0.222`. Delete `kernel/build/netboot` first, because the 2BL is not rebuilt
when only a variable changes.

## 2. Serve a kernel

Build any OXOS image as usual, then point `netboot.py` at it:

```sh
make -C kernel BUILD=build/serial SELFTEST=0 SERIAL=1 build/serial/bios-256k.bin
python3 tools/netboot/netboot.py kernel/build/serial/bios-256k.bin
```

Power the console on. You should see:

```
[   0.00] netboot: serving kernel/build/serial/bios-256k.bin on UDP port 5141 (replies to 255.255.255.255)
[   3.12] netboot: console 192.168.1.222 (02:58:42:4f:58:31) asks for a kernel; offering ... (CRC-32 1a2b3c4d, built ...)
[   3.46] netboot: console 192.168.1.222 downloaded 256 KiB in 0.3 s and is booting it
```

The kernel's own log is on the debug serial port (`SERIAL=1` builds).

`netboot.py` reads the image again for every boot. Leave it running, rebuild, and power-cycle the console to boot
the new build.

| Option | Meaning |
|---|---|
| `IMAGE` | Any OXOS open image: a 256 KiB `bios-256k.bin`, or a 1 MiB `bios.bin`, whose first bank is sent; or an image up to 1 MiB whose kernel stream runs past the first bank (sent whole). Only its kernel and key block are used; the console keeps its own 2BL, and its splash comes from its flash. |
| `--to ADDR` | Where replies go. The default `255.255.255.255` uses the PC's default route. On a PC with several networks (Docker, VPN), give the console LAN's broadcast address, e.g. `--to 192.168.1.255`. |
| `--out FILE` | Also append the printed lines to FILE. |
| `--once` | Exit after one console has booted. |
| `--verbose` | Print every request (debugging the protocol). |
| `--xemu PORT` | Serve an xemu console started with `tools/run.py BIOS --net-udp PORT` instead of the LAN (see 4). |
| `--title DIR` | Also send a title (DIR's `default.xbe` and its files). The kernel writes it to `E:\\NAME\\` and runs `NAME.xbe` instead of the disc or dashboard (spec/oxos-internal/09 §7). Needs a netboot image flashed after 2026-10-25 (4 MiB downloads). |
| `--title-name NAME` | The title's folder and XBE name on E: (default `imgtest`). |

## 3. On the console

| Front LED | Meaning |
|---|---|
| solid orange | the 2BL is waiting for the link and asking the network (at most about 6 s) |
| blinking orange | the 2BL is unpacking the kernel (downloaded, or its own) |
| green / orange | the kernel is starting (spec/oxos-internal/03-debug-serial.md §6) |

- **The link:** the 2BL waits up to 4 s for the Ethernet link (auto-negotiation takes 2–3 s after power-on), then
  asks every 250 ms for 2 s. With no cable, or no answer, it boots from flash.
- **A broken download:** a request not answered within 100 ms is repeated, up to 30 times. If the image's CRC-32 or
  header is wrong, the console boots from flash. `netboot.py` says which happened.
- **Quick reboots** (the dashboard launching a game) re-enter the downloaded kernel without asking the network
  again. A power-cycle asks again.

## 4. Trying it in xemu

```sh
python3 tools/netboot/netboot.py kernel/build/serial/bios-256k.bin --xemu 9130 &
python3 tools/run.py kernel/build/netboot/bios.bin --net-udp 9130
```

xemu's `udp` network backend sends the console's Ethernet frames to `127.0.0.1:9130` and takes frames for it on
port 9131. `netboot.py --xemu` answers there.

## Troubleshooting

- **No "asks for a kernel" line:** check the cable and that the PC is on the same LAN segment (broadcasts do not
  cross routers). Check the PC's firewall allows inbound UDP 5141; with ufw: `sudo ufw allow 5141/udp`.
- **"asks" but no "downloaded":** the replies are not reaching the console. Try `--to` with the LAN's broadcast
  address.
- **It always boots the flash kernel:** the console's flash does not hold a netboot image (`make netboot`).

## 5. Running a test title on the console
Any nxdk title (or other XBE) can run on the console without touching its disk, for example the image test:

```sh
make -C kernel image-test        # tests/image-test-title/bin: default.xbe + picture.png
python3 tools/netboot/netboot.py kernel/build/serial/bios-256k.bin --title tests/image-test-title/bin
```

At power-on the console downloads the kernel and the title, installs the title as `E:\imgtest\imgtest.xbe` (with its
files beside it; D: is that folder) and runs it.
