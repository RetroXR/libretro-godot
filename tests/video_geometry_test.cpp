// Standalone harness for VideoGeometry.hpp. Build and run with tests/run_tests.py.
//
// The unit is the shape of the picture: which way round it is, and which shape
// the glass should be. Neither needs a core to pin down, and both were wrong in
// ways a screen cannot see (a portrait game stretched into a 4:3 tube).

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "VideoGeometry.hpp"

using Xenu::DisplayAspect;
using Xenu::RotatedSize;
using Xenu::TransformFrame;
using Xenu::VideoGeometry;

static int g_failures = 0;

static void Check(bool ok, const char* what)
{
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

static bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

static VideoGeometry Geom(uint32_t bw, uint32_t bh, float aspect, uint32_t fw, uint32_t fh)
{
    VideoGeometry g;
    g.base_width = bw;
    g.base_height = bh;
    g.core_aspect = aspect;
    g.frame_width = fw;
    g.frame_height = fh;
    return g;
}

static void TestAspectOfALandscapeGame()
{
    std::printf("a landscape game\n");
    // CPS1: 384x224 pixels for a 4:3 monitor. The pixel ratio is 1.71; the
    // glass is 1.33, which is why a screen must not size itself from the frame.
    Check(Near(DisplayAspect(Geom(384, 224, 4.0f / 3.0f, 384, 224)), 4.0f / 3.0f),
          "the core's 4:3 wins over the 384x224 pixel ratio");
    Check(Near(DisplayAspect(Geom(384, 224, 0.0f, 384, 224)), 384.0f / 224.0f),
          "no aspect reported: the base size stands in for it");
}

static void TestAspectOfAVerticalGame()
{
    std::printf("a vertical game\n");
    // The bridge has already rotated the frame, so the pixels are portrait. A
    // core may say 3:4 (after its rotation) or 4:3 (before it); both mean the
    // same tube turned on its side.
    Check(Near(DisplayAspect(Geom(224, 384, 3.0f / 4.0f, 224, 384)), 3.0f / 4.0f),
          "a core that reports 3:4 for portrait pixels gives 3:4");
    Check(Near(DisplayAspect(Geom(384, 224, 4.0f / 3.0f, 224, 384)), 3.0f / 4.0f),
          "a core that reports 4:3 for portrait pixels still gives 3:4");
    Check(Near(DisplayAspect(Geom(224, 384, 3.0f / 4.0f, 384, 224)), 4.0f / 3.0f),
          "landscape pixels give landscape whatever the core said (cocktail flip)");
}

static void TestAspectEdges()
{
    std::printf("edges\n");
    Check(Near(DisplayAspect(VideoGeometry{}), 0.0f), "nothing known yet gives 0");
    Check(Near(DisplayAspect(Geom(0, 0, 1.5f, 0, 0)), 1.5f),
          "before the first frame the core's value is returned as it stands");
    Check(Near(DisplayAspect(Geom(256, 256, 1.0f, 256, 256)), 1.0f), "a square stays square");
    Check(Near(DisplayAspect(Geom(0, 0, 0.0f, 320, 240)), 320.0f / 240.0f),
          "only a frame known: its own ratio");
}

// An independent oracle: rotate a frame a quarter turn counter-clockwise the
// obvious way, and flip rows first.
static std::vector<uint32_t> Ccw(const std::vector<uint32_t>& in, uint32_t w, uint32_t h)
{
    std::vector<uint32_t> out(static_cast<size_t>(w) * h);
    // out is h wide and w tall; its row y is source column (w - 1 - y), read top to bottom.
    for (uint32_t y = 0; y < w; ++y)
        for (uint32_t x = 0; x < h; ++x)
            out[static_cast<size_t>(y) * h + x] = in[static_cast<size_t>(x) * w + (w - 1 - y)];
    return out;
}

static std::vector<uint32_t> Oracle(std::vector<uint32_t> in, uint32_t w, uint32_t h,
                                    uint32_t rotation, bool flip_y)
{
    if (flip_y)
    {
        std::vector<uint32_t> flipped(in.size());
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
                flipped[static_cast<size_t>(y) * w + x] = in[static_cast<size_t>(h - 1 - y) * w + x];
        in = flipped;
    }
    uint32_t cw = w, ch = h;
    for (uint32_t i = 0; i < rotation; ++i)
    {
        in = Ccw(in, cw, ch);
        const uint32_t t = cw; cw = ch; ch = t;
    }
    return in;
}

static void TestTransformMatchesTheOracle()
{
    std::printf("rotating a frame\n");
    const uint32_t w = 3, h = 2;
    // A B C / D E F, each pixel its own value.
    const std::vector<uint32_t> src = { 1, 2, 3, 4, 5, 6 };

    bool all = true;
    for (uint32_t rotation = 0; rotation < 4; ++rotation)
    {
        for (int flip = 0; flip < 2; ++flip)
        {
            uint32_t ow = 0, oh = 0;
            RotatedSize(w, h, rotation, &ow, &oh);
            std::vector<uint32_t> dst(static_cast<size_t>(ow) * oh, 0xdeadbeef);
            TransformFrame(src.data(), w, h, rotation, flip != 0, dst.data());
            if (dst != Oracle(src, w, h, rotation, flip != 0))
            {
                std::printf("    mismatch at rotation %u flip %d\n", rotation, flip);
                all = false;
            }
        }
    }
    Check(all, "every rotation, flipped and not, matches a naive quarter-turn walk");

    // The case that matters most, written out by hand: libretro rotation 1 is 90
    // degrees COUNTER-clockwise, so the right-hand column becomes the top row.
    std::vector<uint32_t> dst(6, 0);
    TransformFrame(src.data(), w, h, 1, false, dst.data());
    const std::vector<uint32_t> want = { 3, 6, 2, 5, 1, 4 };
    Check(dst == want, "rotation 1 turns the right-hand column into the top row");

    TransformFrame(src.data(), w, h, 3, false, dst.data());
    const std::vector<uint32_t> want3 = { 4, 1, 5, 2, 6, 3 };
    Check(dst == want3, "rotation 3 turns the left-hand column into the top row, bottom first");
}

static void TestRotatedSize()
{
    std::printf("the size after a turn\n");
    uint32_t w = 0, h = 0;
    RotatedSize(224, 384, 1, &w, &h);
    Check(w == 384 && h == 224, "a quarter turn swaps width and height");
    RotatedSize(224, 384, 2, &w, &h);
    Check(w == 224 && h == 384, "a half turn keeps them");
    RotatedSize(224, 384, 3, &w, &h);
    Check(w == 384 && h == 224, "three quarters swaps them");
}

int main()
{
    TestAspectOfALandscapeGame();
    TestAspectOfAVerticalGame();
    TestAspectEdges();
    TestTransformMatchesTheOracle();
    TestRotatedSize();
    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "OK" : "FAILED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
