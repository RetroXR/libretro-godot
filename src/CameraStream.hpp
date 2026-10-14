#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "PixelSwizzle.hpp"

namespace Xenu
{
// One camera's pictures on their way into a core: RGBA8 in from the main thread,
// XRGB8888 out on the emulation thread, which is the only thread a core's frame
// callback may be called on (libretro's camera is a PUSH interface -- the
// frontend calls the core, not the other way round).
//
// Push, Size, Started and GetStats are the main thread's. Configure, Start, Stop
// and Deliver are the emulation thread's. Reset is for when there is none.
//
// Two rules here are a core's, and each was read out of mGBA rather than guessed:
//
//   * The size is the one the core asked for and nothing else is accepted. mGBA
//     centre-CROPS a larger picture instead of scaling it, and it copies `pitch`
//     bytes a row into a buffer it regrows only when the width or height grows,
//     so a pitch that varied between two frames would write past its heap. Every
//     delivery is therefore the configured size at a pitch of width * 4.
//   * A start redelivers. mGBA frees its copy of the picture each time it starts
//     the camera (at load and at every reset) and takes no photograph until the
//     next one arrives.
class CameraStream
{
public:
    struct Stats
    {
        uint64_t starts    = 0;
        uint64_t pushed    = 0;
        uint64_t rejected  = 0;
        uint64_t delivered = 0;
    };

    /// What a core is shown while there is no picture for it: stopped, in
    /// netplay, or before the first one has been pushed.
    static constexpr uint32_t k_blank = 0xff000000u;

    void Configure(unsigned width, unsigned height)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_width  = width;
        m_height = height;
        std::vector<uint32_t>().swap(m_pending);
        m_pending_stamp = 0;
    }

    /// True for the first start since Reset, for the caller's one log line.
    bool Start()
    {
        m_started.store(true, std::memory_order_release);
        m_redeliver.store(true, std::memory_order_release);
        return m_starts.fetch_add(1, std::memory_order_relaxed) == 0;
    }

    void Stop()
    {
        m_started.store(false, std::memory_order_release);
    }

    bool Started() const
    {
        return m_started.load(std::memory_order_acquire);
    }

    void Size(unsigned& width, unsigned& height) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        width  = m_width;
        height = m_height;
    }

    /// RGBA8, top row first, exactly the configured size. False, and counted,
    /// for anything else.
    bool Push(const uint8_t* rgba, size_t bytes, unsigned width, unsigned height)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const size_t pixels = static_cast<size_t>(width) * height;
        if (!rgba || m_width == 0 || width != m_width || height != m_height || bytes != pixels * 4)
        {
            m_rejected.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        m_pending.resize(pixels);
        const size_t stride = static_cast<size_t>(width) * 4;
        Rgba8ToXrgb8888(m_pending.data(), rgba, width, height, stride, stride);
        m_pending_stamp = ++m_next_stamp;
        m_pushed.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    /// Call before every retro_run. Hands `sink(buffer, width, height, pitch)`
    /// the picture the core should be holding, and only when that has changed:
    /// the live one, or the blank while `silenced` or before the first push.
    /// Evaluated per frame rather than on a mode change, so every peer of a
    /// netplay session is holding the same constant by the same frame.
    template <typename Sink>
    bool Deliver(bool silenced, Sink&& sink)
    {
        if (!m_started.load(std::memory_order_acquire))
            return false;
        const bool again = m_redeliver.exchange(false, std::memory_order_acq_rel);

        unsigned width = 0;
        unsigned height = 0;
        bool live = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            width  = m_width;
            height = m_height;
            if (width == 0 || height == 0)
                return false;
            live = !silenced && m_pending_stamp != 0;
            if (live)
            {
                if (!again && m_held == Held::Live && m_held_stamp == m_pending_stamp)
                    return false;
                // A copy, not a swap: the pending picture is what the next start
                // redelivers.
                m_out = m_pending;
                m_held_stamp = m_pending_stamp;
            }
        }

        const size_t pixels = static_cast<size_t>(width) * height;
        if (!live)
        {
            if (!again && m_held == Held::Blank)
                return false;
            if (m_blank.size() != pixels)
                m_blank.assign(pixels, k_blank);
        }
        m_held = live ? Held::Live : Held::Blank;
        sink(live ? m_out.data() : m_blank.data(), width, height, static_cast<size_t>(width) * 4);
        m_delivered.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void Reset()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_width = m_height = 0;
        std::vector<uint32_t>().swap(m_pending);
        std::vector<uint32_t>().swap(m_out);
        std::vector<uint32_t>().swap(m_blank);
        m_pending_stamp = 0;
        m_held = Held::Nothing;
        m_held_stamp = 0;
        m_started.store(false, std::memory_order_release);
        m_redeliver.store(false, std::memory_order_release);
        m_starts.store(0, std::memory_order_relaxed);
        m_pushed.store(0, std::memory_order_relaxed);
        m_rejected.store(0, std::memory_order_relaxed);
        m_delivered.store(0, std::memory_order_relaxed);
    }

    Stats GetStats() const
    {
        Stats stats;
        stats.starts    = m_starts.load(std::memory_order_relaxed);
        stats.pushed    = m_pushed.load(std::memory_order_relaxed);
        stats.rejected  = m_rejected.load(std::memory_order_relaxed);
        stats.delivered = m_delivered.load(std::memory_order_relaxed);
        return stats;
    }

private:
    enum class Held { Nothing, Blank, Live };

    mutable std::mutex m_mutex;         // the five below
    unsigned m_width  = 0;
    unsigned m_height = 0;
    std::vector<uint32_t> m_pending;
    uint64_t m_pending_stamp = 0;       // 0 = no picture has been pushed
    uint64_t m_next_stamp = 0;

    // Emulation thread only.
    std::vector<uint32_t> m_out;
    std::vector<uint32_t> m_blank;
    Held m_held = Held::Nothing;
    uint64_t m_held_stamp = 0;

    std::atomic<bool> m_started{false};
    std::atomic<bool> m_redeliver{false};
    std::atomic<uint64_t> m_starts{0};
    std::atomic<uint64_t> m_pushed{0};
    std::atomic<uint64_t> m_rejected{0};
    std::atomic<uint64_t> m_delivered{0};
};
}
