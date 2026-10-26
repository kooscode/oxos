# mcpx-dump: read your Xbox's MCPX boot ROM over the network

Every original Xbox has a 512-byte secret boot ROM inside its southbridge (the MCPX). Emulators such as xemu need a
copy of it to run Microsoft BIOS images, and the copy has to come from your own console. This tool gets it out with a
modchip and a network cable. You don't need a soldered serial port, a logic analyser or a screen.

It works on consoles with:

| Console | MCPX | Method |
|---|---|---|
| 1.0 | 1.0 | RC4 attack |
| 1.1-1.5 | 1.1 | TEA attack |
| 1.6 | 1.6 (Xyclops) | not supported |

Design: [spec/oxos-internal/12-mcpx-dump.md](../../spec/oxos-internal/12-mcpx-dump.md).

## What you need
- A modchip you can flash with a 256 KiB image, for example Aladdin, Xenium or Modxo.
- **Your own retail BIOS for the console's MCPX**, as a file:
  - MCPX 1.1 needs a retail BIOS with the TEA first-stage loader (any 5xxx retail BIOS, e.g. 5838).
  - MCPX 1.0 needs a 4xxx retail BIOS.
  - Any size; the top 256 KiB is used. Nothing from Microsoft ships with OXOS.
- A PC with Python 3, on the same network segment as the console. Use a cable or a switch, not a router in between.
- The OXOS build tools (see the repository README), to build the payload once.

## Steps
1. Build the payload, an OXOS netboot image whose boot loader also sends the ROM:

   ```sh
   make -C kernel mcpx-dump        # -> kernel/build/mcpx-dump/bios-256k.bin
   ```
2. Build the flash image from your retail BIOS:

   ```sh
   python3 tools/mcpx-dump/mcpx_dump.py build --mcpx 1.1 --bios my-5838.bin --out mcpx-dump.bin
   ```
   It prints what it changed. For 1.1 it also confirms that the ROM's hash of the loader is unchanged.
3. Flash `mcpx-dump.bin` to the modchip, the same way you flash any 256 KiB BIOS.
4. Start the receiver on the PC:

   ```sh
   python3 tools/mcpx-dump/mcpx_dump.py receive --bios my-5838.bin
   ```
5. Connect the console's network port and power it on. Within a few seconds the receiver prints:

   ```
   dump from 192.168.1.222: MCPX 1.1, md5 ..., sha1 ...
   saved mcpx_1.1.bin
   ```
   The console then carries on booting. It boots the OXOS kernel in the image, or one served by
   `tools/netboot/netboot.py` if one is running.
6. Flash your normal BIOS back.

## How the dump is checked
- It must start with `33 C0` and end with `B0 02 EE` (the ROM's hide sequence).
- With `--bios`, the receiver also checks a value the ROM stores against the BIOS you built the image from:
  - MCPX 1.1: the loader hash at ROM offset 0x1A8 must equal the TEA hash of that BIOS's first-stage loader.
  - MCPX 1.0: the check dword the ROM compares must match that BIOS's decrypted boot loader.
- A dump that fails is not saved, and the receiver waits for the next copy (the console sends three).
  `--keep-bad` saves it anyway.

## Options
| Command | Option | Meaning |
|---|---|---|
| `build` | `--mcpx 1.0` / `1.1` | the console's MCPX (see the table above) |
| | `--bios FILE` | your retail BIOS |
| | `--payload FILE` | the payload image (default `kernel/build/mcpx-dump/bios-256k.bin`) |
| | `--out FILE` | the image to flash |
| `receive` | `--bios FILE` | also check the dump against that BIOS |
| | `--out FILE` | file name (default `mcpx_1.0.bin` / `mcpx_1.1.bin`) |
| | `--xemu PORT` | wait for an xemu console started with `tools/run.py IMAGE --mcpx-rom ROM --net-udp PORT` |
| | `--keep-bad` | save a dump that fails the checks |

## Troubleshooting
- **Nothing arrives:**
  - Check the cable, and that no firewall drops UDP port 5141 (`sudo ufw allow 5141/udp`).
  - Don't run `netboot.py` at the same time: both listen on port 5141.
  - On a PC with several networks, the console's broadcasts may arrive on another interface.
- **"does not start with 33 C0":** the ROM was already hidden. The image was flashed for the wrong MCPX, or the
  console booted something else.
- **The console resets or blinks red/green:** the image was built for the other MCPX, or the retail BIOS is not
  one for this MCPX.
- **Testing in xemu:** only the 1.0 path can be tested there (xemu needs `mcpx_1.0.bin` for it). The 4xxx X-codes
  expect a Conexant encoder, so start xemu with `--encoder conexant`.

## Credits
The attacks are those of [XboxDev/mcpx-tools](https://github.com/XboxDev/mcpx-tools) (`mcpx-attack`); this is an
independent implementation. Background: [xboxdevwiki: Exploits](https://xboxdevwiki.net/Exploits).

The ROM is Microsoft code. Keep your copy to yourself.
