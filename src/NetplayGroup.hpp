#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "LinkCoordinator.hpp"

namespace Xenu
{
class Wrapper;

/// Every machine on one link cable, rolling back as one.
///
/// Rollback rewinds a core to a frame and replays it. A cabled core's state is
/// only half a conversation: rewind one end of the cable and not the other and
/// it replays a transfer the far end has already answered, after which the two
/// disagree about the wire for good. So a cabled group rewinds together -- every
/// core AND the bus between them, which holds bytes in flight and every
/// machine's clock horizon -- to the same frame, or not at all.
///
/// That needs a moment at which "frame N" is one instant on the wire, so the
/// members stop together at every frame edge: nobody starts frame N until
/// everybody has finished N-1. While they are all stopped the bus is still, and
/// that is when it is snapshotted, restored, and re-cabled. The cores must also
/// END their frames at the same instant on the wire, which a core cannot be made
/// to do from here (mednafen_lynx has lynx_fixed_frames for it).
///
/// Decisions are made by whichever member arrives last, with the group's lock
/// held, from what every member proposed on the way in; the others wait and
/// then act on the same decision. Nothing here reads a clock to decide
/// anything: which member happens to be last changes who does the work, never
/// what is decided.
struct NetplayRollbackGroup
{
    std::vector<Wrapper*> members;
    /// The bus endpoint of each member, same order: what CaptureGroup and
    /// RestoreGroup are asked about.
    std::vector<std::pair<Wrapper*, unsigned>> ports;

    /// What a member brings to a frame edge.
    struct Proposal
    {
        int64_t frame = 0;
        /// First frame whose confirmed inputs contradict what ran, or -1.
        int64_t mismatch = -1;
        /// Last frame this member has checked and found right.
        int64_t verified = -1;
        int64_t watermark = -1;
        bool can_run = false;
        /// A scheduled cable change is due at or before this frame.
        bool link_due = false;
    };
    std::vector<Proposal> proposals;

    // The decision, written by the last to arrive and read by everyone after.
    int64_t anchor = -1;
    int64_t verified = -1;
    bool run = false;

    /// The bus at the START of a frame: every member stopped at the edge before
    /// it. `cabled` false means the members were not on one bus then (before a
    /// staggered power-on has joined the cable), which is its own state.
    struct BusAt
    {
        int64_t frame = 0;
        bool cabled = false;
        std::vector<LinkCoordinator::EndpointState> state;
    };
    std::deque<BusAt> bus;
    static constexpr size_t BUS_RING = 64;

    std::mutex mutex;
    std::condition_variable cv;
    uint64_t generation = 0;
    size_t arrived = 0;
    bool broken = false;
    std::string reason;

    void Break(const std::string& why)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!broken)
            {
                broken = true;
                reason = why;
            }
        }
        cv.notify_all();
    }

    bool IsBroken()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return broken;
    }

    std::string Reason()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return reason;
    }

    /// Wait for every member. The last to arrive runs `leader` with the lock
    /// held, then all of them leave together. False if the group broke, or if
    /// `stop()` came true for this member while it waited (which breaks it:
    /// the others would otherwise wait for it for ever).
    template <class Leader, class Stop>
    bool Rendezvous(Leader&& leader, Stop&& stop)
    {
        return Rendezvous([] {}, std::forward<Leader>(leader), std::forward<Stop>(stop));
    }

    /// The same, with `arrive` run under the lock as this member arrives: how
    /// each member hands in its proposal before the last one reads them all.
    template <class Arrive, class Leader, class Stop>
    bool Rendezvous(Arrive&& arrive, Leader&& leader, Stop&& stop)
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (broken)
            return false;
        arrive();
        const uint64_t gen = generation;
        if (++arrived == members.size())
        {
            leader();
            arrived = 0;
            ++generation;
            cv.notify_all();
            return !broken;
        }
        while (generation == gen && !broken)
        {
            cv.wait_for(lock, std::chrono::milliseconds(4));
            if (generation == gen && !broken && stop())
            {
                broken = true;
                reason = "a linked machine stopped";
                cv.notify_all();
            }
        }
        return !broken;
    }

    /// Record the bus as it stands, as the start of `frame`. Caller holds the
    /// lock with every member stopped at the edge.
    void CaptureLocked(int64_t frame)
    {
        while (!bus.empty() && bus.back().frame >= frame)
            bus.pop_back();
        BusAt at;
        at.frame = frame;
        at.cabled = LinkCoordinator::Get().CaptureGroup(ports, at.state);
        bus.push_back(std::move(at));
        while (bus.size() > BUS_RING)
            bus.pop_front();
    }

    /// Put the bus back to the start of `frame`. Caller holds the lock with
    /// every member stopped at the edge.
    bool RestoreLocked(int64_t frame)
    {
        auto it = bus.begin();
        while (it != bus.end() && it->frame != frame)
            ++it;
        if (it == bus.end())
            return false;
        if (it->cabled)
        {
            if (!LinkCoordinator::Get().RestoreGroup(ports, it->state))
                return false;
        }
        else
        {
            // Uncabled then; a cable only ever moves on a confirmed frame, which
            // no rewind reaches back past, so it must be uncabled now too.
            std::vector<LinkCoordinator::EndpointState> now;
            if (LinkCoordinator::Get().CaptureGroup(ports, now))
                return false;
        }
        bus.erase(std::next(it), bus.end());
        return true;
    }
};
}
