#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace Xenu
{
// Per VideoHandler, shared by its emulation and main threads. Disabled by
// default: no clocks, dictionaries or logging on the ordinary frame path.
struct VideoTransferStats
{
    enum Stage { READBACK_TOTAL, READBACK_WAIT, READBACK_COPY, TEXTURE_UPDATE, STAGE_COUNT };
    struct Counter
    {
        std::atomic<uint64_t> calls{0};
        std::atomic<uint64_t> total_ns{0};
        std::atomic<uint64_t> max_ns{0};

        void Add(uint64_t ns)
        {
            total_ns.fetch_add(ns, std::memory_order_relaxed);
            uint64_t peak = max_ns.load(std::memory_order_relaxed);
            while (ns > peak && !max_ns.compare_exchange_weak(peak, ns, std::memory_order_relaxed)) {}
            calls.fetch_add(1, std::memory_order_release);
        }
    };
    std::atomic<bool> enabled{false};
    std::array<Counter, STAGE_COUNT> stages;
};

class VideoTransferTimer
{
public:
    VideoTransferTimer(VideoTransferStats* stats, VideoTransferStats::Stage stage)
        : m_counter(stats && stats->enabled.load(std::memory_order_relaxed) ? &stats->stages[stage] : nullptr)
    {
        if (m_counter) m_start = Clock::now();
    }
    ~VideoTransferTimer()
    {
        if (m_counter)
            m_counter->Add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                Clock::now() - m_start).count()));
    }
    VideoTransferTimer(const VideoTransferTimer&) = delete;
    VideoTransferTimer& operator=(const VideoTransferTimer&) = delete;

private:
    using Clock = std::chrono::steady_clock;
    VideoTransferStats::Counter* m_counter;
    Clock::time_point m_start;
};
} // namespace Xenu
