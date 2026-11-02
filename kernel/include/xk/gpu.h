// The NV2A graphics engine's default channel context (spec/oxos-internal/24-graphics-engine-init.md).
#pragma once

namespace gpu {

// A channel's graphics context image in GPU instance memory: the engine saves its state there when it leaves the
// channel and loads it back when it returns to it.
constexpr unsigned GR_CTX_SIZE = 0x36B0;    // bytes
constexpr unsigned GR_CTX_3D = 0x33C;       // the 3D (Kelvin) part starts here; before it, state the engine fills
constexpr unsigned GR_CTX_BLOCKS = 0x1A9C;  // 136 blocks of 4 dwords
constexpr unsigned GR_CTX_COMMON = 0x231C;  // 0x300 dwords
constexpr unsigned GR_CTX_PIPE = 0x2F1C;    // 0x68 + 0xD0 dwords
constexpr unsigned GR_CTX_4DWORDS = 0x33FC; // 4 dwords
constexpr unsigned GR_CTX_20DWORDS = 0x340C;

// Calls put(offset, value) for each dword of the default image that is not zero (the rest of the image is zero).
// These are the defaults the stock graphics library and Direct3D write before a channel's first context load;
// they are measured and listed in spec 24 §3.
template <class Put> constexpr void DefaultGraphicsContext(Put put) {
    put(0x000, 0x00000001);
    put(GR_CTX_3D, 0xFFFF0000);
    put(0x3A0, 0x0FFF0000);
    put(0x3A4, 0x0FFF0000);
    put(0x47C, 0x00000101);
    put(0x490, 0x00000111);
    put(0x4A8, 0x44400000);
    for (unsigned i = 0; i < 4; i++) {
        put(0x4D4 + 4 * i, 0x00030303);
        put(0x4F4 + 4 * i, 0x00080000);
        put(0x50C + 4 * i, 0x01012000);
        put(0x51C + 4 * i, 0x000105B8);
        put(0x52C + 4 * i, 0x00080008);
    }
    for (unsigned i = 0; i < 16; i++)
        put(0x55C + 4 * i, 0x07FF0000);
    put(0x5A4, 0x4B7FFFFF);
    put(0x5FC, 0x00000001);
    put(0x604, 0x00004000);
    put(0x610, 0x00000001);
    put(0x618, 0x00040000);
    put(0x61C, 0x00010000);
    for (unsigned i = 0; i < 136; i++) {
        put(GR_CTX_BLOCKS + 16 * i + 0, 0x10700FF9);
        put(GR_CTX_BLOCKS + 16 * i + 4, 0x0436086C);
        put(GR_CTX_BLOCKS + 16 * i + 8, 0x000C001B);
    }
    // Floating-point constants: 1.0, 2.0, 1.0, 0.5, 2.0, 1.0, -1.0, -1.0, 1.0.
    put(GR_CTX_COMMON + 0x380, 0x3F800000);
    put(GR_CTX_COMMON + 0x394, 0x3F800000);
    put(GR_CTX_COMMON + 0x3C0, 0x40000000);
    put(GR_CTX_COMMON + 0x3C4, 0x3F800000);
    put(GR_CTX_COMMON + 0x3C8, 0x3F000000);
    put(GR_CTX_COMMON + 0x3D0, 0x40000000);
    put(GR_CTX_COMMON + 0x3D4, 0x3F800000);
    put(GR_CTX_COMMON + 0x3DC, 0xBF800000);
    put(GR_CTX_COMMON + 0x3E4, 0xBF800000);
    put(GR_CTX_PIPE + 0x108, 0x000FE000);
    put(GR_CTX_PIPE + 0x184, 0x000003F8);
    put(GR_CTX_4DWORDS, 0x002FE000);
    for (unsigned i = 0; i < 8; i++)
        put(GR_CTX_20DWORDS + 0x10 + 4 * i, 0x001C527C);
}

} // namespace gpu
