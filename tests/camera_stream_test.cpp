// Standalone harness for CameraStream. Build and run with tests/run_tests.py.
//
// The channel order can ONLY be pinned here. The one core that takes a camera
// today (mGBA, for the Game Boy Camera) turns the picture grey before it is
// seen, so a red and a blue channel exchanged on the way in look the same on
// its screen, and no probe against the real core can tell.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "CameraStream.hpp"

using Xenu::CameraStream;

static int g_failures = 0;

static void Check(bool ok, const char* what)
{
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

// What a core's frame_raw_framebuffer would have been handed.
struct Seen
{
    int calls = 0;
    std::vector<uint32_t> pixels;
    unsigned width = 0;
    unsigned height = 0;
    size_t pitch = 0;

    void operator()(const uint32_t* buffer, unsigned w, unsigned h, size_t p)
    {
        ++calls;
        width = w;
        height = h;
        pitch = p;
        pixels.assign(buffer, buffer + static_cast<size_t>(w) * h);
    }
};

// As mGBA's _updateCamera and its mCOLOR_XRGB8 reader take a pixel apart.
static unsigned Red(uint32_t p)   { return (p >> 16) & 0xffu; }
static unsigned Green(uint32_t p) { return (p >> 8) & 0xffu; }
static unsigned Blue(uint32_t p)  { return p & 0xffu; }

// Every pixel different from every other, and no channel equal to another's, so
// a flip, a mirror or a swapped pair of channels cannot land on the same values.
static std::vector<uint8_t> Picture(unsigned width, unsigned height, unsigned seed = 0)
{
    std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
    for (unsigned y = 0; y < height; ++y)
    {
        for (unsigned x = 0; x < width; ++x)
        {
            uint8_t* p = &rgba[(static_cast<size_t>(y) * width + x) * 4];
            p[0] = static_cast<uint8_t>(10 + x * 7 + seed);
            p[1] = static_cast<uint8_t>(90 + y * 11 + seed);
            p[2] = static_cast<uint8_t>(170 + x + y * 3 + seed);
            p[3] = 0x40;
        }
    }
    return rgba;
}

static bool AllBlank(const Seen& seen)
{
    if (seen.pixels.empty())
        return false;
    for (uint32_t p : seen.pixels)
    {
        if (p != CameraStream::k_blank)
            return false;
    }
    return true;
}

static void TestChannelsAndRows()
{
    std::printf("T1 a picture arrives as XRGB8888, top row first, at a pitch of width * 4\n");
    CameraStream s;
    s.Configure(3, 2);
    s.Start();
    const std::vector<uint8_t> rgba = Picture(3, 2);
    Check(s.Push(rgba.data(), rgba.size(), 3, 2), "the configured size is accepted");

    Seen seen;
    Check(s.Deliver(false, seen), "it is delivered");
    Check(seen.width == 3 && seen.height == 2, "at the size the core asked for");
    Check(seen.pitch == 12, "and a pitch of width * 4 bytes");

    bool channels = seen.pixels.size() == 6;
    for (unsigned y = 0; channels && y < 2; ++y)
    {
        for (unsigned x = 0; x < 3; ++x)
        {
            const uint8_t* in = &rgba[(static_cast<size_t>(y) * 3 + x) * 4];
            const uint32_t out = seen.pixels[static_cast<size_t>(y) * 3 + x];
            channels = channels && Red(out) == in[0] && Green(out) == in[1] && Blue(out) == in[2];
        }
    }
    Check(channels, "red is bits 16-23, green 8-15, blue 0-7, each pixel where it was");
    Check(seen.pixels.size() == 6 && Red(seen.pixels[0]) != Blue(seen.pixels[0]),
          "the picture can tell red from blue (or the line above proves nothing)");
    Check(seen.pixels.size() == 6 && seen.pixels[0] != seen.pixels[3] && seen.pixels[0] != seen.pixels[2],
          "and its first pixel from the row below and from the far end of its own");
}

static void TestWrongSizeRefused()
{
    std::printf("T2 anything but the core's own size is refused\n");
    CameraStream s;
    s.Configure(4, 3);
    s.Start();
    const std::vector<uint8_t> wide = Picture(5, 3);
    const std::vector<uint8_t> tall = Picture(4, 4);
    const std::vector<uint8_t> right = Picture(4, 3);
    Check(!s.Push(wide.data(), wide.size(), 5, 3), "a wider picture");
    Check(!s.Push(tall.data(), tall.size(), 4, 4), "a taller one");
    Check(!s.Push(right.data(), right.size() - 4, 4, 3), "the right size with a short buffer");
    Check(!s.Push(nullptr, right.size(), 4, 3), "no buffer at all");
    Check(s.GetStats().rejected == 4 && s.GetStats().pushed == 0, "four refusals counted, none accepted");

    Seen seen;
    s.Deliver(false, seen);
    Check(AllBlank(seen), "and the core is still holding the blank");
}

static void TestNothingBeforeStart()
{
    std::printf("T3 a core that has not started its camera is handed nothing\n");
    CameraStream s;
    s.Configure(2, 2);
    const std::vector<uint8_t> rgba = Picture(2, 2);
    s.Push(rgba.data(), rgba.size(), 2, 2);
    Seen seen;
    Check(!s.Deliver(false, seen) && seen.calls == 0, "not before start");
    Check(!s.Started(), "and it does not report started");
    s.Start();
    Check(s.Deliver(false, seen) && seen.calls == 1, "the picture that was waiting, once it starts");
    s.Stop();
    const std::vector<uint8_t> next = Picture(2, 2, 5);
    s.Push(next.data(), next.size(), 2, 2);
    Check(!s.Deliver(false, seen) && seen.calls == 1, "and nothing again after stop");
}

static void TestBlankUntilFirstPicture()
{
    std::printf("T4 before the first picture the core holds the blank, handed over once\n");
    CameraStream s;
    s.Configure(2, 2);
    s.Start();
    Seen seen;
    Check(s.Deliver(false, seen) && AllBlank(seen), "the blank");
    Check(seen.width == 2 && seen.height == 2 && seen.pitch == 8, "at the core's size and pitch");
    Check(!s.Deliver(false, seen) && seen.calls == 1, "not handed over again the next frame");
}

static void TestOncePerPicture()
{
    std::printf("T5 one picture is delivered once; a new one is delivered again\n");
    CameraStream s;
    s.Configure(2, 2);
    s.Start();
    const std::vector<uint8_t> first = Picture(2, 2);
    const std::vector<uint8_t> second = Picture(2, 2, 9);
    s.Push(first.data(), first.size(), 2, 2);
    Seen seen;
    s.Deliver(false, seen);
    Check(!s.Deliver(false, seen) && !s.Deliver(false, seen) && seen.calls == 1,
          "the same picture is not sent a second time");
    s.Push(second.data(), second.size(), 2, 2);
    Check(s.Deliver(false, seen) && seen.calls == 2, "a new one is");
    Check(!seen.pixels.empty() && Red(seen.pixels[0]) == second[0], "and it is the new one");
    Check(s.GetStats().delivered == 2 && s.GetStats().pushed == 2, "two in, two out");
}

static void TestStartRedelivers()
{
    std::printf("T6 every start redelivers: the core threw its copy away\n");
    CameraStream s;
    s.Configure(2, 2);
    Check(s.Start(), "the first start says it is the first");
    const std::vector<uint8_t> rgba = Picture(2, 2);
    s.Push(rgba.data(), rgba.size(), 2, 2);
    Seen seen;
    s.Deliver(false, seen);
    Check(!s.Start(), "a second start does not");
    Check(s.Deliver(false, seen) && seen.calls == 2, "the picture it already had is handed over again");
    Check(!seen.pixels.empty() && Red(seen.pixels[0]) == rgba[0], "the live one, not the blank");
    Check(!s.Deliver(false, seen), "once");

    CameraStream blank;
    blank.Configure(2, 2);
    blank.Start();
    Seen dark;
    blank.Deliver(true, dark);
    blank.Start();
    Check(blank.Deliver(true, dark) && dark.calls == 2 && AllBlank(dark),
          "and a silenced core is handed the blank again too");
    Check(s.GetStats().starts == 2, "starts are counted");
}

static void TestSilence()
{
    std::printf("T7 silence swaps the live picture for the blank, and back\n");
    CameraStream s;
    s.Configure(2, 2);
    s.Start();
    const std::vector<uint8_t> rgba = Picture(2, 2);
    s.Push(rgba.data(), rgba.size(), 2, 2);
    Seen seen;
    s.Deliver(false, seen);
    Check(s.Deliver(true, seen) && AllBlank(seen), "silenced: the blank");
    Check(!s.Deliver(true, seen) && seen.calls == 2, "once, however long the silence lasts");
    Check(s.Deliver(false, seen) && seen.calls == 3 && Red(seen.pixels[0]) == rgba[0],
          "lifted: the live picture again");

    // The same call sequence on a second stream, which is what two peers are.
    CameraStream peer;
    peer.Configure(2, 2);
    peer.Start();
    const std::vector<uint8_t> other = Picture(2, 2, 77);
    peer.Push(other.data(), other.size(), 2, 2);
    Seen theirs;
    peer.Deliver(true, theirs);
    Check(theirs.pixels == std::vector<uint32_t>(4, CameraStream::k_blank),
          "a silenced peer holding a DIFFERENT picture is handed the same constant");
}

static void TestReset()
{
    std::printf("T8 reset forgets the size, the picture and the counts\n");
    CameraStream s;
    s.Configure(2, 2);
    s.Start();
    const std::vector<uint8_t> rgba = Picture(2, 2);
    s.Push(rgba.data(), rgba.size(), 2, 2);
    Seen seen;
    s.Deliver(false, seen);
    s.Reset();

    unsigned width = 9;
    unsigned height = 9;
    s.Size(width, height);
    Check(width == 0 && height == 0, "no size");
    Check(!s.Started(), "not started");
    const CameraStream::Stats stats = s.GetStats();
    Check(stats.starts == 0 && stats.pushed == 0 && stats.delivered == 0 && stats.rejected == 0, "counts at zero");
    Check(!s.Push(rgba.data(), rgba.size(), 2, 2), "a push is refused until a core asks again");
    s.Start();
    Check(!s.Deliver(false, seen) && seen.calls == 1, "and a start with no size delivers nothing");

    s.Configure(2, 2);
    s.Start();
    Check(s.Deliver(false, seen) && AllBlank(seen), "the next run begins from the blank, not the last run's picture");
}

static void TestReconfigureDropsPicture()
{
    std::printf("T9 a new size drops a picture made for the old one\n");
    CameraStream s;
    s.Configure(2, 2);
    s.Start();
    const std::vector<uint8_t> small = Picture(2, 2);
    s.Push(small.data(), small.size(), 2, 2);
    s.Configure(3, 2);
    Seen seen;
    s.Deliver(false, seen);
    Check(seen.width == 3 && seen.height == 2 && seen.pixels.size() == 6 && AllBlank(seen),
          "the blank at the new size, never four pixels called six");
    Check(!s.Push(small.data(), small.size(), 2, 2), "the old size is refused from then on");
}

int main()
{
    TestChannelsAndRows();
    TestWrongSizeRefused();
    TestNothingBeforeStart();
    TestBlankUntilFirstPicture();
    TestOncePerPicture();
    TestStartRedelivers();
    TestSilence();
    TestReset();
    TestReconfigureDropsPicture();

    if (g_failures)
    {
        std::printf("\n%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("\nall checks passed\n");
    return 0;
}
