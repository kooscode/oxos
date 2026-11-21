# Spec 26 — Symbolic links whose target goes past a device into a path (xboxkrnl 1.0.5838.1; 4627 compared)

Clean-room behavioural spec. Sources: Ghidra 12.1.4 analysis (REA) and objdump of the shipped binaries below. Addresses are 5838 unless marked. No code is
reproduced: behaviour, order, values and the reference's addresses as citations only. It extends spec 10 §3 (namespace, `ObInsertObject`, symbolic links),
spec 11 §2/§4 (`IoCreateSymbolicLink`, `IopParseDevice`) and spec 04 §5a (the `\Device\Harddisk0` parse); what those say is not repeated.
**Status: implemented** for a title's `IoCreateSymbolicLink`: every status of §4. OXOS's own links (drive letters, the title's `D:`) are made without the
folder open. Difference: OXOS's link keeps the target string and opens names through it from the device again, not through a kept file object (§5), so a
folder deleted after the link was made is not seen as delete-pending (§5.1).

| Image | sha256 |
|---|---|
| xboxkrnl.exe 1.0.5838.1 (primary) | 20adec79555237362a6df1838f69a12a1f7a4b614b099b6fa49889e0e96c1dde |
| xboxkrnl.exe 1.0.4627.1 | 6bf7aecdbea49ed67a44214366520b56649f6b395b75ff346d7dd3d7115c407f |

## 0. What was searched
Function entries were found from the export table by ordinal, then followed through their callees; names in brackets are descriptive, not symbols (the PDB is
not available).
| Item | 5838 | 4627 |
|---|---|---|
| `IoCreateSymbolicLink` (ord 67) | 0x80015FBE | 0x80015E2E |
| `IoDeleteSymbolicLink` (ord 69) | 0x80016037 | 0x80015EA7 |
| [link create] (called only by ord 67) | 0x80020F71 | 0x80020D17 |
| [link target resolution] | 0x80020E40 | 0x80020BE6 |
| `ObCreateObject` (239) / `ObInsertObject` (241) | 0x800213A0 / 0x800214AE | 0x80021158 / 0x80021266 |
| [name lookup by path, behind `ObOpenObjectByName` 243 at 0x800207F5] | 0x80020C4A | 0x800209F0 (243 at 0x8002059B) |
| [lookup of one element in a directory] / [split first element] | 0x80020BA6 / 0x80021951 | 0x8002094C / 0x80021709 |
| [unlink a named object from its directory] | 0x80020856 | — (not compared) |
| `NtOpenFile` (202) → `IoCreateFile` (66) | 0x80016B01 → 0x80015E24 | 0x80016976 → 0x80015C94 |
| `ObReferenceObjectByHandle` (246), `NtClose` (187), `ObfDereferenceObject` (250) | 0x8002077E, 0x800208F0, 0x800206F0 | 0x80020524, 0x80020696, 0x80020491 |
| `KeRaiseIrqlToDpcLevel` (129) / `KfLowerIrql` (161) | 0x80013D04 / 0x80013D24 | 0x80014348 / 0x80014368 |
| Object types (ord 70 device, 71 file, 240 directory, 249 symbolic link) | 0x80010A3C, 0x80010A58, 0x80010D30, 0x80010D4C | 0x80010A44, 0x80010A60, 0x80010D38, 0x80010D54 |
| Device type Parse = [IopParseDevice]; file type Parse = [IopParseFile] | 0x80018599; 0x800189B7 | 0x8001841D; 0x8001883B |
| Symbolic-link type Delete procedure (no Parse, no Close) | 0x80021091 | 0x80020E37 |
| `\Device\Harddisk0` object type ('Disk') and its Parse | 0x80012384, 0x8002498D | 0x80012394, 0x80024721 |
| [create partition device N] | 0x800248A0 | 0x80024634 |
| FATX driver object (MajorFunction from 0x80012470): Create, Cleanup, SetInformation | 0x8002A819, 0x800287A1, 0x800295FA | Create 0x8002A37A |
| FATX: [access check], [Disposition], [directory empty check] | 0x8002A4E6, 0x80029549, 0x80027BE1 | — |
| `IoSetShareAccess` / `IoCheckShareAccess` (first open / later opens) | 0x8001627C / 0x80015B87 | — |
| Globals: root directory, `\??`, `\Win32NamedObjects`; drive-letter cache (26 entries, `a`–`z` and `A`–`Z` share it) | 0x8003BBB0, 0x8003BBAC, 0x8003BBA4; 0x8003BC00 | 0x8003ABB0, … |

## 1. `IoCreateSymbolicLink(link, target)` (0x80015FBE)
1. Build object attributes {RootDirectory NULL, ObjectName = link, Attributes 0x50 = OBJ_PERMANENT | OBJ_CASE_INSENSITIVE}.
2. Call [link create] (§2) with them and the target string.
3. On success close the handle it returned (`NtClose`); the link stays because it is permanent. On failure return the status unchanged.

## 2. [link create] (0x80020F71): order of steps
| # | Step | On failure |
|---|---|---|
| 1 | **Resolve the target first** (§3) → one referenced object | return that status; no link object exists yet |
| 2 | `ObCreateObject(ObSymbolicLinkObjectType, attrs, body size 0xC + target length)` | dereference the target object; return the status |
| 3 | Fill the body (table below) | — |
| 4 | `ObInsertObject(link, attrs, bias 0, &handle)` (spec 10 §3): parent walk, collision check, `\??` drive-letter cache | the insert drops the creator's reference, the link object is deleted, and its Delete procedure (§2.1) dereferences the target object |

Body of a symbolic link (0xC bytes plus the characters):
| Offset | Field | Value |
|---|---|---|
| +0x0 | LinkTargetObject | the object §3 returned (one reference owned by the link) |
| +0x4 | LinkTarget.Length | target string length |
| +0x6 | LinkTarget.MaximumLength | the same length |
| +0x8 | LinkTarget.Buffer | body + 0xC |
| +0xC | characters | the caller's target string, copied as given (no terminator, case kept, no normalisation) |

Nothing else is stored: no split point, no remaining-path string or offset, no device pointer besides what the object itself holds.

### 2.1 Link deletion and the drive-letter cache
- Delete procedure (0x80021091): dereference LinkTargetObject. For a file object this is normally its last reference, so its Delete procedure runs (spec 11
  §4: IRP_MJ_CLOSE, device reference dropped).
- `ObInsertObject` (0x8002163F), when the parent is `\??` and the name is two characters with `:` second: cache slot of the letter ← the link's LinkTargetObject
  (not the link). The unlink routine (0x80020856) clears the slot. The cache holds no reference of its own.
- `IoDeleteSymbolicLink` (0x80016037): open the link by name (attributes 0x40), make it temporary, close the handle → unlink → Delete procedure.

## 3. [link target resolution] (0x80020E40)
Runs with IRQL raised to DISPATCH_LEVEL for the walk and lowered before any parse procedure is called.
1. Target empty, or its first character is not `\` → **STATUS_INVALID_PARAMETER (0xC000000D)**.
2. Start at the root directory. Repeat:
   1. Split the first element off the rest (the separator is dropped; a trailing `\` leaves an empty rest).
   2. The rest is not empty and starts with `\` (a doubled separator) → 0xC000000D.
   3. Look the element up in the current directory **following symbolic links** (lookup flag set for every element, the last included): a symbolic link found
      yields its LinkTargetObject; in `\??` a two-character `X:` name is first looked up in the drive-letter cache. Not found → **0xC000000D** (not
      NAME_NOT_FOUND).
   4. Rest empty → add a reference to the object found and return it: **success, the link targets that object**.
   5. The object is a directory → continue with it.
3. A non-directory object with path left over:
   - its type has no Parse procedure → 0xC000000D;
   - otherwise reference it, lower IRQL, and call its Parse procedure with: ObjectType NULL, Attributes 0x40 (OBJ_CASE_INSENSITIVE), CompleteName = the whole
     target, RemainingName = the rest **without** a leading `\`, **Context NULL** (no open packet), and the output object pointer.
4. Parse result:
   - success → that object is the link's target (the parse procedure set it, referenced);
   - **STATUS_OBJECT_TYPE_MISMATCH (0xC0000024)** → the target is a path inside a device: open it as a file (step 5);
   - any other status → returned as is.
5. **The path open:** `NtOpenFile(&handle, DesiredAccess 0, {RootDirectory NULL, ObjectName = the whole target string, OBJ_CASE_INSENSITIVE 0x40}, &iosb,
   ShareAccess 7 (read | write | delete), OpenOptions 1 = FILE_DIRECTORY_FILE)`. That is `IoCreateFile` with AllocationSize NULL, FileAttributes 0, disposition
   FILE_OPEN (1), options 0: a full name lookup from the root (spec 10 §3, §5 below), through the device's parse procedure with an open packet, the mount if the
   volume is not mounted, and the file system's create. No SYNCHRONIZE is asked and FILE_SYNCHRONOUS_IO_* is not set, so the file object is asynchronous.
   - failure → its status is the link-create status (the file system's or the mount's);
   - success → `ObReferenceObjectByHandle(handle, IoFileObjectType)` (the **file object** becomes the link target, one reference), then `NtClose(handle)`.
     The close is the file object's last handle, so **IRP_MJ_CLEANUP is sent at once**; IRP_MJ_CLOSE only follows when the link is deleted (§2.1).
6. In every case the reference taken on the parsed object in step 3 is dropped at the end.

### 3.1 Parse procedures called with no open packet (the part of step 3 that is new against spec 11)
| Parse procedure | Rest empty | Rest not empty |
|---|---|---|
| [IopParseDevice] 0x80018599 | reference the device and return it, **without** the DeletePending / DEVICE_INITIALIZING / exclusive checks and without mounting | 0xC0000024 at once (no checks, no mount) |
| [IopParseFile] 0x800189B7 | 0xC0000024 (it needs a packet) | 0xC0000024 |
| `\Device\Harddisk0` 0x8002498D (spec 04 §5a) | — (never called with an empty rest from §3: an exact `\Device\Harddisk0` target stops at step 2.4) | below |

`\Device\Harddisk0` parse with a rest, as reached from §3 (the rest has no leading `\` here; a leading `\` would be dropped by its own split the same way):
1. Split off the first element. What follows it starts with `\` → STATUS_OBJECT_NAME_INVALID (0xC0000033).
2. If something follows, or the element itself ended with `\`, one `\` is put back in front of what follows (so `Partition1\` passes `\` on, `Partition1`
   passes an empty rest).
3. Element `Partition<decimal>` (case-insensitive, 10 or more characters, digits only after `Partition`, number < 20): if the partition table slot is empty, the
   partition device is created on demand (0x800248A0: numbers 1–5 known, 0 the whole disk, 6–19 → 0xC0000034); then [IopParseDevice] of that partition with
   the rest and no packet (table above).
4. Partition creation failed with 0xC0000034: 0xC0000034 if nothing follows, else 0xC000003A; another failure status is returned as is.
5. Not a `Partition<n>` element: 0xC0000034 if nothing follows, else 0xC000003A.

## 4. Status of IoCreateSymbolicLink for each kind of target (5838 = 4627)
"OM" = decided in the object manager (§3) before any I/O; "disk namespace" = the `\Device\Harddisk0` parse; "FS" = the file system's create in step 5; "I/O" =
`IopParseDevice` / the mount during step 5.
| Target | Example | Status | Decided by | LinkTargetObject |
|---|---|---|---|---|
| empty, or not starting with `\` | `D:\x` | 0xC000000D | OM | — |
| doubled separator before the device | `\Device\\CdRom0` | 0xC000000D | OM | — |
| object-manager name missing | `\Device\CdRom9`, `\??\Q:` with no Q: | 0xC000000D | OM | — |
| path past an object whose type has no Parse | `\Win32NamedObjects\SomeEvent\x` | 0xC000000D | OM | — |
| a plain object, exactly | `\Device\CdRom0`, `\Device` (a directory), `\Device\Harddisk0` | 0 | OM | that object (device, directory, 'Disk' object …) |
| an existing link, exactly | `\??\CdRom0:` | 0 | OM | the existing link's LinkTargetObject (shared, one more reference) — never the link itself |
| a partition, exactly | `\Device\Harddisk0\Partition1` | 0 | disk namespace + IopParseDevice | the partition device (created on demand if new; not mounted) |
| unknown partition | `\Device\Harddisk0\Partition9` / `…\Partition9\x` / `…\Foo` | 0xC0000034 / 0xC000003A / 0xC0000034 | disk namespace | — |
| partition with a trailing `\` | `\Device\Harddisk0\Partition1\` | as an existing folder (the volume's root directory) | FS | file object of the root directory |
| **existing folder** | `\Device\Harddisk0\Partition1\TDATA\<id>` | 0 | FS | **file object** of that directory (cleanup already sent) |
| **last component missing** (parent exists) | `…\Partition1\TDATA\<id>` with no `<id>` | **STATUS_OBJECT_NAME_NOT_FOUND 0xC0000034** | FS (FILE_OPEN) | — |
| **a parent missing** (or a parent is a file) | `…\Partition1\TDATA\<id>` with no `TDATA` | **STATUS_OBJECT_PATH_NOT_FOUND 0xC000003A** | FS | — |
| an existing **file**, not a folder | `…\Partition1\default.xbe` | STATUS_NOT_A_DIRECTORY 0xC0000103 | FS (FILE_DIRECTORY_FILE) | — |
| invalid FATX name element, path over 250 characters, doubled `\` inside the volume path past the one FATX accepts (spec 04b) | | 0xC0000033 | FS | — |
| volume not mountable / no media | `\Device\CdRom0\x` with no disc, unformatted partition | the mount's status (e.g. STATUS_UNRECOGNIZED_VOLUME 0xC000014F) | I/O | — |
| device being deleted or still initialising, with a path | | STATUS_NO_SUCH_DEVICE 0xC000000E | I/O (packet open) | — |
| path through a path link | `\??\D:\sub` where D: targets a folder | as the FS rows, opened relative to D:'s folder (§5) | FS | new file object of `sub` |
| link name already exists (target valid) | | STATUS_OBJECT_NAME_COLLISION 0xC0000035 | OM (insert, after the target was opened) | — (target released) |
| link name's parent directory missing | `\Foo\t:` | 0xC000003A | OM (insert, after the target was opened) | — (target released) |

Notes:
- The observed case (title, `\??\t:` → `\Device\Harddisk0\Partition1\TDATA\<title id>`, 0xC000003A on the stock kernel in xemu) is the "parent missing" row: E:
  on xemu's dashboard test disk is empty (checked with `tests/fatx.py`: no `TDATA`). With `TDATA` present and only the title's folder missing the stock kernel
  returns 0xC0000034. The kernel never creates the folder.
- The same status number can come from two places: 0xC000003A from the FS (missing parent folder), from the disk namespace (unknown partition with a path) or
  from the insert (link name's parent missing).
- Missing object-manager names before the device give 0xC000000D here, unlike a normal open (0xC0000034 / 0xC000003A, spec 10 §3).

## 5. Opening a name through a path link (`\??\t:\file.txt`)
The stored string is **not** re-parsed; the open goes through the referenced file object, as a relative open:
1. Name lookup (0x80020C4A) reaches `t:` in `\??` (drive-letter cache or hash); it is a symbolic link and more path follows (or the requested type is not
   SymbolicLink), so the walk continues at its LinkTargetObject, the directory's file object.
2. That object has a Parse procedure (the file type's). Before calling a parse procedure the lookup normally puts the separator back in front of the rest
   (spec 10, trailing separator) — **except when the object is a file object**: then the rest goes without a leading `\` (`file.txt`, or empty for `\??\t:`
   and `\??\t:\`).
3. [IopParseFile] sets the open packet's RelatedFileObject = the link's file object and calls [IopParseDevice] on that file object's device (the mounted
   volume device; no mount, no exclusive check for relative opens).
4. FATX create (0x8002A819) with a related file object, in order:
   | Condition | Status |
   |---|---|
   | volume dismounted | STATUS_VOLUME_DISMOUNTED 0xC000026E |
   | related file has no FCB or its FCB is not a directory | 0xC000000D |
   | **the related directory is delete-pending or deleted** (FCB flag 0x10) | **STATUS_DELETE_PENDING 0xC0000056** |
   | empty name | the directory itself is opened again: FILE_OPEN / FILE_OPEN_IF only (else 0xC0000035), FILE_NON_DIRECTORY_FILE → 0xC00000BA |
   | name starts with `\` | 0xC0000033 |
   | directory path length (kept in the FCB) + 1 + name length > 250 | 0xC0000033 |
   | otherwise | the normal walk from that directory (spec 04b) |
5. `NtQuerySymbolicLinkObject` returns the stored string (§2), whatever happened to the folder since.

### 5.1 The folder deleted after the link was made
- The link does not stop the deletion: its open asked no access, so it recorded no share access (`IoSetShareAccess` 0x8001627C clears the record when no
  read / write / delete access is asked, and `IoCheckShareAccess` 0x80015B87 passes such opens without counting them), its cleanup already ran, and the
  Disposition handler (0x80029549) checks only read-only (0xC0000121) and "directory empty" (0x80027BE1; DIRECTORY_NOT_EMPTY 0xC0000101), not other opens.
- At the deleter's last cleanup (0x800287A1, share-access open count 0 and FCB delete-pending) the entry is deleted on disk and the FCB is unlinked from its
  parent, but it stays in memory, referenced by the link's file object, with flag 0x10 still set.
- From then on every open through the link fails with **0xC0000056**, also after a folder of the same name is created again (that is a new FCB; the link still
  holds the old one). Deleting and re-creating the link is the only way back. A dismount of the volume makes such opens fail with 0xC000026E.

## 6. 4627 against 5838
No behavioural difference found. [link create], [link target resolution] (same constants, same order, same `NtOpenFile` arguments: access 0, share 7, options
1, attributes 0x40), `IoCreateSymbolicLink` / `IoDeleteSymbolicLink`, [IopParseDevice] (the no-packet branch), [IopParseFile], the name lookup (no `\`
restored for file objects) and the `\Device\Harddisk0` parse are the same step for step at the 4627 addresses in §0. The FATX create at 0x8002A37A uses the
same set of status codes as 5838's (0xC0000002, 0xC000000D, 0xC0000022, 0xC0000033, 0xC0000034, 0xC0000035, 0xC000003A, 0xC0000056, 0xC00000BA, 0xC0000103,
0xC0000121, 0xC000026E); its branches were not compared one by one. 4627's FATX cleanup and Disposition handler were not compared.

## 7. What stays unknown
- 4627's FATX cleanup, Disposition handler and share-access routines were not traced (§5.1 is 5838 only).
- The exact mount status for each removable-media case (no disc, a tray open, an unformatted memory unit) behind a path target; only "the mount's status" is
  established.
- Whether any kernel caller other than `IoCreateSymbolicLink` creates symbolic links with path targets (the kernel's own `\??\CdRom0:` and the drive letters
  set up at boot were not followed here).
- XDVDFS / UDF behaviour of a relative open through a link to a folder on a disc (not traced; only FATX was).
