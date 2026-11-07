// The NV2A graphics engine at a cold boot: brought to a valid live context, as a stock console's boot animation
// leaves it (spec/oxos_internal/24_graphics_engine_init.md).
#include <xk/hardware.h>
#include <xk/arch.h>
#include <xk/graphics.h>
#include <xk/serial.h>

namespace {

// The GPU instance memory window; in it, the default context image at 0x18000 (inside the reserved 64 KB) and a
// one-entry context table at 0x17F00 (units of 16 bytes). The graphics engine's indirect register table.
constexpr ULONG PRAMIN = 0x700000;
constexpr ULONG CTX_INSTANCE = 0x1800, CTX_TABLE = 0x17F0;
constexpr ULONG RDI_INDEX = 0x400750, RDI_DATA = 0x400754;

// An NV2A register (mapped at 0xFD000000).
volatile ULONG &Nv(ULONG offset) {
    return *(volatile ULONG *)(0xFD000000 + offset);
}

// Waits until the graphics engine is idle (PGRAPH 0x400700 = 0), for at most about 100 ms.
bool WaitIdle() {
    for (ULONG i = 0; i < 1000000; i++) {
        if (Nv(0x400700) == 0)
            return true;
        __asm__ volatile("pause");
    }
    return false;
}

// A write to entry `address` of the indirect register table `select`.
void RdiWrite(ULONG select, ULONG address, ULONG value) {
    Nv(RDI_INDEX) = select << 16 | (address & 0x1FFC);
    Nv(RDI_DATA) = value;
}

} // namespace

// Cold boot, after the GPU reset (hardware/chipset.cpp): the engine's set-up, then the default context loaded into it.
void HalpInitGraphicsEngine() {
    // All engines on, as the stock graphics device does first (spec/stock/22 §7 step 4). At a cold boot PFIFO and
    // PGRAPH are off (PMC 0x200 reads 0x01110000 on the 1.4 console); reading a switched-off block can hang.
    [[maybe_unused]] ULONG pmc = Nv(0x200);
    Nv(0x200) = 0xFFFFFFFF;
    if ((Nv(0x200) & 0x1100) != 0x1100) {
        XK_SERIAL("gpu: FIFO and graphics engines did not switch on (PMC %08lx -> %08lx)\n", pmc, Nv(0x200));
        return;
    }
    [[maybe_unused]] ULONG junk = Nv(0x40005C); // for the log: power-on junk before, a default value after
    Nv(0x400720) = 0; // graphics FIFO access off
    // 1. The engine's configuration registers, as the stock graphics device sets them
    // (spec/oxos_internal/24 §2 step 3).
    Nv(0x400080) = 0;
    Nv(0x400084) = 0x00118700;
    Nv(0x400880) = 0x0008CFFF;
    Nv(0x40008C) = 0xF3DE0479;
    Nv(0x400090) = 0;
    Nv(0x400094) = 4;
    Nv(0x400B80) = (Nv(0x1218) & 0x100) ? 0x45EAD10F : 0x45EAD10E;
    Nv(0x400B84) = 0;
    Nv(0x400098) = 0x78;
    Nv(0x40009C) = 0x40;
    Nv(0x400B88) = 0;
    Nv(0x400780) = CTX_TABLE;
    bool idle = WaitIdle();
    // 2. The engine's copies of the frame-buffer tiles, Z compression and memory configuration (registers and RDI
    //    table 0xEA). HalpQuiesceBusMasters (hardware/chipset.cpp) has just cleared the tiles and Z compression.
    for (ULONG i = 0; i < 8; i++) {
        ULONG tile = Nv(0x100240 + 16 * i), limit = Nv(0x100244 + 16 * i), pitch = Nv(0x100248 + 16 * i);
        Nv(0x400900 + 16 * i) = tile;
        Nv(0x400904 + 16 * i) = limit;
        Nv(0x400908 + 16 * i) = pitch;
        RdiWrite(0xEA, 0x10 + 4 * i, tile);
        RdiWrite(0xEA, 0x30 + 4 * i, limit);
        RdiWrite(0xEA, 0x50 + 4 * i, pitch);
        ULONG z = Nv(0x100300 + 4 * i);
        Nv(0x400980 + 4 * i) = z;
        RdiWrite(0xEA, 0x90 + 4 * i, z);
    }
    Nv(0x4009A0) = Nv(0x100324);
    RdiWrite(0xEA, 0x0C, Nv(0x100324));
    Nv(0x4009A4) = Nv(0x100200);
    RdiWrite(0xEA, 0x00, Nv(0x100200));
    Nv(0x4009A8) = Nv(0x100204);
    RdiWrite(0xEA, 0x04, Nv(0x100204));
    for (ULONG r = 0x40014C; r <= 0x400158; r += 4) // context switch registers
        Nv(r) = 0;
    Nv(0x400144) = 0x10000000; // context control: device enabled
    Nv(0x400764) = 0x08000000;
    // 3. The default context image (graphics.h), written through the instance window, and the context table.
    ULONG image = PRAMIN + (CTX_INSTANCE << 4);
    for (ULONG o = 0; o < gpu::GR_CTX_SIZE; o += 4)
        Nv(image + o) = 0;
    gpu::DefaultGraphicsContext([&](unsigned offset, unsigned value) { Nv(image + offset) = value; });
    Nv(PRAMIN + (CTX_TABLE << 4)) = CTX_INSTANCE;
    // 4. Load it: vertex/index/CAS state reset, RDI table 0x3D cleared, caches invalidated, then the load itself.
    Nv(0x400080) = 0x00070000;
    (void)Nv(0x400080);
    Nv(0x400080) = 0;
    (void)Nv(0x400080);
    Nv(RDI_INDEX) = 0x3D << 16;
    for (ULONG i = 0; i < 15; i++)
        Nv(RDI_DATA) = 0;
    Nv(0x400084) |= 0x01000000;
    Nv(0x400148) = 0;            // context user: channel 0
    Nv(0x400784) = CTX_INSTANCE; // channel context pointer
    Nv(0x400788) = 1;            // load
    idle = WaitIdle() && idle;
    // 5. No channel current (as the stock graphics device leaves it before its first channel), FIFO access on.
    Nv(0x400144) = 0x10000100;
    Nv(0x400764) = 0x08000000;
    Nv(0x400720) = 1;
    Nv(0x400100) = 0xFFFFFFFF; // nothing pending, interrupts stay off
    Nv(0x400140) = 0;
    XK_SERIAL("gpu: default graphics context loaded%s (PMC %08lx -> %08lx, 0x40005c %08lx -> %08lx)\n",
              idle ? "" : " (engine not idle!)", pmc, Nv(0x200), junk, Nv(0x40005C));
}
