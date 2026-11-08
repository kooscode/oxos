# 31. The upload server: a dashboard from a browser (OXOS's own)

Status: implemented 2026-11-08. Code: `kernel/src/network/` (the network card and a small IPv4 stack),
`kernel/src/uploader/` (the HTTP server and its page), `kernel/include/xk/nic.h` (the network card's registers and
bring-up, shared with the 2BL's netboot), `kernel/src/titles/loader.cpp` (when it runs), `kernel/src/core/fatal.cpp`
(its screen).

A console without a dashboard, such as one whose disk OXOS has just formatted (spec/oxos_internal/30), gets one
without a PC tool: the kernel serves a web page on the local network, and any browser uploads files and folders to
the disk and launches an XBE.

## 1. When it runs
Where the kernel would stop with "No Dashboard Found" (13–20, spec/oxos_internal/29) on a cold boot: no title to
start, and none of the dashboards of spec/oxos_internal/19 loads. Instead of that fatal error it shows the upload
screen (§5) and serves until an XBE is launched. When the network card does not start (no PCI device), the fatal
error comes as before. A quick reboot into the dashboard with none on the disk reboots cold, as before (the server
runs from code freed when the first title started, §6).

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
| `GET /api/list?path=/E/Apps` | the folder's entries as JSON: `[{"name":"default.xbe","dir":false,"size":123}, ...]` |
| `PUT /api/file?path=/E/Apps/x/default.xbe` | the request body becomes that file; missing folders on the way are created; `201` |
| `POST /api/launch?path=/E/Apps/x/default.xbe` | `200`, then the server stops and the kernel starts that XBE as a title |

Paths are percent-decoded; `..`, empty parts and characters FATX does not allow are refused (`400`). A file
system error answers `500` with the NT status. Uploads stream: the body goes to the disk as it arrives, in 128 KB
writes; `Expect: 100-continue` (curl's large uploads) is answered. `GET /api/list?path=/` lists the drives that open.

## 4. The page
One HTML page with a small script, served from the kernel image: the current folder (drive buttons, a path, the
folder's entries; folders open, an `.xbe` has a Launch button), "Upload files" (several at once) and "Upload a
folder" (the browser sends every file with its path inside the folder: a whole dashboard in one go). Files go one
at a time with `PUT` and a progress line.

## 5. The screen
The fatal error screen's layout (spec/oxos_internal/29 §2), not a fatal error (no LED sequence, no EEPROM error
history): the code in large digits as before, "No Dashboard Found", and

```
Upload one from a browser on this network:
  http://192.168.1.91/

[ You can upload files and launch any XBE ]

<status: waiting for the network / an address / the last upload or launch>
```

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
  read back byte for byte from the disk image afterwards, `..` refused, the FTP server's XBE uploaded and launched
  (the kernel runs it from `E:\app`).
- `make test_format`: the three blank or foreign disks end in the upload server (no link in those runs), the
  boot-only pages kept.
- Console (debug kit, 2026-11-08): the new 2BL netboots; after deleting all partitions the disk is formatted at boot
  and the server answers at its DHCP address with the page and the drives; uploads at 11.8 MB/s (network only, a
  drive that does not exist) and 10.9 MB/s to the disk (50 MB in 4.8 s); the FTP server's XBE uploaded and launched
  from the page's API, 15 boot-only pages freed as it started, and the 50 MB file read back over FTP intact (SHA-256).
