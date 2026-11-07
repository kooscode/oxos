// The vector boot splash's renderer (kernel/src/hal/vsplash.cpp; scene format spec/oxos-internal/00-boot-splash.md
// §1.1): anti-aliased fills, fill rules, phases, gradients, the glow filter, and the checks that keep a bad scene from
// being drawn.
#include "kernel_test.h"

#include <cstdint>
#include <vector>

#include "../../kernel/src/hal/vsplash.h"

namespace {

using Bytes = std::vector<uint8_t>;

void U8(Bytes &b, unsigned v) { b.push_back(uint8_t(v)); }
void U16(Bytes &b, unsigned v) { U8(b, v & 0xFF), U8(b, v >> 8 & 0xFF); }
void I32(Bytes &b, int v) {
    for (int i = 0; i < 4; i++)
        U8(b, unsigned(v) >> (8 * i) & 0xFF);
}

struct Pt {
    int x, y; // 1/16 pixel
};

// A scene with the given paints (already encoded) and layers (already encoded).
Bytes Scene(const Bytes &paints, unsigned paintCount, const Bytes &layers, unsigned layerCount) {
    Bytes b;
    U16(b, 0), U16(b, 0), U16(b, vsplash::W), U16(b, vsplash::H);
    U16(b, paintCount);
    b.insert(b.end(), paints.begin(), paints.end());
    U16(b, layerCount);
    b.insert(b.end(), layers.begin(), layers.end());
    return b;
}

void Solid(Bytes &b, unsigned rgb, unsigned a = 255) {
    U8(b, 0), U8(b, 1);
    U16(b, 0), U8(b, rgb >> 16 & 0xFF), U8(b, rgb >> 8 & 0xFF), U8(b, rgb & 0xFF), U8(b, a);
}

void Layer(Bytes &b, unsigned phases, unsigned filter, unsigned sigma16, unsigned draws, unsigned opacity = 255) {
    U8(b, phases), U8(b, filter), U16(b, sigma16), U8(b, opacity), U8(b, 0), U16(b, draws);
}

void Draw(Bytes &b, unsigned paint, const std::vector<std::vector<Pt>> &contours, bool evenOdd = false,
          unsigned alpha = 255) {
    U16(b, paint), U8(b, alpha), U8(b, evenOdd), U16(b, unsigned(contours.size()));
    for (auto &c : contours) {
        U16(b, unsigned(c.size()));
        for (auto p : c)
            U16(b, unsigned(p.x) & 0xFFFF), U16(b, unsigned(p.y) & 0xFFFF);
    }
}

std::vector<Pt> Square(int x0, int y0, int x1, int y1, bool reverse = false) { // pixels
    std::vector<Pt> s = {{x0 * 16, y0 * 16}, {x1 * 16, y0 * 16}, {x1 * 16, y1 * 16}, {x0 * 16, y1 * 16}};
    if (reverse)
        s = {s[0], s[3], s[2], s[1]};
    return s;
}

std::vector<unsigned> Render(const Bytes &scene, vsplash::Phase phase) {
    unsigned scratch = 0;
    EXPECT_EQ(vsplash::Check(scene.data(), unsigned(scene.size()), &scratch), 0u);
    std::vector<unsigned> out(vsplash::W * vsplash::H, 0xDEADBEEF), mem(scratch / 4 + 1);
    vsplash::Render(scene.data(), unsigned(scene.size()), phase, out.data(), mem.data());
    return out;
}

unsigned At(const std::vector<unsigned> &px, int x, int y) { return px[y * vsplash::W + x]; }

} // namespace

TEST(VectorSplash, FillsAPolygonWithAntiAliasedEdges) {
    Bytes paints, layers;
    Solid(paints, 0xFFFFFF);
    Layer(layers, 3, 0, 0, 1);
    // A square from x = 10.5 to 20 and y = 10 to 20 pixels.
    Draw(layers, 0, {{{168, 160}, {320, 160}, {320, 320}, {168, 320}}});
    auto px = Render(Scene(paints, 1, layers, 1), vsplash::END);
    EXPECT_EQ(At(px, 15, 15), 0xFFFFFFu);
    EXPECT_EQ(At(px, 5, 5), 0u) << "outside: black";
    EXPECT_EQ(At(px, 21, 15), 0u);
    unsigned half = At(px, 10, 15) & 0xFF; // half covered
    EXPECT_GE(half, 120u);
    EXPECT_LE(half, 136u);
}

TEST(VectorSplash, NonZeroAndEvenOddRules) {
    Bytes paints, layers;
    Solid(paints, 0x00FF00);
    Layer(layers, 3, 0, 0, 2);
    // Left: an outer square with an inner one wound the other way (non-zero: a hole). Right: both wound the same way
    // (non-zero: filled; even-odd: a hole).
    Draw(layers, 0, {Square(10, 10, 50, 50), Square(20, 20, 40, 40, true)});
    Draw(layers, 0, {Square(100, 10, 140, 50), Square(110, 20, 130, 40)}, true);
    auto px = Render(Scene(paints, 1, layers, 1), vsplash::START);
    EXPECT_EQ(At(px, 15, 15), 0x00FF00u);
    EXPECT_EQ(At(px, 30, 30), 0u) << "non-zero: opposite winding is a hole";
    EXPECT_EQ(At(px, 105, 15), 0x00FF00u);
    EXPECT_EQ(At(px, 120, 30), 0u) << "even-odd: a hole";
}

TEST(VectorSplash, PhasesSelectTheLayersOfEachPicture) {
    Bytes paints, layers;
    Solid(paints, 0xFF0000);
    Solid(paints, 0x0000FF);
    Layer(layers, vsplash::START, 0, 0, 1);
    Draw(layers, 0, {Square(10, 10, 20, 20)});
    Layer(layers, vsplash::END, 0, 0, 1);
    Draw(layers, 1, {Square(10, 10, 20, 20)});
    auto scene = Scene(paints, 2, layers, 2);
    EXPECT_EQ(At(Render(scene, vsplash::START), 15, 15), 0xFF0000u);
    EXPECT_EQ(At(Render(scene, vsplash::END), 15, 15), 0x0000FFu);
}

TEST(VectorSplash, LinearGradientAndOpacity) {
    Bytes paints, layers;
    // t = x / 256 (65536 * x / 256 = 256 x): black at x = 0 to white at x = 256.
    U8(paints, 1), U8(paints, 2);
    I32(paints, 256), I32(paints, 0), I32(paints, 0);
    U16(paints, 0), U8(paints, 0), U8(paints, 0), U8(paints, 0), U8(paints, 255);
    U16(paints, 256), U8(paints, 255), U8(paints, 255), U8(paints, 255), U8(paints, 255);
    Layer(layers, 3, 0, 0, 1);
    Draw(layers, 0, {Square(0, 0, 300, 10)});
    auto px = Render(Scene(paints, 1, layers, 1), vsplash::END);
    EXPECT_LE(At(px, 0, 5) & 0xFF, 2u);
    EXPECT_NEAR(int(At(px, 128, 5) & 0xFF), 128, 3);
    EXPECT_EQ(At(px, 280, 5) & 0xFF, 255u) << "past the last stop: its colour";

    Bytes half;
    Layer(half, 3, 0, 0, 1);
    Draw(half, 0, {Square(0, 0, 300, 10)}, false, 128);
    EXPECT_NEAR(int(At(Render(Scene(paints, 1, half, 1), vsplash::END), 280, 5) & 0xFF), 128, 2);
}

TEST(VectorSplash, GlowSpreadsAroundTheShapeAndKeepsItSharp) {
    Bytes paints, layers;
    Solid(paints, 0x00FF00);
    Layer(layers, 3, 2, 5 * 16, 1);
    Draw(layers, 0, {Square(100, 100, 120, 120)});
    auto px = Render(Scene(paints, 1, layers, 1), vsplash::END);
    EXPECT_EQ(At(px, 110, 110), 0x00FF00u) << "the shape itself, drawn over its blur";
    unsigned near = At(px, 123, 110) >> 8 & 0xFF, far = At(px, 135, 110) >> 8 & 0xFF;
    EXPECT_GT(near, 20u) << "the glow reaches past the edge";
    EXPECT_LT(far, near);
    EXPECT_EQ(At(px, 200, 200), 0u);
}

TEST(VectorSplash, CheckRejectsBadScenes) {
    Bytes paints, layers;
    Solid(paints, 0xFFFFFF);
    Layer(layers, 3, 0, 0, 1);
    Draw(layers, 0, {Square(10, 10, 20, 20)});
    Bytes good = Scene(paints, 1, layers, 1);
    unsigned scratch = 0;
    ASSERT_EQ(vsplash::Check(good.data(), unsigned(good.size()), &scratch), 0u);
    EXPECT_GT(scratch, 0u);
    for (size_t n = 0; n < good.size(); n++)
        EXPECT_NE(vsplash::Check(good.data(), unsigned(n), &scratch), 0u) << "truncated to " << n << " bytes";
    Bytes trailing = good;
    trailing.push_back(0);
    EXPECT_NE(vsplash::Check(trailing.data(), unsigned(trailing.size()), &scratch), 0u);
    Bytes badPaint;
    Layer(badPaint, 3, 0, 0, 1);
    Draw(badPaint, 1, {Square(10, 10, 20, 20)});
    Bytes s = Scene(paints, 1, badPaint, 1);
    EXPECT_NE(vsplash::Check(s.data(), unsigned(s.size()), &scratch), 0u) << "paint index out of range";
    Bytes farAway;
    Layer(farAway, 3, 0, 0, 1);
    Draw(farAway, 0, {{{0, 0}, {20000, 0}, {0, 100}}});
    s = Scene(paints, 1, farAway, 1);
    EXPECT_NE(vsplash::Check(s.data(), unsigned(s.size()), &scratch), 0u) << "coordinates out of range";
    Bytes badClip = good;
    badClip[4] = 0x81, badClip[5] = 0x02; // x1 = 641
    EXPECT_NE(vsplash::Check(badClip.data(), unsigned(badClip.size()), &scratch), 0u);
}
