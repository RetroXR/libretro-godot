// Standalone harness for MicrophoneLevelMeter. Build and run with tests/run_tests.py.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "MicrophoneLevel.hpp"

using Xenu::MicrophoneLevel;
using Xenu::MicrophoneLevelMeter;

static int g_failures = 0;

static void Check(bool ok, const char* what)
{
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

static constexpr double kRate = 48000.0;
static constexpr double kTau  = 6.283185307179586;

// Interleaved stereo, both channels the same, as Godot delivers a mono device.
static std::vector<float> Tone(size_t frames, double hz, double amplitude, double offset = 0.0)
{
    std::vector<float> out(frames * 2);
    for (size_t i = 0; i < frames; ++i)
    {
        const float v = static_cast<float>(offset + amplitude * std::sin(kTau * hz * static_cast<double>(i) / kRate));
        out[i * 2]     = v;
        out[i * 2 + 1] = v;
    }
    return out;
}

static MicrophoneLevel Measure(const std::vector<float>& frames, float gain = 1.0f)
{
    return MicrophoneLevelMeter::Measure(frames.data(), frames.size() / 2, kRate, gain);
}

static bool Near(double a, double b, double tolerance)
{
    return std::abs(a - b) <= tolerance;
}

static void TestSilence()
{
    std::printf("silence\n");
    const std::vector<float> frames(960 * 2, 0.0f);
    const MicrophoneLevel level = Measure(frames);
    Check(level.rms == 0.0f && level.peak == 0.0f, "a silent block measures zero");
}

static void TestSpeechBandTone()
{
    std::printf("a tone in the speech band\n");
    // 960 frames is exactly 20 cycles at 1 kHz, so the block is whole cycles
    // and one sample lands on the crest.
    const MicrophoneLevel level = Measure(Tone(960, 1000.0, 0.5));
    Check(Near(level.rms, 0.5 / std::sqrt(2.0), 0.005), "rms is the amplitude over root two");
    // Within 5%: the filter's output starts from rest, so the first trough of a
    // tone that was already running overshoots once.
    Check(Near(level.peak, 0.5, 0.05), "peak is the amplitude");
    Check(level.peak > level.rms * 1.4f, "and stands above the rms");
}

static void TestRumbleIsRejected()
{
    std::printf("rumble against speech\n");
    const MicrophoneLevel rumble = Measure(Tone(9600, 20.0, 1.0));
    const MicrophoneLevel speech = Measure(Tone(9600, 1000.0, 1.0));
    const double down = 20.0 * std::log10(speech.rms / rumble.rms);
    std::printf("    20 Hz is %.1f dB below 1 kHz\n", down);
    Check(down >= 10.0, "20 Hz at the same amplitude reads at least 10 dB quieter");
    Check(rumble.rms > 0.01f, "and is attenuated rather than erased");
}

static void TestOffsetIsRemoved()
{
    std::printf("a capture chain's offset\n");
    const MicrophoneLevel dc = Measure(std::vector<float>(4800 * 2, 0.5f));
    Check(dc.rms < 1e-6f && dc.peak < 1e-6f, "a constant reads as silence");

    // The seeded filter is what makes this true: an offset is gone from the
    // first sample rather than decaying over the first block.
    const MicrophoneLevel plain  = Measure(Tone(960, 1000.0, 0.5));
    const MicrophoneLevel offset = Measure(Tone(960, 1000.0, 0.5, 0.3));
    Check(Near(offset.rms, plain.rms, plain.rms * 0.01), "and does not inflate a tone measured with it");
}

static void TestGain()
{
    std::printf("gain\n");
    const std::vector<float> frames = Tone(960, 1000.0, 0.25);
    const MicrophoneLevel unity = Measure(frames, 1.0f);
    const MicrophoneLevel twice = Measure(frames, 2.0f);
    Check(Near(twice.rms, unity.rms * 2.0, unity.rms * 0.001), "scales the rms");
    Check(Near(twice.peak, unity.peak * 2.0, unity.peak * 0.001), "and the peak");
    Check(Measure(frames, 0.0f).rms == 0.0f, "and zero silences the block");
}

static void TestBlockSizeIrrelevant()
{
    std::printf("block size\n");
    // The same signal cut into ten blocks measures the same as one long one,
    // because nothing is carried between blocks and none of them starts with a
    // step. Whole cycles either way.
    const MicrophoneLevel whole = Measure(Tone(960, 1000.0, 0.5, 0.3));
    std::vector<float> chunk = Tone(96, 1000.0, 0.5, 0.3);
    const MicrophoneLevel part = Measure(chunk);
    Check(Near(part.rms, whole.rms, whole.rms * 0.01), "a tenth of the frames reads the same level");
}

static void TestStereoIsMixed()
{
    std::printf("the channel mix\n");
    std::vector<float> opposed(960 * 2);
    const std::vector<float> mono = Tone(960, 1000.0, 0.5);
    for (size_t i = 0; i < 960; ++i)
    {
        opposed[i * 2]     = mono[i * 2];
        opposed[i * 2 + 1] = -mono[i * 2];
    }
    Check(Measure(opposed).rms < 1e-6f, "two channels in opposition cancel");

    std::vector<float> one_sided(960 * 2, 0.0f);
    for (size_t i = 0; i < 960; ++i)
        one_sided[i * 2] = mono[i * 2];
    Check(Near(Measure(one_sided).rms, Measure(mono).rms * 0.5, 0.005), "one channel alone reads half");
}

static void TestDegenerate()
{
    std::printf("degenerate input\n");
    Check(MicrophoneLevelMeter::Measure(nullptr, 100, kRate, 1.0f).rms == 0.0f, "a null block is zero");
    const std::vector<float> frames = Tone(960, 1000.0, 0.5);
    Check(MicrophoneLevelMeter::Measure(frames.data(), 0, kRate, 1.0f).rms == 0.0f, "no frames is zero");
    Check(Measure(frames, -1.0f).rms == 0.0f, "a negative gain is zero, not an inversion");
    Check(MicrophoneLevelMeter::Measure(frames.data(), 960, 0.0, 1.0f).rms == 0.0f, "a zero rate is zero");
    Check(MicrophoneLevelMeter::Measure(frames.data(), 960, std::nan(""), 1.0f).rms == 0.0f, "a NaN rate is zero");
    Check(MicrophoneLevelMeter::Measure(frames.data(), 960, kRate, std::nan("")).rms == 0.0f, "a NaN gain is zero");
    const MicrophoneLevel unity = Measure(frames, 1.0f);
    Check(Near(Measure(frames, 1e9f).peak, unity.peak * 64.0, unity.peak * 0.001),
          "an absurd gain is clamped to 64");
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    TestSilence();
    TestSpeechBandTone();
    TestRumbleIsRejected();
    TestOffsetIsRemoved();
    TestGain();
    TestBlockSizeIrrelevant();
    TestStereoIsMixed();
    TestDegenerate();
    std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASS",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
