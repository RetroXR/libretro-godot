// Standalone harness for Xrgb8888ToRgba8. Build and run with tests/run_tests.py.

#include <cstdint>
#include <cstdio>
#include <vector>

#include "PixelSwizzle.hpp"

using Xenu::Xrgb8888ToRgba8;

static int g_failures = 0;

static void Check(bool ok, const char* what)
{
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

// One XRGB8888 word the way a core lays it out: B, G, R, then the padding byte.
static uint32_t Word(uint8_t r, uint8_t g, uint8_t b, uint8_t x)
{
    return (static_cast<uint32_t>(x) << 24)
         | (static_cast<uint32_t>(r) << 16)
         | (static_cast<uint32_t>(g) << 8)
         | static_cast<uint32_t>(b);
}

static void TestPaddingBecomesOpaque()
{
    std::printf("the padding byte\n");
    // Zero is what a core leaves there, and copying it through as alpha gives a
    // frame that draws nothing wherever something blends.
    const std::vector<uint32_t> src = { Word(0x12, 0x34, 0x56, 0x00) };
    std::vector<uint8_t> dst(4, 0);
    Xrgb8888ToRgba8(dst.data(), src.data(), 1, 1, 4, 4);
    Check(dst[3] == 0xff, "an unfilled padding byte comes out opaque");
    Check(dst[0] == 0x12 && dst[1] == 0x34 && dst[2] == 0x56,
          "red, green and blue land in that order");
}

static void TestPaddingAlreadySet()
{
    std::printf("a padding byte a core did fill\n");
    const std::vector<uint32_t> src = { Word(0x10, 0x20, 0x30, 0xff) };
    std::vector<uint8_t> dst(4, 0);
    Xrgb8888ToRgba8(dst.data(), src.data(), 1, 1, 4, 4);
    Check(dst[3] == 0xff, "stays opaque");
}

static void TestRowPitch()
{
    std::printf("the source pitch\n");
    // A core's pitch is its own and is wider than the row here, so a conversion
    // that walked by width would read the padding as picture.
    const uint32_t width = 2;
    const uint32_t height = 2;
    std::vector<uint32_t> src(8, Word(0xaa, 0xaa, 0xaa, 0x00));
    src[0] = Word(0x01, 0x02, 0x03, 0x00);
    src[1] = Word(0x04, 0x05, 0x06, 0x00);
    src[4] = Word(0x07, 0x08, 0x09, 0x00);
    src[5] = Word(0x0a, 0x0b, 0x0c, 0x00);
    std::vector<uint8_t> dst(width * height * 4, 0);
    Xrgb8888ToRgba8(dst.data(), src.data(), width, height, width * 4, 4 * 4);
    Check(dst[0] == 0x01 && dst[4] == 0x04 && dst[8] == 0x07 && dst[12] == 0x0a,
          "each row is read at the source pitch");
    Check(dst[3] == 0xff && dst[7] == 0xff && dst[11] == 0xff && dst[15] == 0xff,
          "every pixel is opaque");
}

int main()
{
    TestPaddingBecomesOpaque();
    TestPaddingAlreadySet();
    TestRowPitch();
    std::printf("%s\n", g_failures == 0 ? "RESULT=PASS" : "RESULT=FAIL");
    return g_failures == 0 ? 0 : 1;
}
