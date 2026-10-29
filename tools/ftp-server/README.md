# ftp-server

An FTP server for the console's hard disk, as an nxdk title. Copy files to and from the console and organise them
(make folders, move, rename, delete) from any FTP client: FileZilla, WinSCP, lftp, curl, Windows Explorer, Python's
`ftplib`. It runs on OXOS and, as a plain nxdk title, on other kernels.

```
make -C kernel ftp-server                    # -> tools/ftp-server/bin/default.xbe and ftp-server.iso

# On a console with an OXOS netboot image: the kernel and the server in one go (the server runs from E:\ftp\)
python3 tools/netboot/netboot.py kernel/build/serial/bios-256k.bin --title tools/ftp-server/bin --title-name ftp

# Or copy bin/default.xbe to the disk and start it from a dashboard, or burn / mount ftp-server.iso.
```

The screen shows the address, e.g. `ftp://192.168.1.182/`. With a `SERIAL=1` kernel the address and every change
(upload, folder, move, delete) are also on the debug serial port. Log in with any user name and password.

| Path | Drive |
|---|---|
| `/` | the drives that exist: C, E, F, G, X, Y, Z |
| `/F/games/Halo` | `F:\games\Halo` (`\Device\Harddisk0\Partition6\games\Halo`) |

## What it does
- **Commands:** USER PASS SYST FEAT OPTS PWD CWD CDUP TYPE MODE STRU PASV EPSV PORT LIST NLST MLSD MLST RETR STOR
  APPE REST SIZE MDTM DELE MKD RMD RNFR RNTO NOOP ABOR QUIT. Transfers are always binary. Up to 8 clients at once.
- **Moving:** RNTO to a path in another folder moves the file or folder there (same drive only; the kernel opens the
  target folder as the reference kernel does). Moving between drives is refused; copy and delete instead.
- **Big images:** on exFAT, files of 4 GB or more are readable, can be moved, renamed and deleted, but not written
  ([`spec/oxos-internal/07-exfat-driver.md`](../../spec/oxos-internal/07-exfat-driver.md)). Upload images under
  4 GB or split them (`tools/oxos-isonator --split`), or copy big images with the disk on a PC.
- **SITE REBOOT** restarts the console (cold); **SITE POWEROFF** turns it off. In a client:
  `curl ftp://ADDR/ -Q 'SITE POWEROFF'`.
- **Network:** DHCP; with no answer in 10 s the fixed address 192.168.1.223/24. Passive data ports are 50000-50099
  (open them in a firewall between the PC and the console). Writes go straight to the disk, so the console can be
  switched off as soon as a transfer has finished.
- **xemu:** `tools/run.py BIOS --iso tools/ftp-server/ftp-server.iso --net-forward 2121:21` plus `--net-forward P:P`
  for each passive port P in 50000-50099, then `ftp://127.0.0.1:2121/` (a client that ignores the address in the PASV
  reply, as curl and `ftplib` with `trust_server_pasv_ipv4_address = False` do).
