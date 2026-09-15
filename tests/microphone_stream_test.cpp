// Standalone harness for MicrophoneStream. Build and run with tests/run_tests.py.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "MicrophoneStream.hpp"

using Xenu::MicrophoneStream;

static int g_failures = 0;

static void Check(bool ok, const char* what)
{
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

struct Frame
{
    float x;
    float y;
};

static MicrophoneStream::Limits Unbounded(size_t capacity)
{
    MicrophoneStream::Limits limits;
    limits.capacity = capacity;
    return limits;
}

static std::vector<Frame> Sine(size_t count, double rate, double hz)
{
    std::vector<Frame> frames(count);
    for (size_t i = 0; i < count; ++i)
    {
        const float v = static_cast<float>(0.8 * std::sin(6.283185307179586 * hz * static_cast<double>(i) / rate));
        frames[i] = {v, v};
    }
    return frames;
}

static std::vector<int16_t> Drain(MicrophoneStream& stream)
{
    std::vector<int16_t> out(stream.Depth());
    if (!out.empty())
        stream.Read(out.data(), out.size(), true);
    return out;
}

static bool Near(uint64_t got, uint64_t want, uint64_t slack)
{
    return (got > want ? got - want : want - got) <= slack;
}

static void TestPassThrough()
{
    std::printf("T1 matching rates pass samples through unchanged\n");
    MicrophoneStream s;
    s.Configure(48000, Unbounded(1000));
    std::vector<Frame> frames(480);
    for (size_t i = 0; i < frames.size(); ++i)
    {
        const float v = static_cast<float>(i % 7) / 10.0f - 0.3f;
        frames[i] = {v, v};
    }
    s.PushStereo(frames.data(), frames.size(), 48000.0, 1.0f);
    Check(s.Produced() == 480, "480 in, 480 out");

    std::vector<int16_t> out(480);
    Check(s.Read(out.data(), out.size(), true) == 480, "a full read returns 480");
    bool same = true;
    for (size_t i = 0; i < out.size(); ++i)
        same = same && out[i] == static_cast<int16_t>(std::lround(frames[i].x * 32767.0f));
    Check(same, "every sample is the input scaled to int16");
}

static void TestDownsampleCounts()
{
    std::printf("T2 48000 -> 44100 produces the right count\n");
    MicrophoneStream s;
    s.Configure(44100, Unbounded(1000000));
    const std::vector<Frame> chunk = Sine(800, 48000.0, 440.0);
    for (int i = 0; i < 1000; ++i)
    {
        s.PushStereo(chunk.data(), chunk.size(), 48000.0, 1.0f);
        Drain(s);
    }
    Check(Near(s.Produced(), 735000, 2), "800000 frames become 735000 samples, within 2");
}

static void TestPushSizeIrrelevant()
{
    std::printf("T2b how the frames are split across pushes changes nothing\n");
    const std::vector<Frame> all = Sine(48000, 48000.0, 997.0);

    MicrophoneStream whole;
    whole.Configure(44100, Unbounded(100000));
    whole.PushStereo(all.data(), all.size(), 48000.0, 1.0f);
    const std::vector<int16_t> a = Drain(whole);

    MicrophoneStream pieces;
    pieces.Configure(44100, Unbounded(100000));
    size_t offset = 0;
    size_t size = 1;
    while (offset < all.size())
    {
        const size_t n = std::min(size, all.size() - offset);
        pieces.PushStereo(all.data() + offset, n, 48000.0, 1.0f);
        offset += n;
        size = size % 997 + 1;
    }
    const std::vector<int16_t> b = Drain(pieces);

    Check(!a.empty() && a == b, "one push and many uneven pushes give identical samples");
}

static void TestHeavyDownsampleAndUpsample()
{
    std::printf("T3 44100 -> 8000 and 8000 -> 48000 counts\n");
    MicrophoneStream down;
    down.Configure(8000, Unbounded(1000000));
    const std::vector<Frame> chunk = Sine(735, 44100.0, 300.0);
    for (int i = 0; i < 1200; ++i)
    {
        down.PushStereo(chunk.data(), chunk.size(), 44100.0, 1.0f);
        Drain(down);
    }
    Check(Near(down.Produced(), 160000, 2), "882000 frames become 160000 samples, within 2");

    MicrophoneStream up;
    up.Configure(48000, Unbounded(1000000));
    const std::vector<Frame> small = Sine(160, 8000.0, 300.0);
    for (int i = 0; i < 100; ++i)
        up.PushStereo(small.data(), small.size(), 8000.0, 1.0f);
    Check(Near(up.Produced(), 96000, 7), "16000 frames become 96000 samples, within 7");
}

static void TestCapDropsOldest()
{
    std::printf("T4 a full ring drops its oldest samples\n");
    MicrophoneStream s;
    s.Configure(1000, Unbounded(100));
    for (int k = 1; k <= 250; ++k)
    {
        const float v = static_cast<float>(k) / 32767.0f;
        const Frame f{v, v};
        s.PushStereo(&f, 1, 1000.0, 1.0f);
    }
    std::vector<int16_t> out(100);
    s.Read(out.data(), out.size(), true);
    bool newest = true;
    for (int i = 0; i < 100; ++i)
        newest = newest && out[i] == 151 + i;
    Check(newest, "the ring holds 151..250");
    Check(s.Dropped() == 150, "150 samples were dropped");
}

static void TestUnderflowPadsSilence()
{
    std::printf("T5 an underflow pads with silence and returns the full count\n");
    MicrophoneStream s;
    s.Configure(1000, Unbounded(1000));
    for (int k = 1; k <= 100; ++k)
    {
        const float v = static_cast<float>(k) / 32767.0f;
        const Frame f{v, v};
        s.PushStereo(&f, 1, 1000.0, 1.0f);
    }
    std::vector<int16_t> out(300, 0x7777);
    Check(s.Read(out.data(), out.size(), true) == 300, "a read of 300 returns 300");
    bool ok = true;
    for (int i = 0; i < 300; ++i)
        ok = ok && out[i] == (i < 100 ? i + 1 : 0);
    Check(ok, "100 real samples, then 200 of silence");
    Check(s.Underflows() == 1, "one underflow counted");
}

static void TestPriming()
{
    std::printf("T6 silence is served until the prime depth is reached\n");
    MicrophoneStream s;
    MicrophoneStream::Limits limits = Unbounded(1000);
    limits.prime = 50;
    s.Configure(1000, limits);

    auto push_range = [&](int from, int to) {
        for (int k = from; k <= to; ++k)
        {
            const float v = static_cast<float>(k) / 32767.0f;
            const Frame f{v, v};
            s.PushStereo(&f, 1, 1000.0, 1.0f);
        }
    };

    push_range(1, 30);
    std::vector<int16_t> out(10, 0x7777);
    s.Read(out.data(), out.size(), true);
    bool silent = true;
    for (int16_t v : out)
        silent = silent && v == 0;
    Check(silent, "below the prime depth a read is silence");
    Check(s.Depth() == 30, "and consumes nothing");

    push_range(31, 60);
    s.Read(out.data(), out.size(), true);
    bool real = true;
    for (int i = 0; i < 10; ++i)
        real = real && out[i] == i + 1;
    Check(real, "past the prime depth the read starts at the first sample");
}

static void TestGainAndClip()
{
    std::printf("T7 gain, clipping and non-finite input\n");
    auto one = [](Frame f, float gain) {
        MicrophoneStream s;
        s.Configure(1000, Unbounded(10));
        s.PushStereo(&f, 1, 1000.0, gain);
        int16_t out = 0x7777;
        s.Read(&out, 1, true);
        return out;
    };
    Check(one({0.5f, 0.5f}, 4.0f) == 32767, "gain past full scale clips to 32767");
    Check(one({-1.0f, -1.0f}, 3.0f) == -32767, "and to -32767 below");
    Check(one({0.25f, -0.25f}, 1.0f) == 0, "opposite channels cancel");
    Check(one({0.5f, 0.0f}, 1.0f) == 8192, "one channel is halved by the downmix");
    Check(one({std::nanf(""), 0.0f}, 1.0f) == 0, "a NaN sample becomes silence");
    Check(one({0.5f, 0.5f}, std::nanf("")) == 0, "a NaN gain becomes silence");
}

static void TestNoDeliver()
{
    std::printf("T8 a read that must not deliver is silence and consumes nothing\n");
    MicrophoneStream s;
    s.Configure(1000, Unbounded(100));
    const std::vector<Frame> frames(10, Frame{0.5f, 0.5f});
    s.PushStereo(frames.data(), frames.size(), 1000.0, 1.0f);
    std::vector<int16_t> out(5, 0x7777);
    Check(s.Read(out.data(), out.size(), false) == 5, "returns the full count");
    bool silent = true;
    for (int16_t v : out)
        silent = silent && v == 0;
    Check(silent, "every sample is silence");
    Check(s.Depth() == 10, "the ring is untouched");
}

static void TestTrim()
{
    std::printf("T9 a read that leaves too much trims back\n");
    MicrophoneStream s;
    MicrophoneStream::Limits limits = Unbounded(1000);
    limits.trim_high = 100;
    limits.trim_to = 60;
    s.Configure(1000, limits);
    for (int k = 1; k <= 300; ++k)
    {
        const float v = static_cast<float>(k) / 32767.0f;
        const Frame f{v, v};
        s.PushStereo(&f, 1, 1000.0, 1.0f);
    }
    std::vector<int16_t> out(10);
    s.Read(out.data(), out.size(), true);
    Check(s.Depth() == 60, "depth is trimmed to 60");
    int16_t next = 0;
    s.Read(&next, 1, true);
    Check(next == 241, "the oldest were dropped, so the next sample is 241");
    Check(s.Dropped() == 230, "230 samples dropped");
}

static void TestDegenerate()
{
    std::printf("T10 degenerate input does nothing\n");
    MicrophoneStream s;
    s.Configure(1000, Unbounded(100));
    const Frame f{0.5f, 0.5f};
    int16_t out = 0;
    Check(s.Read(&out, 0, true) == 0, "a read of 0 returns 0");
    s.PushStereo(&f, 1, 0.0, 1.0f);
    s.PushStereo(&f, 1, std::nan(""), 1.0f);
    s.PushStereo(&f, 0, 1000.0, 1.0f);
    s.PushStereo<Frame>(nullptr, 1, 1000.0, 1.0f);
    Check(s.Produced() == 0, "a zero or NaN rate, no frames or no buffer produce nothing");

    MicrophoneStream unconfigured;
    unconfigured.PushStereo(&f, 1, 1000.0, 1.0f);
    int16_t one = 0x7777;
    Check(unconfigured.Read(&one, 1, true) == 1 && one == 0, "an unconfigured stream reads silence");
}

static void TestLimits()
{
    std::printf("T11 per-rate limits\n");
    const MicrophoneStream::Limits l = MicrophoneStream::LimitsForRate(44100);
    Check(l.capacity == 6615 && l.prime == 1764 && l.trim_high == 4410 && l.trim_to == 2646,
          "44100 Hz gives 6615 / 1764 / 4410 / 2646");
    bool ordered = true;
    for (unsigned rate : {8000u, 11025u, 16000u, 32000u, 44100u, 48000u, 96000u})
    {
        const MicrophoneStream::Limits r = MicrophoneStream::LimitsForRate(rate);
        ordered = ordered && r.prime <= r.trim_to && r.trim_to < r.trim_high && r.trim_high < r.capacity;
    }
    Check(ordered, "prime <= trim_to < trim_high < capacity at every rate");
}

static void TestReset()
{
    std::printf("T12 Reset empties the ring and re-arms priming\n");
    MicrophoneStream s;
    MicrophoneStream::Limits limits = Unbounded(1000);
    limits.prime = 20;
    s.Configure(1000, limits);
    const std::vector<Frame> frames(50, Frame{0.5f, 0.5f});
    s.PushStereo(frames.data(), frames.size(), 1000.0, 1.0f);
    std::vector<int16_t> out(10);
    s.Read(out.data(), out.size(), true);
    Check(out[0] != 0, "primed stream delivers");

    s.Reset();
    Check(s.Depth() == 0, "Reset empties the ring");
    s.PushStereo(frames.data(), 10, 1000.0, 1.0f);
    s.Read(out.data(), out.size(), true);
    Check(out[0] == 0 && s.Depth() == 10, "and priming holds back a short buffer again");
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    TestPassThrough();
    TestDownsampleCounts();
    TestPushSizeIrrelevant();
    TestHeavyDownsampleAndUpsample();
    TestCapDropsOldest();
    TestUnderflowPadsSilence();
    TestPriming();
    TestGainAndClip();
    TestNoDeliver();
    TestTrim();
    TestDegenerate();
    TestLimits();
    TestReset();
    std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASS",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
