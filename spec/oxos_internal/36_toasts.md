# 36. Toasts: short messages on screen while a title runs (proposal)

Status: **proposal**, not implemented (user, 2026-11-14: "write it up as a proposal ... would be nice if extensions could also popup a toast"; reworked
2026-11-23 after a pilot on the GPU's video overlay, user: "update the toast spec to use pvideo and the rest of implementation proposals"). Nothing here is
decided until the user says so.

**Credit:** showing toasts on the NV2A's video overlay (PVIDEO) instead of drawing into the title's frames was suggested by
[JayFoxRox](https://github.com/JayFoxRox) in [issue #7](https://github.com/kooscode/oxos/issues/7): the first version wrote into the game's memory, which
might cause side effects, while his [xbox-fps-overlay](https://github.com/JayFoxRox/xbox-fps-overlay) used PVIDEO, which games rarely use (mostly for
movies), with a fallback when a game needs it (§7).

Today OXOS draws only while no title runs: the splash, the error screens and the File Manager (`kernel/src/graphics/`). A **toast** is a short message the
kernel shows over the running title for a few seconds, then removes: "Saved", "Extension example_run_loop stopped", "Friend request: Name. BLACK + WHITE to
open". Extensions push toasts through a kernel export (§5); a toast can carry an action, a button combination that starts another XBE while it is shown (§4).

## 1. What a toast is
| Field | Meaning |
|---|---|
| Text | one or two lines of printable ASCII (the kernel's font, `graphics::DrawText`), at most 48 characters a line; longer is cut |
| Place | one of nine anchors (top, middle or bottom; left, centre or right), in fractions of the screen, so it lands in the same place in every mode (§3.3) |
| Time | how long it stays up, 1 to 30 seconds (default 5) |
| Action (optional) | a button combination and an XBE path: pressing the combination while the toast is up starts that XBE (§4) |

- **One toast at a time.** Others wait in a queue (8 entries) and follow in order; when the queue is full the push fails (§5), the queue is never reordered.
- **Look:** OXOS's colours from the boot splash: black box, a border in OXOS green `#8BE31F`, "OXOS" or the extension's short name in green, the text in
  `#F4FFE6`.
- **The title keeps running and keeps its controllers.** A toast with an action only watches for its combination (§4); every button still reaches the title.

## 2. Who shows one
- **The kernel**, for its own events, each a decision of its own when it is built: the in-game reset's combination seen (spec 22), an extension unloaded or a
  slow run loop (spec 34 §4.3), a log file that could not be written (spec 35).
- **Extensions**, through the export (§5), from `ExtensionStart` (the toast waits until the title has run 5 seconds), from `ExtensionRunLoop`, from a hook or a
  title event.
- **The user switches them off** with one setting (`[toasts]` in `C:\OXOS\oxos_settings.ini`, `enabled = no`); the export then returns FALSE and nothing
  shows. Safe boot (B held) shows no toasts.

## 3. How the kernel shows one: the GPU's video overlay (PVIDEO)
The NV2A has a video overlay: the CRTC blends a second picture, from its own buffer in memory, over the frame it scans out. The title's frames, its frame
buffers and its GPU commands are never touched; the toast costs no drawing per frame. A pilot (2026-11-23, a throwaway kernel thread at each title start,
5 seconds after the start, up for 10 seconds) showed a toast this way over a dashboard and over a game, in xemu and on the Conexant debug kit at 720p.

### 3.1 The overlay registers
NV2A registers at 0xFD000000 + offset, as the open nouveau driver documents the NV10-class overlay and as the pilot used them (buffer 0 only):
| Offset | Register | The pilot's value |
|---|---|---|
| 0x8704 | STOP | 1 while it is set up, then 0; 1 again to stop |
| 0x8900 | BASE(0) | 0 |
| 0x8908 | LIMIT(0) | 0x07FFFFFF |
| 0x8910 / 0x8918 | LUMINANCE(0) / CHROMINANCE(0) | 0x00001000 each (contrast 1.0, brightness 0) |
| 0x8920 | OFFSET(0) | the picture's physical address |
| 0x8928 | SIZE_IN(0) | height << 16 \| width |
| 0x8930 | POINT_IN(0) | 0 (12.4 fixed point: the source's top left) |
| 0x8938 / 0x8940 | DS_DX(0) / DT_DY(0) | 1 << 20 (1:1; smaller values scale the picture up, §3.3) |
| 0x8948 | POINT_OUT(0) | y << 16 \| x, in the current mode's pixels |
| 0x8950 | SIZE_OUT(0) | height << 16 \| width on screen |
| 0x8958 | FORMAT(0) | row pitch in bytes \| 1 << 16 (YUY2: Y0 U Y1 V); bit 20 (colour key) clear |
| 0x8700 | BUFFER | 1: buffer 0 in use (shown); 0: hidden |

### 3.2 What the pilot found
- **The picture is YUV 4:2:2 (YUY2):** the kernel draws the toast in RGB with its font, then converts it (BT.601), two pixels to a dword.
- **The row pitch must be a multiple of 64 bytes on a console.** A 400-pixel picture (800 bytes a row) came out with every row shifted (garbled text); 384
  pixels (768 bytes) was right. xemu does not check this, so xemu alone does not prove a layout.
- **No transparency.** The overlay has no alpha, and its colour key works the wrong way for toasts (it shows the overlay only where the title's frame has the
  key colour). A toast is a solid rectangle; a rounded or shaped look is drawn inside it. True shapes need §8.
- **The buffer** (384 x 44 x 2 = 33 KB in the pilot) is contiguous memory taken when the toast shows and freed after it, write-combined.

### 3.3 Place in every mode
The kernel reads the mode in use (width, height, interlaced; from the CRTC's timing registers, or from the mode the title set through `AvSetDisplayMode`) and
places the anchor in fractions of it: bottom centre is (width / 2 - box / 2, height × 0.85). The box is drawn once at 480-line size; in 720p and 1080i the
overlay scales it up itself (DS_DX / DT_DY below 1 << 20), so the font is the same size on every screen and nothing is drawn twice. Interlaced modes (480i,
1080i) need the field handling checked on hardware.

### 3.4 When and where
A kernel thread of the toast service (§6) waits on the queue, at a low priority, from the title's start: it shows a toast only once the title has run 5
seconds (titles set their video mode early), keeps it up for its time, hides it, and takes the next.

## 4. Actions: a button combination while the toast is up
A toast may carry an action: a combination of buttons (the digital buttons and the analog buttons as pressed, e.g. BLACK + WHITE) and an XBE path. While the
toast is up, and for one second after, the kernel compares the pads' state against the combination; when all of its buttons are held, it starts the XBE as a
title launch does (a launch data page with that path, then a quick reboot, as `XLaunchNewImage`; spec/stock/05 §2), and the title ends.
- **How the kernel sees the buttons: the USB host controllers' done lists** (spec 22 §9.2). At each USB interrupt, before the title's own handler, the kernel
  reads the pads' 20-byte input reports the controller has just finished, through a reserved page (the title's USB buffers are often in its own memory). No
  hook in the title: it works the same for every title, whether built with the XDK, nxdk or anything else, and the in-game reset uses the same reports.
- **An action toast waits for the first pad report.** Games often sit in intro movies without reading a pad, and a combination cannot be seen before the title
  reads its pads. The service holds an action toast until the first report of the title (up to 60 seconds), then shows it one second later and watches for the
  combination while it is up. A toast without an action keeps the fixed delay (§3.4).
- **The launch stops the title first** (spec 22 §9.3): the interrupt only sets a flag; the service's thread suspends every thread of the title, waits 50 ms,
  writes the launch data page and quick-reboots. Without the suspend a title's code ran on during the reboot and stopped the kernel (§4.1).
- **Rules:** a combination needs at least two buttons; it may not be the in-game reset's (LT + RT + BACK + BLACK) or a subset of it; an action toast says its
  combination in its text. With toasts off (§2) no action can fire. An action toast whose XBE is the one already running (the same path as
  `XeImageFileName`) is not shown: "BLACK + WHITE: FTP" never appears over the FTP server.
- **Example:** an Xbox Live service extension pushes "Friend request: Name. BLACK + WHITE to open" with the action BLACK + WHITE →
  `E:\Apps\Dashboard\default.xbe`.

### 4.1 Pilot (2026-11-23, the Conexant debug kit, a throwaway build)
The toast "OXOS BLACK + WHITE: FTP" with the action BLACK + WHITE → `C:\OXOS\apps\oxos_ftp.xbe`:
| Step | What happened |
|---|---|
| Buttons from the in-game reset's `XInputGetState` hook, toast 5 s after the title's start | in a game, nothing: the game read no pad until after the toast (its intro) |
| The same, the toast held until the game's first pad read | in a game: works, the FTP server loaded 0.4 s after the press; in nxdk titles the hook never fires |
| Buttons from the USB done lists, quick reboot from the toast thread | an nxdk dashboard: the combination seen; the reboot let the dashboard's own code run on and stopped the kernel (bug check 0x1E in the title) |
| The same, the title's threads suspended first | an nxdk dashboard (10 threads): works, 0.47 s from the press to the FTP server |
| A retail game (its USB buffers in its own memory), reading through 0x80000000 | bug check 0x1E in the interrupt dispatch: the buffer was not mapped there |
| The same, reading through a reserved page | works: 2 threads suspended, 0.47 s from the press to the FTP server |

## 5. The export and the SDK
A kernel export of OXOS's own (after 432, spec 34 §9.1) takes a toast, copies it and queues it:
```cpp
// oxos_sdk.h: toasts over the running title (spec/oxos_internal/36).
typedef enum _OXOS_TOAST_PLACE {
    OXOS_TOAST_TOP_LEFT, OXOS_TOAST_TOP_CENTRE, OXOS_TOAST_TOP_RIGHT,
    OXOS_TOAST_MIDDLE_LEFT, OXOS_TOAST_MIDDLE_CENTRE, OXOS_TOAST_MIDDLE_RIGHT,
    OXOS_TOAST_BOTTOM_LEFT, OXOS_TOAST_BOTTOM_CENTRE, OXOS_TOAST_BOTTOM_RIGHT,
} OXOS_TOAST_PLACE;

typedef struct _OXOS_TOAST {
    const char *Text;          // one or two lines ('\n' between them), printable ASCII
    OXOS_TOAST_PLACE Place;    // default OXOS_TOAST_BOTTOM_CENTRE
    ULONG Seconds;             // how long it stays up, 1 to 30 seconds; 0 means the default (5 seconds)
    ULONG ActionButtons;       // 0 = no action; else XINPUT_GAMEPAD_* digital bits | OXOS_TOAST_BUTTON_* analog buttons, held together
    const char *ActionPath;    // the XBE the action starts, a drive path ("E:\\Apps\\Dashboard\\default.xbe")
} OXOS_TOAST;

BOOLEAN oxos_toast_show(const OXOS_TOAST *Toast); // queued; FALSE when toasts are off, the queue is full or the toast is not valid (§4 rules)
BOOLEAN oxos_toast(const char *Format, ...);      // one line, bottom centre, 5 seconds, no action: formatted like oxos_log
```
- The kernel copies the text and the path, so the caller's buffers may go away at once. It may be called at any IRQL up to DISPATCH_LEVEL; the drawing
  happens on the service's thread.
- The kernel's own toasts use the same queue.

**An SDK example, `sdk/examples/example_toast`:** it hooks `NtOpenFile` and `NtCreateFile` (`oxos_hook`) and pushes a toast when the title opens a file
whose name is in its ini (`extensions\example_toast.ini`, `[files]`, e.g. `save = savegame.dat`): "Saving: savegame.dat", top right, 3 seconds. It shows a hook,
the ini helpers and the toast export together, and does the toast from the hook (no work of its own in the title's thread beyond the name compare).

## 6. Memory
Titles get what the kernel leaves free (spec 18 §7). The service has three parts:
| Part | Where | Cost |
|---|---|---|
| The export and the queue (8 toasts, their text) | resident | under 1 KB |
| The thread, the drawing, the YUY2 conversion, the overlay registers, the actions | a kernel module would be unloaded before the title runs; so either resident (estimated 2-3 KB: check against the page budget), or a kernel extension shipped with OXOS (`oxos_toasts.xos`, loaded only when toasts are on) that registers itself with the export | resident: maybe a page; extension: its pages only when on |
| The picture | contiguous memory taken while a toast shows | 33 KB for 384 x 44, freed after |

Which of the two is decided when it is built, by the page count (test_memory, 15 960 pages).

## 7. Titles that use the overlay themselves
A few titles play video through PVIDEO. A toast would replace their video for its time, and their next overlay write would take the overlay back. Before
showing, the service reads BUFFER (0x8700) and STOP (0x8704); when the title has the overlay on, the toast waits (up to its own time) and is dropped if the
overlay never goes off. After a toast, the service puts back every register it changed.

## 8. Alternatives tried or kept for later
- **Drawing into the title's frame** (this spec's first version, 2026-11-14): find the frame shown through the CRTC's start address (0x600800) and write the
  toast into it after each flip. Any shape and real transparency, and the base for screenshots, but redrawn every frame, with tiled frame buffers, pixel
  formats and tearing to handle. Kept for shaped toasts and screenshots.
- **The hardware cursor** as a 64 x 64 badge with alpha beside the box: tried in the pilot (cursor config 0x600810 = 64 x 64, 32-bit, enabled; CRTC CR30 /
  CR31 / CR2F with the address and the show bit; position at 0x680300, as nouveau documents them). Nothing showed on the debug kit (the config register held
  0x2000000 before); not pursued.

## 9. Tests
- **Host unit tests:** the queue (order, full queue, copies), the anchors in 480p, 720p and 1080i, the action rules (§4), the YUY2 conversion.
- **xemu:** extension_test pushes a toast; boot_test.py reads the PVIDEO registers over QMP (shown, place, size, pitch a multiple of 64) and that they are
  back after the toast's time.
- **Hardware:** the capture card on each encoder (Conexant, Focus, Xcalibur) in 480i, 480p, 720p and 1080i: the toast's place and text, no flicker, the game's
  frame rate unchanged; an action toast's combination starting its XBE; a title that plays video through the overlay (§7).

## 10. Open
- Resident service or shipped extension (§6).
- Which kernel events show a toast (§2): each its own decision.
- A second size, or an icon in the box (drawn in the rectangle, §3.2).
- Whether an action may start any XBE or only ones listed in a settings file.
