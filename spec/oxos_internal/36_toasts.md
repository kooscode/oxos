# 36. Toasts: short messages on screen while a title runs

Status: **implemented 2026-11-23** (user, 2026-11-14: "write it up as a proposal ... would be nice if extensions could also popup a toast"; reworked
2026-11-23 after a pilot on the GPU's video overlay, user: "update the toast spec to use pvideo and the rest of implementation proposals"; built the same
day for the in-game shortcuts, spec 48, user: "go ahead with the toast service - get it all working end to end"). Code: `kernel/src/toasts/` (the queue
`toast_queue.cpp`, the display `toast_overlay.cpp`, the box's size, place and colours `toast_layout.h`), the SDK's section 8 (`sdk/include/oxos_sdk.h`),
`sdk/examples/example_toast`. Tests: `tests/unit/test_toasts.cpp`. The kernel's own toasts (§2) are not built: each is a decision of its own.

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
| Text | one or two lines of printable ASCII (the kernel's font, `graphics::DrawText`), at most 42 characters a line (`OXOS_TOAST_LINE_MAX`: the widest box then fits between the side margins in 640x480); anything else is refused (`STATUS_INVALID_PARAMETER`); `oxos_toast` cuts its one line to fit |
| Place | one of nine anchors (top, middle or bottom; left, centre or right), in fractions of the screen, so it lands in the same place in every mode (§3.3) |
| Time | how long it stays up, 1 to 30 seconds (default 5) |

- **One toast at a time.** Others wait in a queue (8 entries) and follow in order; when the queue is full, `oxos_toast_enqueue` fails and the caller may try
  again, or `oxos_toast_enqueue_wait` waits for room (§5); the queue is never reordered.
- **Look:** OXOS's colours from the boot splash: black box, a 3-pixel border in OXOS green `#8BE31F`, the text in `#F4FFE6` (font scale 2: 12 pixels a
  character, lines 20 pixels apart). The box is as wide as its longest line plus 16 pixels each side, rounded up to 32 pixels (§3.2's pitch), and
  44 pixels high for one line, 64 for two.
- **The title keeps running and keeps its controllers.** A toast is never interactive itself; an extension can act on buttons while its toast is up (§4).

## 2. Who shows one
- **The kernel**, for its own events, each a decision of its own when it is built: a frozen sound codec (spec 46 §3, 2026-11-23: "Audio Codec Frozen -
  Restarting" drawn at once before the power cycle, `toasts::ShowNotice`, and "Audio Codec Frozen" / "Power off and on to restore sound" queued when the
  guard stops a second cycle, `toasts::QueueKernelToast`); proposed, not built: an extension unloaded or a slow run loop (spec 34 §4.3), a log file that
  could not be written (spec 35).
- **Extensions**, through the export (§5), from `ExtensionStart` (the toast waits until the title has set its video mode, §3.4), from `ExtensionRunLoop`,
  from a hook or a title event.
- **The user switches them off** with one setting (`[toasts]` in `C:\OXOS\oxos_settings.ini`, `enabled = no`, read at the cold boot by the kernel
  settings module); the exports then return `STATUS_NOT_SUPPORTED` and nothing shows. Safe boot (B held) loads no extensions, so no toasts.

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
The kernel reads the mode in use from the NV2A's flat-panel timing, which every mode set writes: the vertical display end (0x680800) and the horizontal
display end (0x680820), each the active size minus one. Not the horizontal CRTC size (0x680828): the encoders' 640-pixel modes hold 600 there, and a toast
placed by it sat 20 pixels left of centre (xemu, 2026-11-23). Values outside 320-1920 x 240-1080 (no mode set yet) count as 640x480. The box is drawn once
at 480-line size and scaled by height / 480 on screen (DS_DX / DT_DY = box size << 20 / screen size), so the font is the same size on every screen and
nothing is drawn twice; then it is placed:
| Anchor | x | y |
|---|---|---|
| left / centre / right | 6% of the width / (width - box) / 2 / width - 6% - box | |
| top / middle / bottom | | 10% of the height / (height - box) / 2 / height - 10% - box |

In 640x480, a one-line toast of 26 characters is 352 x 44 at (144, 388) at the bottom centre. Interlaced modes (480i, 1080i: the timing holds one field,
540 lines) need the field handling checked on hardware.

### 3.4 When and where
The extensions' run loop (spec 34 §4.3, every millisecond) drives the display: no thread of its own. Each round it shows the next toast once the title has
set its video mode, keeps it up for its time, hides it, and takes the next. **When toasts may show:** every title sets its mode through `AvSetDisplayMode`
(nxdk's `XVideoSetMode` too), so a toast waits for the title's first mode set after the title-loaded event, and half a second more (a second mode set
starts the half second again); a title that keeps the mode it was started in gets toasts 2 seconds after its start. The first version waited a fixed 5
seconds from the start: an in-game shortcut pressed 4.5 s into a game got no toast (user, 2026-11-23: "is the toast allowed purely a timeout or we know
video is ready?"). The run loop runs
whenever an extension is loaded, and only extensions queue toasts, so a toast always has it.

## 4. Actions: an extension acts while its toast is up
The kernel's toast has no action of its own (user, 2026-11-23: "all the kernel does is queue up the toast and the extension can then get notified via
callback when the toast on and when the toast is off, so it can monitor button combos and launch a title while toast is on screen"). An extension that wants
one builds it from three kernel interfaces:
1. **The toast's events** (§5): it queues its toast and keeps the `ToastId`; between that toast's "shown" and "hidden" events it watches for its combination.
2. **The pads' changes** (spec 34 §7.3, `oxos_on_pad_changed`): the pads' state comes from the USB host controllers' done lists (spec 22 §9.2), for every
   title however it was built.
3. **A title launch or a reset** (spec 34 §7.3, `oxos_title_launch`, `oxos_console_reset`): the kernel stops the title's threads first (spec 22 §9.3).

Rules for extensions that do this (the SDK documents them):
- A combination needs at least two buttons, and the toast says it in its text ("Friend request: Name. BLACK + WHITE to open").
- An extension does not offer to start the XBE that is already running (`XeImageFileName`): "BLACK + WHITE: FTP" never appears over the FTP server.
- A title's controllers stay its own: every button still reaches the title; the extension only reads.
- Games often sit in an intro without reading a pad: an action toast is best queued after the title's first pad report (the first `oxos_on_pad_changed`
  call, or `oxos_pad_state` returning TRUE), so it is not over before its combination can be seen (§4.1).

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

## 5. The exports and the SDK
Kernel exports of OXOS's own, cdecl, each the kernel's own function (spec 34 §9.1, as `oxos_file_create`): 436 `oxos_toast_enqueue` and 437
`oxos_toast_enqueue_wait` are `toasts::EnqueueToast` and `toasts::EnqueueToastWait`. The SDK's library adds `oxos_toast` (formats one line, cut to 42
characters, and queues it) and `oxos_on_toast` (registers event 5, `OXOS_EVENT_TOAST`, whose data is `{ToastId, Event}`, and unpacks it for the callback):
```cpp
// oxos_sdk.h §8: toasts over the running title (spec/oxos_internal/36).
typedef enum OXOS_TOAST_PLACE {
    OXOS_TOAST_TOP_LEFT, OXOS_TOAST_TOP_CENTRE, OXOS_TOAST_TOP_RIGHT,
    OXOS_TOAST_MIDDLE_LEFT, OXOS_TOAST_MIDDLE_CENTRE, OXOS_TOAST_MIDDLE_RIGHT,
    OXOS_TOAST_BOTTOM_LEFT, OXOS_TOAST_BOTTOM_CENTRE, OXOS_TOAST_BOTTOM_RIGHT,
} OXOS_TOAST_PLACE;

#define OXOS_TOAST_LINE_MAX 42

typedef struct OXOS_TOAST {
    const char *Text;       // one or two lines ('\n' between them) of printable ASCII, OXOS_TOAST_LINE_MAX characters each
    OXOS_TOAST_PLACE Place; // where it shows
    ULONG Seconds;          // how long it stays up, 1 to 30 seconds; 0 means the default (5 seconds)
    ULONG ToastId;          // written by the kernel when the toast is queued (0 when it was not); the id its events carry
} OXOS_TOAST;

NTSTATUS oxos_toast_enqueue(OXOS_TOAST &Toast);                        // queue it; never waits
NTSTATUS oxos_toast_enqueue_wait(OXOS_TOAST &Toast, ULONG TimeoutMs);  // queue it, waiting up to TimeoutMs for room in the queue
NTSTATUS oxos_toast(const char *Format, ...);                          // one line, bottom centre, 5 seconds: formatted like oxos_log, never waits

typedef enum OXOS_TOAST_EVENT { OXOS_TOAST_SHOWN, OXOS_TOAST_HIDDEN, OXOS_TOAST_DROPPED } OXOS_TOAST_EVENT;
BOOLEAN oxos_on_toast(void (*Callback)(ULONG ToastId, OXOS_TOAST_EVENT Event, void *UserData), void *UserData); // the extension's own toasts' events
```
| Status | When | Retry? |
|---|---|---|
| `STATUS_SUCCESS` | queued; `ToastId` is set | no |
| `STATUS_DEVICE_BUSY` | the queue is full (`oxos_toast_enqueue`) | yes, later, or with `oxos_toast_enqueue_wait` |
| `STATUS_TIMEOUT` | no room within `TimeoutMs` (`oxos_toast_enqueue_wait`) | yes |
| `STATUS_CANCELLED` | the title is ending: nothing is queued, and waits end | no |
| `STATUS_NOT_SUPPORTED` | toasts are switched off (§2) | no |
| `STATUS_INVALID_PARAMETER` | bad text, place or time; or `oxos_toast_enqueue_wait` above passive level | no: the call is wrong |

- **Copies:** the kernel copies the text, so the caller's buffer may go away at once.
- **`oxos_toast_enqueue`** may be called at any IRQL up to DISPATCH_LEVEL, from a hook too (the queue changes with interrupts off);
  **`oxos_toast_enqueue_wait`** only at passive level, from the extension's run loop or its own threads, never from a hook (a hook runs in the title's thread, and waiting there stops the
  game). It always has a timeout: it waits on an event the display sets each time it takes a toast from the queue, and tries again; when the title ends,
  the event is set and every wait returns `STATUS_CANCELLED`.
- **The owner:** each toast belongs to the extension whose code called the export (the return address, inside its image: `extensions::FindExtensionAt`),
  so the exports need no context argument; its events go to that extension only. A toast queued by an extension whose `ExtensionStart` then fails keeps no
  owner (its index goes to the next extension).
- **Events** (`oxos_on_toast`): shown when it appears, hidden when its time is up, dropped when it never showed (the title ended first, or the title held the
  overlay for the toast's whole time, §7). Called on the run-loop thread, at passive level, only for the extension's own toasts.
- The kernel's own toasts use the same queue.

**An SDK example, `sdk/examples/example_toast`:** it hooks `NtOpenFile` and `NtCreateFile` (`oxos_hook`) and pushes a toast when the title opens a file
whose name is in its ini (`extensions\example_toast.ini`, `[files]`, e.g. `save = savegame.dat`): "Saving: savegame.dat", top right, 3 seconds. It shows a hook,
the ini helpers and `oxos_toast_enqueue` together, and queues the toast from the hook (no work of its own in the title's thread beyond the name compare;
`STATUS_DEVICE_BUSY` is simply skipped).

## 6. Memory
Titles get what the kernel leaves free (spec 18 §7). The service has three parts:
| Part | Where | Cost |
|---|---|---|
| The exports and the queue (8 toasts, their text), 16 events | resident | under 1 KB of data |
| The drawing, the YUY2 conversion, the overlay registers | resident (a kernel module would be unloaded before the title runs); driven by the extensions' run loop, no thread of its own | a few hundred bytes of code; the kernel's font and `DrawText` were resident already (the error screens) |
| The picture | contiguous memory taken while a toast shows: the box in X8R8G8B8, turned into YUY2 in place | up to 147 KB (576 x 64 x 4) for the widest two-line toast, 62 KB for a one-line one of 26 characters; freed when it hides |

Decided when built (2026-11-23): resident. test_memory with the service and the in-game shortcuts' pad reading, without the in-game reset's hook: 15 974
free pages (the limit is 15 960).

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
- **Host unit tests** (`tests/unit/test_toasts.cpp`): the queue (order, a full queue, copies, refused texts, closed and switched off, events to the owner, a
  failed extension's toasts), the box and the anchors in 640x480, 720x480 and 1280x720, the YUY2 conversion; `[toasts] enabled` in `test_settings_ini.cpp`.
- **xemu** (`make test_extensions`): extension_test queues a one-second toast at every launch and has a three-line one refused; its own toast must be shown
  and hidden at least once over the launches, and no other toast's event may reach it. By hand (2026-11-23, `tools/xemu_screenshot.py`): a two-line toast
  at the bottom centre and a one-line one at the top right over the xemu dashboard, centred once the mode was read from the display-end registers (§3.3);
  the in-game shortcuts' toast before their action (spec 48).
- **Hardware:** the capture card on each encoder (Conexant, Focus, Xcalibur) in 480i, 480p, 720p and 1080i: the toast's place and text, no flicker, the game's
  frame rate unchanged; an action toast's combination starting its XBE; a title that plays video through the overlay (§7).

## 10. Open
- Which kernel events show a toast (§2): each its own decision.
- A second size, or an icon in the box (drawn in the rectangle, §3.2).
- Whether an action may start any XBE or only ones listed in a settings file.
