# 31. The upload server: a dashboard from a browser (OXOS's own)

Status: implemented 2026-11-08. Code: `kernel/src/network/` (the network card and a small IPv4 stack),
`kernel/src/uploader/` (the HTTP server and its page), `kernel/include/xk/nic.h` (the network card's registers and
bring-up, shared with the 2BL's netboot), `kernel/src/titles/loader.cpp` (when it runs), `kernel/src/core/fatal.cpp`
(its screen).

The OXOS File Manager: the kernel serves a web page on the local network, and any browser uploads files and folders
to the disk, creates and deletes folders and files, and launches an XBE. A console without a dashboard, such as one
whose disk OXOS has just formatted (spec/oxos_internal/30), gets one without a PC tool.

## 1. When it runs
On a cold boot with X pressed during the boot splash or on the "No Dashboard Found" screen
(spec/oxos_internal/32), until an XBE is launched. When the network card does not start (no PCI device), the title is chosen as usual. Until 2026-11-08 it
also ran by itself where the kernel would stop with "No Dashboard Found"; that screen now waits for X.

## 2. The network
- **The card:** the MCPX's NVIDIA Ethernet controller, polled (no interrupts), with the same bring-up as the 2BL's
  netboot (`xk/nic.h`). 64 receive and 32 transmit descriptors in one uncached page (the card and the processor both
  change them); the frame buffers in ordinary cached memory (the MCPX's DMA keeps the cache coherent; with uncached
  buffers every byte the stack read went to RAM, and uploads ran at 2.7 MB/s instead of 11.8). Its MAC address is the
  console's, EEPROM factory setting 0x101, so the server gets the address the console's titles get from DHCP.
- **IPv4** with ARP (answers requests for our address; a reply goes to the MAC address the request came from), ICMP
  echo (ping), UDP for DHCP, and TCP. No fragments, no IP options, no routing beyond answering the sender.
- **DHCP:** DISCOVER, OFFER, REQUEST, ACK; no lease renewal (the server runs for minutes, not days). No answer within
  30 s: the netboot address (`NETBOOT_IP`, 192.168.1.222 by default), as the FTP server does. The link is checked
  continuously: "Connect a network cable" until it is up.
- **TCP**, as a server on port 80 only: up to 4 connections, each with its own receive (64 KB) and send (64 KB)
  buffers. In-order data only (a segment past a gap is dropped and the expected one asked for again with a duplicate
  ACK). One ACK per connection after each batch of received frames, not one per segment (a window is some 45
  segments; an ACK each queued behind the card's transmit ring). The receive window is the free space of the
  connection's buffer, at most 65535 (no window scaling), so a sender never has more in flight than the receive ring
  holds; when it has grown by two segments since the last segment we sent, a window update goes out at once. What we
  send is kept until acknowledged and sent again from the oldest unacknowledged byte after 1 s without an ACK (up to
  8 times, then the connection is reset). MSS 1460, or less when the peer asks. Connections idle for 30 s are reset;
  after our FIN, a connection ends when both FINs are acknowledged, or 2 s after ours.

## 3. HTTP
HTTP/1.1, one request per connection (`Connection: close`). Paths use the FTP server's form: `/C`, `/E`, `/F`, `/X`,
`/Y`, `/Z` for the drives (spec/oxos_internal/05 §2 on exFAT, the partitions on FATX), then folders, separated by `/`.

| Request | Answer |
|---|---|
| `GET /` | the page (§4) |
| `GET /api/disk` | the disk as JSON: `{"fs":"exFAT","bytes":2000398934016}`; `fs` is `FATX`, `exFAT`, or `none` (a PC disk without an exFAT volume, spec/oxos_internal/05) |
| `GET /api/list?path=/E/Apps` | the folder's entries as JSON: `[{"name":"default.xbe","dir":false,"size":123}, ...]` |
| `PUT /api/file?path=/E/Apps/x/default.xbe` | the request body becomes that file; missing folders on the way are created; `201` |
| `PUT /api/folder?path=/E/Apps/new` | creates that folder (and missing folders on the way); `201`; one that exists: `409` |
| `POST /api/rename?path=/E/Apps/old&name=new` | renames that file or folder in its folder (`FileRenameInformation`); `200`. A name in use: `409`; a drive (FATX) or top-level folder (exFAT): `403` ("Cannot rename E:\\ (a drive)" or "Cannot rename /E (a top-level folder)"); a name FATX does not allow: `400` |
| `DELETE /api/file?path=/E/Apps/old.bin` | deletes that file, or that folder when it is empty (`NtDeleteFile`); `200`. A folder with anything in it: `409`, nothing deleted (nothing is deleted recursively). A drive or top-level folder (`/E`): `403` ("Cannot delete E:\\ (a drive)", or "/E (a top-level folder)" on exFAT) |
| `POST /api/launch?path=/E/Apps/x/default.xbe` | `200`, then the server stops and the kernel starts that XBE as a title |

Paths are percent-decoded; `..`, empty parts and characters FATX does not allow are refused (`400`). A file
system error answers `500` with the NT status. Uploads stream: the body goes to the disk as it arrives, in 128 KB
writes; `Expect: 100-continue` (curl's large uploads) is answered. `GET /api/list?path=/` lists the drives that open.

## 4. The page
One HTML page with a small script, served from the kernel image, titled "OXOS File Manager". Its header adds the disk's
file system and size from `/api/disk`, e.g. `OXOS File Manager [exFAT, 2.0 TB]` (decimal units, as disk makers state
them). Below it, one button for each drive or top-level folder that `/api/list?path=/` returns: on a FATX disk the
label is `Drives:` and the buttons read `C:` `E:` ...; on an exFAT disk, which has one volume and no drives, the label
is `Folders:` and the buttons read `/C` `/E` ... (spec/oxos_internal/05 §2). Paths are shown the same way, on the
page and in the server's messages and status line: `E:\Apps\x.xbe` on a FATX disk (partitions), `/E/Apps/x.xbe` on an
exFAT disk (folders off the volume's root); requests always use the `/E/...` form. Then the current folder (a path, the
folder's entries; folders open, an `.xbe` has a Launch button, every file and folder but `..` and the drives has
Rename (asks for the new name) and Delete (asks first); the `..` row has Create Folder, which asks for the name), "Upload files" (several at once) and "Upload a
folder" (the browser sends every file with its path inside the folder: a whole dashboard in one go). Files go one
at a time with `PUT` and a progress line.

## 5. The screen
The OXOS File Manager's screen: spec/oxos_internal/32 §5. Its status line shows the network's state, then the last
upload, create, delete or launch.

## 6. Memory
The network stack, the server and its page are boot-only (spec/oxos_internal/18; about 15 KB of code and data): they
run before the first title and are freed when it starts (15 pages on the debug kit), so titles lose no memory. The
resident kernel grows by the upload screen and the hooks only (about 1.8 KB, no new page). Their buffers (the card's
rings and frame buffers, the connections' and the requests' buffers, about 1.3 MB) are allocated when the server
starts and freed before the title starts; the network card is stopped first, so it does no DMA into memory the title
gets. Ending the splash early (the format countdown, the upload screen: `HalpStopSplash`) keeps the boot-only pages;
the title start (`HalpEndSplash`) frees them.

## 7. Safety
While the screen is up, anyone on the local network can write to the disk and start code, as with the FTP server
(no log-in). The screen says so: it only appears when the console has nothing else to run, and it ends when a title
starts.

## 8. Checks
- `make test_upload` (xemu, NAT network with a forwarded port): a blank disk is formatted at once (marker 0), no
  dashboard, the server starts; the page, the drive list, a 300 KB upload into a new folder listed with its size and
  read back byte for byte from the disk image afterwards, `..` refused, a folder with a file in it not deleted
  (`409`), the file then the empty folder deleted, a drive not deleted (`403`), a folder created (`201`) and not
  created twice (`409`), a file renamed but not onto a name in use (`409`), a folder renamed with its files, a drive
  not renamed (`403`), the FTP server's XBE uploaded and launched
  (the kernel runs it from `E:\app`).
- `make test_format`: the three blank or foreign disks end in the upload server (no link in those runs), the
  boot-only pages kept.
- Console (debug kit, 2026-11-08): the new 2BL netboots; after deleting all partitions the disk is formatted at boot
  and the server answers at its DHCP address with the page and the drives; uploads at 11.8 MB/s (network only, a
  drive that does not exist) and 10.9 MB/s to the disk (50 MB in 4.8 s); the FTP server's XBE uploaded and launched
  from the page's API, 15 boot-only pages freed as it started, and the 50 MB file read back over FTP intact (SHA-256).
