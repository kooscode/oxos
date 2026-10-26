# 12. MCPX boot-ROM dumper

OXOS addition, a tool rather than kernel behaviour. A flash image that makes a console send its MCPX boot ROM over
the network. Implemented 2026-10-25. User guide: `tools/mcpx-dump/README.md`.

Verified:
- MCPX 1.1, on the no-DVD 1.4 console. The dump matched the loader hash it stores.
- MCPX 1.0, in xemu with a 1.0 ROM. The dump was identical to the ROM given to xemu.

## 1. Why our normal entry can't read the ROM
- Open images enter the 2BL through the X-codes' overflow trick (spec/stock/01 §5). The ROM's check fails, it hides
  itself (host bridge 0x80000880 ← 2), and execution wraps to RAM address 0. By then 0xFFFFFE00-0xFFFFFFFF reads
  flash.
- X-code reads don't help either. On the real ROM, X-code memory reads of the flash or ROM range return 0 (observed
  in xemu with the 1.0 ROM; our own reset-code interpreter reads them fine).
- So the dumper passes the ROM's check instead. The ROM then hands over control without hiding itself.

## 2. The image (`tools/mcpx-dump/mcpx_dump.py build`)
Built from the user's retail BIOS (top 256 KiB):
- Kept from the retail BIOS: 0x0000-0x0FFF (init tables, X-codes); for MCPX 1.1 also 0x3D400 onwards; for MCPX 1.0
  0x39E00 onwards.
- Payload (`make -C kernel mcpx-dump`, an open netboot image built with `MCPX_DUMP=1` and no splash): copied over
  0x1000 to 0x3D400 (1.1) or 0x39E00 (1.0). Only the payload's quick-reboot `.data` copy and reset code fall
  outside.
- **MCPX 1.1 (TEA attack):**
  - The ROM hashes flash 0xFFFFD400-0xFFFFFC80 with TEA. For each 8 bytes it runs two TEA rounds, with key =
    {the running value, the data's two dwords}. Bit 31 of key words 2 and 3 flipped together gives the same result,
    so flipping bit 31 of the first-stage loader's dwords 0 and 1 keeps the hash.
  - Its first instruction is `jmp rel32` (E9 83 01 00 00); the flip makes it land at 0x7FD588.
  - Two extra X-code pokes before the X-codes' `exit` write `jmp 0xFFFC1000` there.
- **MCPX 1.0 (RC4 attack):**
  - The ROM RC4-decrypts flash 0x39E00 (0x6000 bytes) to 0x90000, compares the dword at 0x95FE4 with its stored
    value, and jumps to the entry pointer in the first decrypted dword.
  - That ciphertext dword is replaced with 0xFFFC1000 XOR the key's first keystream dword (0x79277645).
- The attacks are those of XboxDev/mcpx-tools (mcpx-attack), implemented independently.

## 3. The payload's 2BL (`MCPX_DUMP=1`)
- At entry (`kernel/boot/start.S`), after its own GDT and segments: copy 512 bytes from 0xFFFFFE00 to physical
  0x2000, then write 'MCPX' (0x5850434D) at 0x2200.
- In netboot (`kernel/boot/netboot.h`): once the link is up, before the first HELLO, if the marker is present, send
  netboot op 6 (`OP_MCPX`) three times, 100 ms apart. The message is the usual 20-byte header (Offset 0, Length 512)
  followed by the 512 bytes.
- Then it boots as any netboot image: a served kernel, else its own.
- Builds without `MCPX_DUMP` are unchanged.

## 4. The receiver (`mcpx_dump.py receive`)
- Listens on UDP 5141 (or `--xemu PORT` for xemu's frame transport) for op 6, and saves the 512 bytes.
- Checks:
  - Starts `33 C0`, ends `B0 02 EE`.
  - With the retail BIOS: for 1.1 (identified by the TEA constant 0x9E3779B9 in the ROM), ROM 0x1A8-0x1AF must equal
    the BIOS loader's TEA hash. For 1.0, the BIOS 2BL decrypted with the key at ROM 0x1A5 must hold the ROM's value
    (0x187) at the ROM's offset (0x182 − 0x90000).
- The dump is the user's; nothing from it or the retail BIOS goes into the repository.
