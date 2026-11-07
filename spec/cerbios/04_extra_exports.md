# Cerbios spec 04: exports beyond the retail 4627 set

Studied from Cerbios 3.1.0 Beta's export table (378 ordinals; retail 4627: 366).

| Ordinal | Name (public kernel export lists) | In Cerbios | Notes |
|---|---|---|---|
| 7 | DbgLoadImageSymbols | implemented (0x80027C1A) | debugger-only, absent in retail 4627; OXOS keeps it as kind `debug` in exports.tsv |
| 11 | DbgUnLoadImageSymbols | implemented (0x80027C5C) | as above |
| 367–373 | (later-kernel ordinals) | **absent** | |
| 374 | MmDbgAllocateMemory | implemented (0x80022D77) | debug-kernel memory services for the debug monitor (xbdm) |
| 375 | MmDbgFreeMemory | implemented (0x80022D96) | |
| 376 | MmDbgQueryAvailablePages | implemented (0x80022DAB) | |
| 377 | MmDbgReleaseAddress | implemented (0x80022D01) | |
| 378 | MmDbgWriteCheck | implemented (0x80022C9D) | |

These serve Debug mode (spec 14 §1: the debug monitor `xbdm.dll` is loaded into memory from MmDbgAllocateMemory).
The behaviour of each MmDbg* function (and whether it uses spec 02's second page region) is still to study.
