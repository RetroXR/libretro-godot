#pragma once

#include <atomic>
#include <cstdint>

namespace Xenu
{
/// What one machine's audio sink last said about itself, held apart from the
/// handler that owns the sink so the machines CABLED to it can read it.
///
/// A sink is a clock: the mixer drains it at the hardware's rate, and the pacing
/// loop runs a frame when it wants audio and waits when it does not. That is one
/// machine's question. Machines on a link bus advance emulated time together, so
/// one of them asleep on a full sink holds every other one where it stands, and
/// their sinks go on draining while it sleeps. Three PlayStation 2s on an i.LINK
/// hub each slept at a different moment of every frame and each waited out the
/// other two: a retro_run that should take a few milliseconds took 25 to 35, and
/// every sink sat 20 ms below where it was being held. So the brake of a cabled
/// machine has to be the bus's, and this is the part of the brake a peer can see.
///
/// Plain atomics and no Godot, because LinkCoordinator hands these out and has to
/// stay buildable on its own. Shared by pointer because the readers are other
/// machines' emulation threads, and a machine can be switched off under them.
struct SinkClock
{
    /// How long a published reading goes on counting toward a peer's brake. A
    /// machine that is short of audio stops publishing only because it is stuck
    /// in retro_run, and its last brake has run down to "wants audio" long before
    /// this; one that is silent (no set cabled to it, a core with no sound yet)
    /// never publishes at all. Past this the reading describes neither, and a
    /// peer goes back to its own sink.
    static constexpr double k_vote_max_age_ms = 500.0;

    /// The brake as measured just after the last push, with the steady_clock
    /// reading it was taken at, in nanoseconds since the clock's epoch because
    /// time_point is not lock-free on every ABI we build for. A zero stamp means
    /// nothing has been published yet. The stamp is stored last, with release, so
    /// a non-zero one guarantees the brake beside it is the one measured with it.
    std::atomic<double>  brake_ms{0.0};
    std::atomic<int64_t> sampled_at_ns{0};

    /// Whether the sink behind this is one a brake can be read from: a voice ring
    /// held at a target fill. Godot's own generator has no target, reports "wants
    /// audio" until it is completely full, and would have a whole bus running
    /// flat out on its say-so.
    std::atomic<bool> paces{false};

    void Publish(double brake, int64_t now_ns, bool sink_paces)
    {
        brake_ms.store(brake, std::memory_order_relaxed);
        paces.store(sink_paces, std::memory_order_relaxed);
        sampled_at_ns.store(now_ns, std::memory_order_release);
    }

    /// Milliseconds since the last publish, or a negative number if there has
    /// not been one.
    double AgeMs(int64_t now_ns) const
    {
        const int64_t at = sampled_at_ns.load(std::memory_order_acquire);
        return at == 0 ? -1.0 : static_cast<double>(now_ns - at) / 1.0e6;
    }

    /// How long until this sink wants audio as of `now_ns`, 0 when it wants some
    /// now. Between pushes a sink only drains, and at the mixer's rate, so a
    /// brake measured at N ms is worth N minus the real time since.
    double RemainingMs(int64_t now_ns) const
    {
        const double age_ms = AgeMs(now_ns);
        if (age_ms < 0.0)
            return 0.0;
        const double left = brake_ms.load(std::memory_order_relaxed) - age_ms;
        return left > 0.0 ? left : 0.0;
    }

    /// Whether a peer should count this sink at all. See k_vote_max_age_ms.
    bool Votes(int64_t now_ns) const
    {
        const double age_ms = AgeMs(now_ns);
        return age_ms >= 0.0 && age_ms <= k_vote_max_age_ms
            && paces.load(std::memory_order_relaxed);
    }
};
}
