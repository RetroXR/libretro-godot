#pragma once

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace Xenu
{
// One microphone's buffer: stereo host frames in, mono int16 at the core's rate out.
// Not thread-safe; the owner locks around every call.
class MicrophoneStream
{
public:
    struct Limits
    {
        size_t capacity  = 0;  // a push past this drops the oldest samples
        size_t prime     = 0;  // silence is served until this much is buffered
        size_t trim_high = 0;  // 0 disables the post-read trim
        size_t trim_to   = 0;
    };

    static constexpr unsigned k_capacity_ms  = 150;
    static constexpr unsigned k_prime_ms     = 40;
    static constexpr unsigned k_trim_high_ms = 100;
    static constexpr unsigned k_trim_to_ms   = 60;

    static Limits LimitsForRate(unsigned rate)
    {
        Limits limits;
        limits.capacity  = MsToSamples(k_capacity_ms, rate);
        limits.prime     = MsToSamples(k_prime_ms, rate);
        limits.trim_high = MsToSamples(k_trim_high_ms, rate);
        limits.trim_to   = MsToSamples(k_trim_to_ms, rate);
        return limits;
    }

    void Configure(unsigned target_rate, const Limits& limits)
    {
        m_rate   = target_rate;
        m_limits = limits;
        m_ring.assign(std::max<size_t>(limits.capacity, 1), 0);
        m_produced = m_dropped = m_underflows = 0;
        Reset();
    }

    void Release()
    {
        std::vector<int16_t>().swap(m_ring);
        m_rate = 0;
        Reset();
    }

    void Reset()
    {
        m_head      = 0;
        m_size      = 0;
        m_have_prev = false;
        m_prev      = 0.0f;
        m_pos       = 0.0;
        m_priming   = m_limits.prime > 0;
    }

    // StereoFrame is anything with .x and .y (godot::Vector2 in the extension).
    template <typename StereoFrame>
    void PushStereo(const StereoFrame* frames, size_t count, double source_rate, float gain)
    {
        if (m_ring.empty() || m_rate == 0 || frames == nullptr || count == 0)
            return;
        if (!std::isfinite(source_rate) || source_rate <= 0.0)
            return;
        if (!std::isfinite(gain))
            gain = 0.0f;

        const double target = static_cast<double>(m_rate);
        const bool passthrough = std::fabs(source_rate - target) < 0.5;
        const double step = source_rate / target;

        for (size_t i = 0; i < count; ++i)
        {
            float x = (static_cast<float>(frames[i].x) + static_cast<float>(frames[i].y)) * 0.5f * gain;
            if (!std::isfinite(x))
                x = 0.0f;

            if (passthrough)
            {
                Emit(x);
                m_have_prev = false;
                continue;
            }
            if (!m_have_prev)
            {
                m_prev      = x;
                m_have_prev = true;
                m_pos       = 0.0;
                continue;
            }
            while (m_pos < 1.0)
            {
                Emit(m_prev + (x - m_prev) * static_cast<float>(m_pos));
                m_pos += step;
            }
            m_pos -= 1.0;
            m_prev = x;
        }
    }

    // Always fills n samples and returns n; what the ring cannot supply is silence.
    int Read(int16_t* out, size_t n, bool deliver)
    {
        if (n > static_cast<size_t>(INT_MAX))
            n = static_cast<size_t>(INT_MAX);
        if (n == 0)
            return 0;

        size_t real = 0;
        if (deliver && !m_ring.empty())
        {
            if (m_priming && m_size >= m_limits.prime)
                m_priming = false;
            if (!m_priming)
            {
                const size_t cap = m_ring.size();
                real = std::min(n, m_size);
                const size_t first = std::min(real, cap - m_head);
                std::memcpy(out, m_ring.data() + m_head, first * sizeof(int16_t));
                std::memcpy(out + first, m_ring.data(), (real - first) * sizeof(int16_t));
                m_head = (m_head + real) % cap;
                m_size -= real;

                if (real < n)
                {
                    ++m_underflows;
                    m_priming = m_limits.prime > 0;
                }
                if (m_limits.trim_high > 0 && m_size > m_limits.trim_high)
                {
                    const size_t drop = m_size - m_limits.trim_to;
                    m_head = (m_head + drop) % cap;
                    m_size -= drop;
                    m_dropped += drop;
                }
            }
        }
        std::memset(out + real, 0, (n - real) * sizeof(int16_t));
        return static_cast<int>(n);
    }

    size_t   Depth() const { return m_size; }
    unsigned TargetRate() const { return m_rate; }
    uint64_t Produced() const { return m_produced; }
    uint64_t Dropped() const { return m_dropped; }
    uint64_t Underflows() const { return m_underflows; }

private:
    static size_t MsToSamples(unsigned ms, unsigned rate)
    {
        return static_cast<size_t>((static_cast<uint64_t>(ms) * rate + 999) / 1000);
    }

    void Emit(float v)
    {
        v = std::min(1.0f, std::max(-1.0f, v));
        const int16_t sample = static_cast<int16_t>(std::lround(v * 32767.0f));
        const size_t cap = m_ring.size();
        if (m_size == cap)
        {
            m_head = (m_head + 1) % cap;
            --m_size;
            ++m_dropped;
        }
        m_ring[(m_head + m_size) % cap] = sample;
        ++m_size;
        ++m_produced;
    }

    std::vector<int16_t> m_ring;
    size_t   m_head = 0;
    size_t   m_size = 0;
    unsigned m_rate = 0;
    Limits   m_limits;

    bool   m_priming   = false;
    bool   m_have_prev = false;
    float  m_prev      = 0.0f;
    double m_pos       = 0.0;

    uint64_t m_produced   = 0;
    uint64_t m_dropped    = 0;
    uint64_t m_underflows = 0;
};
}
