#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace Xenu
{
// How loud a block of captured frames is, measured where the samples already
// are. GDScript cannot visit 800 frames a tick, and the machines that want a
// level -- a Famicom's Controller II is the first -- have no microphone open
// in their core to carry one, so this is deliberately not attached to a handle.
//
// Stateless by construction: the high-pass is seeded from the block's own first
// sample, so it starts matched to the signal and a block boundary contributes
// no step of its own. That is what lets one measurement be made per block with
// nothing carried between them, which in turn is what lets the one reader
// measure once and hand the answer to every machine.
//
// The filter's own output still starts from rest, so a block that opens on a
// steady tone overshoots its amplitude by about 5% once, in `peak`. The cost of
// removing that is carrying state between blocks, and nothing here needs a peak
// that precise.
struct MicrophoneLevel
{
    float rms  = 0.0f;
    float peak = 0.0f;
};

class MicrophoneLevelMeter
{
public:
    // Rumble, handling noise and a capture chain's DC offset all live below
    // this; speech does not.
    static constexpr double k_highpass_hz = 80.0;

    // `interleaved` is frame_count pairs of left and right in [-1, 1]. Godot
    // duplicates a mono device into both channels, so the two are averaged.
    static MicrophoneLevel Measure(const float* interleaved, size_t frame_count, double rate, float gain)
    {
        MicrophoneLevel level;
        if (interleaved == nullptr || frame_count == 0)
            return level;
        if (!std::isfinite(rate) || rate <= 0.0)
            return level;
        gain = std::isfinite(gain) ? std::clamp(gain, 0.0f, 64.0f) : 0.0f;
        if (gain == 0.0f)
            return level;

        const double rc    = 1.0 / (6.283185307179586 * k_highpass_hz);
        const double alpha = rc / (rc + 1.0 / rate);

        double prev_in  = static_cast<double>(interleaved[0] + interleaved[1]) * 0.5;
        double prev_out = 0.0;
        double sum      = 0.0;
        double peak     = 0.0;
        for (size_t i = 0; i < frame_count; ++i)
        {
            const double in  = static_cast<double>(interleaved[i * 2] + interleaved[i * 2 + 1]) * 0.5;
            const double out = alpha * (prev_out + in - prev_in);
            prev_in  = in;
            prev_out = out;
            sum += out * out;
            peak = std::max(peak, std::abs(out));
        }

        const double rms = std::sqrt(sum / static_cast<double>(frame_count));
        if (!std::isfinite(rms) || !std::isfinite(peak))
            return level;
        level.rms  = static_cast<float>(rms) * gain;
        level.peak = static_cast<float>(peak) * gain;
        return level;
    }
};
}
