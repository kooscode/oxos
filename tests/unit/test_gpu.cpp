// The graphics engine's default channel context (spec/oxos-internal/24): the table HalpInitGraphicsEngine loads at a
// cold boot so that titles which create their own graphics device start from a valid engine.
#include "kernel_test.h"

#include <map>

#include <xk/gpu.h>

namespace {
std::map<unsigned, unsigned> DefaultImage() {
    std::map<unsigned, unsigned> image;
    gpu::DefaultGraphicsContext([&](unsigned offset, unsigned value) {
        EXPECT_EQ(image.count(offset), 0u) << "offset " << std::hex << offset << " written twice";
        image[offset] = value;
    });
    return image;
}
} // namespace

TEST(Gpu, DefaultContextFitsTheImageAndIsDwordAligned) {
    auto image = DefaultImage();
    ASSERT_FALSE(image.empty());
    for (auto [offset, value] : image) {
        EXPECT_EQ(offset % 4, 0u) << std::hex << offset;
        EXPECT_LT(offset, gpu::GR_CTX_SIZE) << std::hex << offset;
        EXPECT_NE(value, 0u) << "zero entries are implied, offset " << std::hex << offset;
    }
}

TEST(Gpu, DefaultContextHas136ContiguousBlocks) {
    // The stock graphics library writes the 136 blocks 16 bytes apart (nxdk's pbkit writes every other one).
    auto image = DefaultImage();
    for (unsigned i = 0; i < 136; i++) {
        unsigned b = gpu::GR_CTX_BLOCKS + 16 * i;
        EXPECT_EQ(image[b], 0x10700FF9u) << i;
        EXPECT_EQ(image[b + 4], 0x0436086Cu) << i;
        EXPECT_EQ(image[b + 8], 0x000C001Bu) << i;
        EXPECT_EQ(image.count(b + 12), 0u) << i;
    }
    EXPECT_EQ(gpu::GR_CTX_BLOCKS + 136 * 16, gpu::GR_CTX_COMMON);
}

TEST(Gpu, DefaultContextKeyValues) {
    auto image = DefaultImage();
    EXPECT_EQ(image[0x000], 1u);
    EXPECT_EQ(image[gpu::GR_CTX_3D], 0xFFFF0000u);
    EXPECT_EQ(image[0x5A4], 0x4B7FFFFFu);
    EXPECT_EQ(image[gpu::GR_CTX_COMMON + 0x380], 0x3F800000u); // 1.0f
    EXPECT_EQ(image[gpu::GR_CTX_COMMON + 0x3DC], 0xBF800000u); // -1.0f
    EXPECT_EQ(image[gpu::GR_CTX_PIPE + 0x184], 0x000003F8u);
    EXPECT_EQ(image[gpu::GR_CTX_4DWORDS], 0x002FE000u);
    EXPECT_EQ(image[gpu::GR_CTX_20DWORDS + 0x2C], 0x001C527Cu);
    // The part before the 3D context is the engine's own state: nothing to preset there but the first dword.
    for (auto [offset, value] : image)
        if (offset != 0)
            EXPECT_GE(offset, gpu::GR_CTX_3D) << std::hex << offset;
}
