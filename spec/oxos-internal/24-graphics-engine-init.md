# 24 — Graphics engine brought up at a cold boot

OXOS addition, implemented 2026-11-01.
- **Code:** `kernel/src/hal/gpu.cpp` (`HalpInitGraphicsEngine`, called from `HalpInitPci` after the NV2A's PCI
  set-up) and `kernel/include/xk/gpu.h` (`gpu::DefaultGraphicsContext`, the default context image).
- **Unit tests:** `tests/unit/test_gpu.cpp`.
- **Background:**
  - spec/oxos-internal/10 §2 step 7 (the GPU reset at entry, tiles and Z compression);
  - spec/stock/22 §7 (the stock graphics device's set-up).

At a cold boot, before the splash and before any title, the kernel brings the NV2A graphics engine (PGRAPH) to a
valid live context, as a stock console's boot animation leaves it.

## 1. Why
- **The engine powers up holding junk.**
  - Its state registers and its internal tables hold power-on junk, e.g. PGRAPH 0x40005C = 0x39FEFF79.
  - Switching the engine off and on (PMC 0x200 bit 12) changes none of it: 448 registers were checked on the 1.4
    console.
- **On a stock console** the boot animation's graphics device (spec/stock/22 §7) runs before any title. It loads a
  default context image, so titles always find a valid engine. The Cerbios boot animation (an XDK title) does the
  same.
- **OXOS has no boot animation.** So the first title that creates its own graphics device meets the junk.
  - Direct3D titles cope: their first context switch only loads the image they filled.
  - nxdk's pbkit (up to 2026-11, `pb_load_gr_ctx`) saves the engine's live state over its freshly filled image on
    the first switch. With the junk, every Kelvin method then fails with "FD invalid operation" (class 0x97, first
    at method 0x1D6C), and pbkit stops at its GPU error screen.
  - This hit every pbkit app on hardware: nxdk's samples, the XBL Dashboard installer. xemu is not affected.
  - Fixed in pbkit by https://github.com/XboxDev/nxdk/pull/801, but apps built before the fix keep the bug.
- **With a valid live context**, that save is harmless. Measured: these apps work after any Direct3D title has run,
  and on a cold OXOS boot with this set-up.
- The splash does not draw with the graphics engine (it writes the frame buffer). This set-up also leaves the engine
  in a defined state for any later graphics code in the kernel.

## 2. Sequence
All NV2A registers are at 0xFD000000 + offset. The tiles and Z compression have just been cleared (spec 10 §2
step 7.3).

1. **Engines on:** PMC 0x200 ← 0xFFFFFFFF.
   - At a cold boot it reads 0x01110000 (FIFO and graphics off), and 0x13111113 after.
   - If bits 8 and 12 do not read back as set, stop here: reads of a switched-off block can hang.
2. **Graphics FIFO access off:** 0x400720 ← 0.
3. **Configuration registers:**

   | Register | Value |
   |---|---|
   | 0x400080 | 0 |
   | 0x400084 | 0x00118700 |
   | 0x400880 | 0x0008CFFF |
   | 0x40008C | 0xF3DE0479 |
   | 0x400090 | 0 |
   | 0x400094 | 4 |
   | 0x400B80 | 0x45EAD10F if PBUS 0x1218 bit 8 is set, else 0x45EAD10E |
   | 0x400B84 | 0 |
   | 0x400098 | 0x78 |
   | 0x40009C | 0x40 |
   | 0x400B88 | 0 |
   | 0x400780 (context table) | 0x17F0 |

   Then wait until idle (§4).
4. **The engine's copies of the memory set-up.** Set each register and the matching RDI entry: RDI select 0xEA,
   index register 0x400750 = select << 16 | address, data 0x400754.

   | Source (PFB) | PGRAPH register | RDI 0xEA address |
   |---|---|---|
   | tile 0x100240 + 16i | 0x400900 + 16i | 0x10 + 4i |
   | limit 0x100244 + 16i | 0x400904 + 16i | 0x30 + 4i |
   | pitch 0x100248 + 16i | 0x400908 + 16i | 0x50 + 4i |
   | Z compression 0x100300 + 4i | 0x400980 + 4i | 0x90 + 4i |
   | 0x100324 | 0x4009A0 | 0x0C |
   | 0x100200 | 0x4009A4 | 0x00 |
   | 0x100204 | 0x4009A8 | 0x04 |

   (i = 0-7.)
5. **Context switch and control:** 0x40014C-0x400158 ← 0; 0x400144 ← 0x10000000 (device enabled);
   0x400764 ← 0x08000000.
6. **The default context image** (§3), 0x36B0 bytes at instance 0x1800: PRAMIN 0x18000, written through the
   instance window at 0xFD700000.
   - It lies inside the reserved 64 KB of GPU instance memory (PRAMIN 0x10000-0x1FFFF, physical
     0x3FE0000-0x3FEFFFF, spec/stock/09 §2). Titles lay their own structures out from PRAMIN 0x10000.
   - The one-entry context table at instance 0x17F0 holds 0x1800.
   - Never write PRAMIN below 0x10000: that is the kernel's page tables.
7. **State reset:** 0x400080 ← 0x00070000, read it, ← 0, read it; RDI 0x3D addresses 0x00-0x38 ← 15 zero dwords;
   0x400084 |= 0x01000000 (cache invalidate).
8. **Load:** 0x400148 ← 0 (channel 0); 0x400784 ← 0x1800 (context pointer); 0x400788 ← 1 (load); wait until idle.
9. **No channel current**, as the stock graphics device leaves it before its first channel:
   - 0x400144 ← 0x10000100; 0x400764 ← 0x08000000;
   - 0x400720 ← 1 (FIFO access on);
   - 0x400100 ← 0xFFFFFFFF (nothing pending); 0x400140 ← 0 (interrupts off).
10. **Log (serial builds):** the PMC value before and after, and PGRAPH 0x40005C before and after. On the 1.4
    console 0x39FEFF71 becomes 0x00000040.

Only at a cold boot: a quick reboot keeps whatever the last title left, as on a stock console.

## 3. The default context image
These are the values the stock graphics library (and Direct3D, and nxdk's pbkit) writes into a channel's context
image before its first load. `gpu::DefaultGraphicsContext` lists the non-zero dwords; the rest is zero.

| Offset | Value |
|---|---|
| 0x000 | 1 |
| 0x33C (the 3D part) | 0xFFFF0000 |
| 0x3A0, 0x3A4 | 0x0FFF0000 |
| 0x47C | 0x101 |
| 0x490 | 0x111 |
| 0x4A8 | 0x44400000 |
| 0x4D4-0x4E0 | 0x00030303 |
| 0x4F4-0x500 | 0x00080000 |
| 0x50C-0x518 | 0x01012000 |
| 0x51C-0x528 | 0x000105B8 |
| 0x52C-0x538 | 0x00080008 |
| 0x55C-0x598 | 0x07FF0000 |
| 0x5A4 | 0x4B7FFFFF |
| 0x5FC | 1 |
| 0x604 | 0x4000 |
| 0x610 | 1 |
| 0x618 | 0x40000 |
| 0x61C | 0x10000 |
| 0x1A9C | 136 blocks of 16 bytes {0x10700FF9, 0x0436086C, 0x000C001B, 0} |
| 0x231C (common) + 0x380, 0x394, 0x3C4, 0x3D4 | 0x3F800000 (1.0) |
| 0x231C + 0x3C0, 0x3D0 | 0x40000000 (2.0) |
| 0x231C + 0x3C8 | 0x3F000000 (0.5) |
| 0x231C + 0x3DC, 0x3E4 | 0xBF800000 (-1.0) |
| 0x2F1C (pipe) + 0x108 | 0x000FE000 |
| 0x2F1C + 0x184 | 0x3F8 |
| 0x33FC | 0x002FE000 |
| 0x340C + 0x10-0x2C | 0x001C527C (8 dwords) |

- **Bytes 0x004-0x33B** are state the engine fills when it saves a context; they start at zero.
- **The 136 blocks are 16 bytes apart**, as the stock library writes them (Cerbios BootAnim's Direct3D at 0x4C5E6).
  nxdk's pbkit advances 32 bytes per block, so it writes only every other block.

## 4. Waiting for idle
Poll PGRAPH 0x400700 until it reads 0, for at most 1,000,000 polls (about 100 ms). On a timeout the sequence
carries on and the log says "engine not idle".

## 5. Checked
1.4 console (Modxo, serial build), cold boot, netbooted titles:
- **nxdk's `triangle` sample with an unpatched pbkit:** it runs (60 fps). Without this set-up it stopped at the GPU
  error screen.
- **The XBL Dashboard installer** (pbkit, v0.8): its "Ready to install" screen appears.
- **The boot splash** shows as before.

xemu: `make test-dash` passes.
