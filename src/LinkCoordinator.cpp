#include "LinkCoordinator.hpp"

#include <algorithm>
#include <limits>

#include "Debug.hpp"

#if !defined(NDEBUG) || defined(XENU_LINK_WAIT_DIAGNOSTICS)
#define XENU_MEASURE_LINK_WAITS 1
#else
#define XENU_MEASURE_LINK_WAITS 0
#endif

#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#endif

namespace Xenu
{
namespace
{
/// a * b / c without losing the top half of the product.
///
/// Converting a tick count between two clock rates overflows 64 bits long
/// before it overflows anything physical: a few hours of Game Boy Advance time
/// multiplied by a megahertz-scale rate is well past 2^64. The result is
/// rounded DOWN, always, because these values become grants, and a grant that
/// is a tick short only costs a rendezvous while a grant that is a tick long is
/// a desync.
uint64_t MulDiv64(uint64_t a, uint64_t b, uint64_t c)
{
    if (c == 0)
    {
        return 0;
    }
    if (a == 0 || b == 0)
    {
        return 0;
    }

#if defined(__SIZEOF_INT128__)
    const unsigned __int128 product = static_cast<unsigned __int128>(a) * b;
    const unsigned __int128 quotient = product / c;
    if (quotient > std::numeric_limits<uint64_t>::max())
    {
        return std::numeric_limits<uint64_t>::max();
    }
    return static_cast<uint64_t>(quotient);
#elif defined(_MSC_VER) && defined(_M_X64)
    uint64_t high = 0;
    const uint64_t low = _umul128(a, b, &high);
    // _udiv128 raises #DE when the quotient will not fit, so saturate instead.
    if (high >= c)
    {
        return std::numeric_limits<uint64_t>::max();
    }
    uint64_t remainder = 0;
    return _udiv128(high, low, c, &remainder);
#else
    // Portable long division, 32 bits of the product at a time.
    const uint64_t a_hi = a >> 32, a_lo = a & 0xFFFFFFFFull;
    const uint64_t b_hi = b >> 32, b_lo = b & 0xFFFFFFFFull;
    uint64_t product[4] = {a_lo * b_lo, a_lo * b_hi, a_hi * b_lo, a_hi * b_hi};
    uint64_t carry = product[0] >> 32;
    uint64_t mid = product[1] + product[2] + carry;
    uint64_t high = product[3] + (mid >> 32);
    uint64_t low = ((mid & 0xFFFFFFFFull) << 32) | (product[0] & 0xFFFFFFFFull);
    if (high >= c)
    {
        return std::numeric_limits<uint64_t>::max();
    }
    uint64_t quotient = 0, remainder = 0;
    for (int bit = 127; bit >= 0; --bit)
    {
        const uint64_t next = (bit >= 64) ? ((high >> (bit - 64)) & 1u) : ((low >> bit) & 1u);
        remainder = (remainder << 1) | next;
        if (remainder >= c)
        {
            remainder -= c;
            if (bit < 64)
            {
                quotient |= (1ull << bit);
            }
        }
    }
    return quotient;
#endif
}

/// A delta on `from`'s timeline expressed on `to`'s.
uint64_t ConvertDelta(uint64_t delta, uint64_t from_rate, uint64_t to_rate)
{
    if (from_rate == to_rate)
    {
        return delta;
    }
    return MulDiv64(delta, to_rate, from_rate);
}
}

LinkCoordinator& LinkCoordinator::Get()
{
    static LinkCoordinator coordinator;
    return coordinator;
}

// ── Trampolines ─────────────────────────────────────────────────────────────
// The libretro signatures carry no userdata, so the caller is recovered from
// the emulation thread. A core calling these from a thread the host does not
// know about gets a clean "not attached" answer rather than a crash.
namespace
{
/* Nothing here works out WHO is calling.
 *
 * The old shape recovered the caller from a thread-local set when a core's
 * emulation thread started, which holds only while a core does its work on that
 * one thread. Plenty do not: Dolphin runs its CPU on a thread of its own
 * whenever dual-core is enabled, and that is a user-facing setting, so a link
 * built on the calling thread would have worked and then silently stopped
 * working the moment somebody turned dual-core on for speed.
 *
 * The handle a core was given at attach says which port it means, and the
 * coordinator checks it against the ports it still has, under its own lock. */
retro_link_port_t *RETRO_CALLCONV LinkAttachCb(unsigned port, const char* protocol_id,
                                                uint64_t clock_rate)
{
    /* Attach is the ONE call that still has to know which core is asking, and
     * the only one a core makes before it has a handle. Cores call it while
     * loading a game, which is the emulation thread in every frontend this has
     * to work with, so the thread-local is sound here in a way it is not for
     * the calls that follow. */
    Wrapper* owner = CurrentThreadWrapper();
    if (!owner)
    {
        LogError("LinkAttach: no wrapper on this thread.");
        return nullptr;
    }
    return LinkCoordinator::Get().Attach(owner, port, protocol_id, clock_rate);
}

void RETRO_CALLCONV LinkDetachCb(retro_link_port_t *handle)
{
    LinkCoordinator::Get().Detach(handle);
}

int RETRO_CALLCONV LinkPeersCb(retro_link_port_t *handle, unsigned* count)
{
    return LinkCoordinator::Get().Peers(handle, count);
}

bool RETRO_CALLCONV LinkSendCb(retro_link_port_t *handle, uint64_t tick, unsigned to,
                               const void* buf, size_t len)
{
    return LinkCoordinator::Get().Send(handle, tick, to, buf, len);
}

bool RETRO_CALLCONV LinkRecvCb(retro_link_port_t *handle, uint64_t* tick, unsigned* from,
                               void* buf, size_t* len)
{
    return LinkCoordinator::Get().Recv(handle, tick, from, buf, len);
}

uint64_t RETRO_CALLCONV LinkAdvanceCb(retro_link_port_t *handle, uint64_t local_tick,
                                      uint64_t safe_tick, uint64_t request_tick,
                                      uint32_t* wake_flags)
{
    return LinkCoordinator::Get().Advance(handle, local_tick, safe_tick, request_tick, wake_flags);
}
}

const retro_link_interface* LinkCoordinator::Interface()
{
    static const retro_link_interface iface = {
        LinkAttachCb,
        LinkDetachCb,
        LinkPeersCb,
        LinkSendCb,
        LinkRecvCb,
        LinkAdvanceCb
    };
    return &iface;
}

// ── Endpoint bookkeeping ────────────────────────────────────────────────────

LinkCoordinator::Endpoint* LinkCoordinator::Resolve(retro_link_port_t *handle)
{
    const uint64_t id = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
    if (!id)
    {
        return nullptr;
    }
    return id < m_handles.size() ? m_handles[static_cast<size_t>(id)] : nullptr;
}

LinkCoordinator::Endpoint* LinkCoordinator::Find(Wrapper* owner, unsigned port)
{
    for (auto& ep : m_endpoints)
    {
        if (ep->owner == owner && ep->port == port)
        {
            return ep.get();
        }
    }
    return nullptr;
}

LinkCoordinator::Endpoint& LinkCoordinator::FindOrCreate(Wrapper* owner, unsigned port)
{
    if (Endpoint* existing = Find(owner, port))
    {
        return *existing;
    }
    m_endpoints.push_back(std::make_unique<Endpoint>());
    Endpoint& ep = *m_endpoints.back();
    ep.owner = owner;
    ep.port = port;
    ep.id = m_next_id++;
    ep.label = "m" + std::to_string(++m_next_label) + ":" + std::to_string(port);
    if (m_handles.size() <= ep.id)
    {
        m_handles.resize(static_cast<size_t>(ep.id + 1), nullptr);
    }
    m_handles[static_cast<size_t>(ep.id)] = &ep;
    return ep;
}

void LinkCoordinator::Detached(Endpoint& ep)
{
	++ep.topology_generation;
    ep.bus = nullptr;
    ep.index = -1;
    // A pulled cable is a pulled cable: anything still queued was addressed to
    // a machine this one is no longer wired to.
    ep.inbox.clear();
    ep.published = false;
    ep.local_delta = 0;
    ep.safe_delta = 0;
}

void LinkCoordinator::CutLinksAt(const Endpoint& ep)
{
    m_links.erase(std::remove_if(m_links.begin(), m_links.end(),
                                 [&ep](const Link& l) { return l.a == &ep || l.b == &ep; }),
                  m_links.end());
}

void LinkCoordinator::LogBusesLocked(const char* why) const
{
    std::string line = std::string(why) + ": ";
    if (m_buses.empty())
    {
        line += "nothing cabled";
    }
    for (size_t b = 0; b < m_buses.size(); ++b)
    {
        if (b)
        {
            line += " | ";
        }
        line += "bus " + std::to_string(b) + " [";
        for (size_t i = 0; i < m_buses[b]->members.size(); ++i)
        {
            const Endpoint* ep = m_buses[b]->members[i];
            if (i)
            {
                line += ", ";
            }
            // "on" means the core called attach, which is the whole question
            // when a cable looks right and the game refuses to link anyway.
            line += ep->label + " P" + std::to_string(i + 1) + (ep->attached ? " on" : " OFF");
        }
        line += "]";
    }

    // Endpoints the room has cabled that are on no bus at all, which is what a
    // lead going nowhere looks like from here.
    std::string loose;
    for (const auto& ep : m_endpoints)
    {
        if (!ep->bus)
        {
            loose += (loose.empty() ? "" : ", ") + ep->label + (ep->attached ? " on" : " OFF");
        }
    }
    if (!loose.empty())
    {
        line += "  loose: " + loose;
    }
    Log(line);
}

void LinkCoordinator::RebuildBuses()
{
    // Everything comes off first. Anything that is still joined to something
    // will be put back below, and anything that is not has just had its cable
    // pulled and should look like it.
    //
    // Two refinements, both about WHO re-anchors:
    //
    // A LOOSE endpoint re-anchors too. A machine running with no cable still
    // publishes, so it holds an origin from the first time it ever called in;
    // left alone, a cable joined later measured it from there, and the unit
    // switched on first looked as far ahead on the wire as it had been running
    // alone -- then stood still for that long waiting for the other to catch
    // up. Found by a netplay session powering two Lynxes on seven frames apart
    // and cabling them after.
    //
    // A bus whose members did not change keeps everything: its clocks, its
    // queued bytes, its topology generation. It used to be torn down and
    // re-anchored whenever ANY cable in the room moved, which moved every other
    // wire's origin to wherever each of its machines next happened to call in.
    // Harmless to a room, fatal to netplay: a lead plugged into two machines
    // nobody is playing shifted a live session's clocks on one peer only.
    struct Kept
    {
        std::vector<Endpoint*> members;
        bool published = false;
        uint64_t origin = 0;
        uint64_t local_delta = 0;
        uint64_t safe_delta = 0;
        uint64_t topology_generation = 0;
        std::deque<Message> inbox;
    };
    std::vector<std::pair<Endpoint*, Kept>> kept;
    for (auto& ep : m_endpoints)
    {
        if (ep->bus)
        {
            Kept k;
            k.members = ep->bus->members;
            k.published = ep->published;
            k.origin = ep->origin;
            k.local_delta = ep->local_delta;
            k.safe_delta = ep->safe_delta;
            k.topology_generation = ep->topology_generation;
            k.inbox = std::move(ep->inbox);
            kept.emplace_back(ep.get(), std::move(k));
        }
        Detached(*ep);
    }
    m_buses.clear();

    // Connected components over the wires. A flood fill rather than union-find:
    // there are a handful of machines in a room, and this way the members come
    // out in a stable order rather than in the order the flood happened to reach
    // them. Two peers replaying the same inputs have to agree about who is
    // player one.
    std::vector<Endpoint*> seen;
    for (auto& start_ep : m_endpoints)
    {
        Endpoint* root = start_ep.get();
        if (root->bus || std::find(seen.begin(), seen.end(), root) != seen.end())
        {
            continue;
        }

        std::vector<Endpoint*> component;
        std::vector<Endpoint*> pending{root};
        seen.push_back(root);

        while (!pending.empty())
        {
            Endpoint* here = pending.back();
            pending.pop_back();
            component.push_back(here);

            for (const Link& l : m_links)
            {
                Endpoint* other = (l.a == here) ? l.b : (l.b == here ? l.a : nullptr);
                if (!other || std::find(seen.begin(), seen.end(), other) != seen.end())
                {
                    continue;
                }
                seen.push_back(other);
                pending.push_back(other);
            }
        }

        // One machine on its own is not cabled to anything.
        if (component.size() < 2)
        {
            continue;
        }

        // By the seat the room gave each machine, which is where the player
        // numbers come from.
        //
        // NOT by how long the coordinator has known an endpoint, which is what
        // this used to be. That is a stable order and a wrong one: an endpoint
        // is created when its CORE attaches, so it ranked the machines by which
        // handheld was switched on first, and the cable had no say at all. Plug
        // the purple connector into the console you turned on second and it
        // still came out as player two. On hardware the parent is whichever unit
        // the cable pulls SI low on, so which end of the lead a machine holds is
        // the whole of the answer.
        //
        // Creation order remains the tie-break, for a bus described one wire at
        // a time through Connect rather than as a set through ConnectGroup.
        std::sort(component.begin(), component.end(),
                  [this](const Endpoint* x, const Endpoint* y) {
                      if (x->seat != y->seat && x->seat >= 0 && y->seat >= 0)
                      {
                          return x->seat < y->seat;
                      }
                      if ((x->seat >= 0) != (y->seat >= 0))
                      {
                          return x->seat >= 0;
                      }
                      auto pos = [this](const Endpoint* e) {
                          for (size_t i = 0; i < m_endpoints.size(); ++i)
                          {
                              if (m_endpoints[i].get() == e)
                              {
                                  return i;
                              }
                          }
                          return m_endpoints.size();
                      };
                      return pos(x) < pos(y);
                  });

        m_buses.push_back(std::make_unique<Bus>());
        Bus& bus = *m_buses.back();
        bus.members = component;
        bus.cv_slot = m_buses.size() - 1;
        for (size_t i = 0; i < bus.members.size(); ++i)
        {
            Endpoint* member = bus.members[i];
            member->bus = &bus;
            member->index = static_cast<int>(i);
            auto was = std::find_if(kept.begin(), kept.end(),
                                    [member](const auto& k) { return k.first == member; });
            if (was != kept.end() && was->second.members == bus.members)
            {
                // The same wire as before, in the same order: nothing moved.
                Kept& k = was->second;
                member->published = k.published;
                member->origin = k.origin;
                member->local_delta = k.local_delta;
                member->safe_delta = k.safe_delta;
                member->topology_generation = k.topology_generation;
                member->inbox = std::move(k.inbox);
                continue;
            }
			++member->topology_generation;
        }
    }

    EnsureCvSlotsLocked(m_buses.size());
}

void LinkCoordinator::EnsureCvSlotsLocked(size_t count)
{
    while (m_cv_slots.size() < count)
    {
        m_cv_slots.push_back(std::make_unique<std::condition_variable>());
        m_cv_parked.push_back(0);
    }
}

void LinkCoordinator::WakeBusLocked(const Endpoint& ep)
{
    if (!ep.bus)
    {
        return;
    }

    // One broadcast for the whole wire, NOT a notify_one per ready peer.
    //
    // Waking each peer individually was tried and measured worse: a broadcast is
    // a single FUTEX_WAKE naming a count, so it releases the whole party in one
    // syscall, while N targeted wakes cost N. It also has to work out who is
    // ready, and the ceiling scan is O(members) inside a loop over members --
    // O(N^2) under the lock on every publish.
    //
    // The bench (tests/link_bench.cpp) says what that trade is worth: at four
    // machines the targeted version cut wasted parks by 42% and still ran 61%
    // SLOWER, because in lockstep almost every peer genuinely is ready, so the
    // scan bought nothing and the extra syscalls cost real time. The wasteful
    // wake was never the expensive part; the RATE of rendezvous is.
    const size_t slot = ep.bus->cv_slot;
    if (slot < m_cv_slots.size() && m_cv_parked[slot] > 0)
    {
        ++m_counters.wakes;
        m_cv_slots[slot]->notify_all();
    }
}
void LinkCoordinator::WakeAllLocked()
{
    for (size_t i = 0; i < m_cv_slots.size(); ++i)
    {
        if (m_cv_parked[i] > 0)
        {
            ++m_counters.wakes;
            m_cv_slots[i]->notify_all();
        }
    }
}

LinkCoordinator::Counters LinkCoordinator::CountersSnapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_counters;
}

void LinkCoordinator::ResetCounters()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_counters = Counters{};
}

// ── Host side ───────────────────────────────────────────────────────────────

bool LinkCoordinator::Connect(Wrapper* a, unsigned port_a, Wrapper* b, unsigned port_b)
{
    if (!a || !b || (a == b && port_a == port_b))
    {
        LogError("Connect: a port cannot be cabled to itself.");
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    Endpoint& ea = FindOrCreate(a, port_a);
    Endpoint& eb = FindOrCreate(b, port_b);

    // Attaching is what tells the host a machine's serial hardware is live, but
    // a cable can be seated before the guest ever touches it. Refuse only a
    // genuine protocol mismatch, which is what stops a Game Boy lead being
    // pushed into something that speaks a different wire format.
    if (ea.attached && eb.attached && ea.protocol_id != eb.protocol_id)
    {
        LogError("Connect: protocol mismatch, '" + ea.protocol_id + "' vs '" + eb.protocol_id + "'.");
        return false;
    }

    // One wire per call. Seating the same cable twice must not double it up, or
    // pulling it once would leave half of it behind.
    for (const Link& l : m_links)
    {
        if ((l.a == &ea && l.b == &eb) || (l.a == &eb && l.b == &ea))
        {
            return true;
        }
    }

    // Deliberately NOT cutting whatever else these two are already joined to.
    //
    // A machine's socket does hold one plug, but that rule belongs to the room's
    // snap zones, not here. A GBA chain does not put two cables in one handheld:
    // the third machine joins through the JUNCTION on the cable, which is a
    // scene object with no core behind it and so can never be an endpoint. The
    // room walks the cables and their junctions, works out which machines end up
    // sharing one wire, and reports that set as edges. Enforcing one-link-per-
    // port here would tear the chain apart as fast as the room described it.
    m_links.push_back(Link{&ea, &eb});
    RebuildBuses();
    LogBusesLocked(("cabled " + ea.label + " to " + eb.label).c_str());

    WakeAllLocked();
    return true;
}

bool LinkCoordinator::ConnectGroup(const std::vector<std::pair<Wrapper*, unsigned>>& ports)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    std::vector<Endpoint*> group;
    for (const auto& p : ports)
    {
        if (!p.first)
        {
            continue;
        }
        Endpoint& ep = FindOrCreate(p.first, p.second);
        if (std::find(group.begin(), group.end(), &ep) == group.end())
        {
            group.push_back(&ep);
        }
    }

    if (group.size() < 2)
    {
        // Whatever this endpoint was on before, it is on no bus now.
        for (Endpoint* ep : group)
        {
            CutLinksAt(*ep);
            ep->seat = -1;
        }
        // A lead going nowhere: one machine, or the same one named twice. The
        // endpoints have already been cut loose above, which is the right state,
        // but the caller has to hear that nothing was joined or it will record a
        // link the bus never made.
        RebuildBuses();
        WakeAllLocked();
        return false;
    }

    // Validate before disturbing any existing buses. A failed attempt to plug
    // an incompatible lead must not silently unplug working links.
    std::string protocol;
    for (Endpoint* ep : group)
    {
        if (!ep->attached)
        {
            continue;
        }
        if (protocol.empty())
        {
            protocol = ep->protocol_id;
        }
        else if (ep->protocol_id != protocol)
        {
            LogError("ConnectGroup: protocol mismatch, '" + ep->protocol_id + "' vs '" + protocol + "'.");
            return false;
        }
    }

    // Whatever these were on before, they are on this now. Stated as a set
    // rather than accumulated, so a cable pulled out of the middle of a chain
    // leaves the two halves apart instead of merged.
    for (Endpoint* ep : group)
    {
        CutLinksAt(*ep);
        ep->seat = -1;
    }

    {
        // A star from the first, which is enough to make one component. The
        // shape of the tree does not matter; only who ends up reachable.
        for (size_t i = 1; i < group.size(); ++i)
        {
            m_links.push_back(Link{group.front(), group[i]});
        }

        // The order given IS the seating. The room walks the leads from the head
        // of the chain and takes its purple end first, so group.front() is the
        // machine holding the connector that owns the clock on real hardware.
        for (size_t i = 0; i < group.size(); ++i)
        {
            group[i]->seat = static_cast<int>(i);
        }
    }

    RebuildBuses();
    {
        std::string who;
        for (const Endpoint* ep : group)
        {
            who += (who.empty() ? "" : " + ") + ep->label;
        }
        LogBusesLocked(("cabled " + who).c_str());
    }
    WakeAllLocked();
    return true;
}

bool LinkCoordinator::CaptureGroup(
    const std::vector<std::pair<Wrapper*, unsigned>>& ports,
    std::vector<EndpointState>& out)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    out.clear();
    if (ports.size() < 2)
        return false;

    Bus* bus = nullptr;
    std::vector<Endpoint*> endpoints;
    endpoints.reserve(ports.size());
    for (const auto& p : ports)
    {
        Endpoint* ep = Find(p.first, p.second);
        if (!ep || !ep->attached || !ep->bus)
            return false;
        if (!bus)
            bus = ep->bus;
        else if (ep->bus != bus)
            return false;
        endpoints.push_back(ep);
    }
    if (!bus || bus->members.size() != endpoints.size())
        return false;

    out.reserve(endpoints.size());
    for (Endpoint* ep : endpoints)
    {
        EndpointState state;
        state.published = ep->published;
        state.origin = ep->origin;
        state.local_delta = ep->local_delta;
        state.safe_delta = ep->safe_delta;
        state.last_grant = ep->last_grant;
        state.inbox.assign(ep->inbox.begin(), ep->inbox.end());
        out.push_back(std::move(state));
    }
    return true;
}

bool LinkCoordinator::RestoreGroup(
    const std::vector<std::pair<Wrapper*, unsigned>>& ports,
    const std::vector<EndpointState>& states)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (ports.size() < 2 || ports.size() != states.size())
        return false;

    Bus* bus = nullptr;
    std::vector<Endpoint*> endpoints;
    endpoints.reserve(ports.size());
    for (const auto& p : ports)
    {
        Endpoint* ep = Find(p.first, p.second);
        if (!ep || !ep->attached || !ep->bus)
            return false;
        if (!bus)
            bus = ep->bus;
        else if (ep->bus != bus)
            return false;
        endpoints.push_back(ep);
    }
    if (!bus || bus->members.size() != endpoints.size())
        return false;

    for (size_t i = 0; i < endpoints.size(); ++i)
    {
        for (const Message& message : states[i].inbox)
        {
            if (message.from >= endpoints.size() || message.size > MAX_PAYLOAD)
                return false;
        }
    }

    for (size_t i = 0; i < endpoints.size(); ++i)
    {
        Endpoint& ep = *endpoints[i];
        const EndpointState& state = states[i];
        ep.published = state.published;
        ep.origin = state.origin;
        ep.local_delta = state.local_delta;
        ep.safe_delta = state.safe_delta;
        ep.last_grant = state.last_grant;
        ep.inbox.assign(state.inbox.begin(), state.inbox.end());
    }
    WakeAllLocked();
    return true;
}

void LinkCoordinator::Disconnect(Wrapper* owner, unsigned port)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (Endpoint* ep = Find(owner, port))
        {
            const std::string label = ep->label;
            CutLinksAt(*ep);
            ep->seat = -1;
            RebuildBuses();
            LogBusesLocked(("pulled the cable at " + label).c_str());
        }
        // Whoever was waiting on the other end may now be unbounded, and a chain
        // that has just been cut in the middle has two halves to wake.
        //
        // Inside the lock now, because the wake has to read which slots exist
        // and that vector is guarded like everything else here. Notifying with
        // the mutex held costs the woken thread one more trip round the lock;
        // a cable being pulled is rare enough for that not to matter.
        WakeAllLocked();
    }
}

void LinkCoordinator::ReportCostLocked(const Endpoint& ep) const
{
    if (!ep.advance_calls)
    {
        return;
    }
#if !XENU_MEASURE_LINK_WAITS
    Log("m" + std::to_string(ep.id) + ":" + std::to_string(ep.port) + " cost: " +
        std::to_string(ep.advance_calls) + " advance calls, " +
        std::to_string(ep.advance_waits) + " of them parked (release wait timing disabled)");
#else
    const double ms = static_cast<double>(ep.blocked_ns) / 1e6;
    const double per_call_us =
        static_cast<double>(ep.blocked_ns) / 1e3 / static_cast<double>(ep.advance_calls);
    Log("m" + std::to_string(ep.id) + ":" + std::to_string(ep.port) + " worst stall " +
        std::to_string(static_cast<double>(ep.worst_block_ns) / 1e6) + " ms at " +
        std::to_string(ep.worst_at_ms) + " ms in | over 100ms: " +
        std::to_string(ep.stalls_over_100ms) + " | over 20ms: " +
        std::to_string(ep.stalls_over_20ms) + " | cost: " +
        std::to_string(ep.advance_calls) + " advance calls, " +
        std::to_string(ep.advance_waits) + " of them parked, " +
        std::to_string(ms) + " ms blocked (" + std::to_string(per_call_us) +
        " us per call)");
#endif
}

void LinkCoordinator::DropOwner(Wrapper* owner)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& ep : m_endpoints)
        {
            if (ep->owner == owner)
            {
                ep->shutting_down = true;
                ReportCostLocked(*ep);
                CutLinksAt(*ep);
                m_handles[static_cast<size_t>(ep->id)] = nullptr;
            }
        }
        m_endpoints.erase(std::remove_if(m_endpoints.begin(), m_endpoints.end(),
                                         [owner](const std::unique_ptr<Endpoint>& e) { return e->owner == owner; }),
                          m_endpoints.end());
        RebuildBuses();
        LogBusesLocked("after a machine was switched off");
        WakeAllLocked();
    }
}

// ── Core side ───────────────────────────────────────────────────────────────

retro_link_port_t *LinkCoordinator::Attach(Wrapper* owner, unsigned port,
                                            const char* protocol_id, uint64_t clock_rate)
{
    if (clock_rate == 0)
    {
        LogError("Attach: clock_rate must not be zero.");
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    Endpoint& ep = FindOrCreate(owner, port);
    const std::string incoming_protocol = protocol_id ? protocol_id : "";
    // A cable may be seated before either guest attaches.  ConnectGroup cannot
    // validate that topology yet, so the later attachment must check every
    // already-live member before accepting the interface.
    if (ep.bus)
    {
        for (Endpoint* peer : ep.bus->members)
        {
            if (peer != &ep && peer->attached && peer->protocol_id != incoming_protocol)
            {
                LogError("Attach: protocol mismatch, '" + incoming_protocol + "' vs '"
                         + peer->protocol_id + "'.");
                return nullptr;
            }
        }
    }
    ep.protocol_id = incoming_protocol;
    ep.clock_rate = clock_rate;
    ep.attached = true;

    // A machine joining a live wire resets "now" for everybody on it.
    //
    // A message is placed on a peer at that peer's origin plus the delta the
    // SENDER has run since its own, so two endpoints only agree about when
    // something happened if their origins mean the same moment. They do when a
    // bus is built, because a rebuild takes every endpoint off and back on and
    // the whole party re-anchors together. They do not when a core attaches to
    // a bus that is already carrying one: the newcomer anchors at zero while
    // the machine already running is a long way past it, so everything that
    // machine says lands that far into the newcomer's future.
    //
    // Which is exactly a console switched on second. The first console raises
    // DTR into a bus with nobody on it, the announcement is dropped, and the
    // repeat it sends once the newcomer appears is stamped for a moment the
    // newcomer will not reach for another half-second of emulated time. The
    // early console sees DSR at once -- the newcomer's own announcement is in
    // ITS past -- and the late one sees nothing at all, which is a link that is
    // up at one end and down at the other. Measured on two cabled PlayStations
    // booted thirty frames apart: eight runs out of eight.
    //
    // Pulling the cable and putting it back is what a player finds, and it
    // works for the wrong reason: it rebuilds the bus, and the rebuild is what
    // re-aligns them. Doing it here means they never have to.
    if (ep.bus)
    {
        bool joined_a_live_wire = false;
        for (Endpoint* peer : ep.bus->members)
        {
            if (peer != &ep && peer->attached)
            {
                joined_a_live_wire = true;
                break;
            }
        }
        if (joined_a_live_wire)
        {
            for (Endpoint* peer : ep.bus->members)
            {
                // Not the inbox: a message already placed carries an absolute
                // tick in the receiver's own clock, which a moved origin does
                // not invalidate.
                peer->published = false;
                peer->local_delta = 0;
                peer->safe_delta = 0;
            }
        }
    }

    Log(ep.label + " attached, protocol '" + ep.protocol_id + "', " + std::to_string(clock_rate) + " Hz");
    LogBusesLocked("after attach");

    WakeAllLocked();
    return reinterpret_cast<retro_link_port_t *>(static_cast<uintptr_t>(ep.id));
}

void LinkCoordinator::Detach(retro_link_port_t *handle)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (Endpoint* ep = Resolve(handle))
        {
            ep->attached = false;
            // The cable is untouched: the guest simply stopped driving its
            // serial hardware. Leave the bus membership alone so the link comes
            // straight back when the game re-enables the port, and only stop
            // holding peers up in the meantime.
            ep->published = false;
            ep->local_delta = 0;
            ep->safe_delta = 0;
            ep->inbox.clear();
            Log(ep->label + " detached; the guest stopped driving its serial port");
            LogBusesLocked("after detach");
        }
        WakeAllLocked();
    }
}

int LinkCoordinator::Peers(retro_link_port_t *handle, unsigned* count)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    Endpoint* ep = Resolve(handle);
    if (!ep || !ep->attached || !ep->bus)
    {
        if (count)
        {
            *count = 0;
        }
        return -1;
    }

    if (count)
    {
        unsigned live = 0;
        for (const Endpoint* member : ep->bus->members)
        {
            if (member->attached)
            {
                ++live;
            }
        }
        *count = live;
    }
    return ep->index;
}

int LinkCoordinator::PeersFor(Wrapper* owner, unsigned port, unsigned* count)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    Endpoint* ep = Find(owner, port);
    if (!ep || !ep->attached || !ep->bus)
    {
        if (count)
        {
            *count = 0;
        }
        return -1;
    }

    if (count)
    {
        unsigned live = 0;
        for (const Endpoint* member : ep->bus->members)
        {
            if (member->attached)
            {
                ++live;
            }
        }
        *count = live;
    }
    return ep->index;
}

uint64_t LinkCoordinator::Sent(Wrapper* owner, unsigned port)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    Endpoint* ep = Find(owner, port);
    return ep ? ep->sent : 0;
}

uint64_t LinkCoordinator::Delivered(Wrapper* owner, unsigned port)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    Endpoint* ep = Find(owner, port);
    return ep ? ep->delivered : 0;
}

bool LinkCoordinator::Send(retro_link_port_t *handle, uint64_t tick, unsigned to, const void* buf,
                           size_t len)
{
    if (len > MAX_PAYLOAD)
    {
        LogError("Send: payload of " + std::to_string(len) + " bytes exceeds the cap.");
        return false;
    }
    if (len > 0 && !buf)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    Endpoint* sender = Resolve(handle);
    if (!sender || !sender->attached || !sender->bus)
    {
        return false;
    }
    if (!sender->published)
    {
        // A core that sends before it has ever published has told the bus
        // nothing about where it is; anchor it here rather than dropping the
        // message.
        sender->published = true;
        sender->origin = tick;
    }

    const uint64_t sender_delta = (tick > sender->origin) ? (tick - sender->origin) : 0;
    bool delivered = false;
    bool actionable = false;

    for (Endpoint* member : sender->bus->members)
    {
        if (member == sender || !member->attached)
        {
            continue;
        }
        if (to != RETRO_LINK_BROADCAST && static_cast<int>(to) != member->index)
        {
            continue;
        }
        if (!member->published)
        {
            // Nowhere to place the message on a timeline that does not exist
            // yet. The peer will ask again once it starts publishing.
            continue;
        }

        Message msg;
        msg.from = static_cast<unsigned>(sender->index);
        msg.tick = member->origin + ConvertDelta(sender_delta, sender->clock_rate, member->clock_rate);
        msg.Assign(buf, len);
        const uint64_t delivery_tick = msg.tick;

        // Keep each inbox ordered by delivery tick. Two peers can hand a third
        // machine traffic stamped out of order, and a serial port that reads
        // its bytes out of order is not a serial port.
        auto at = std::upper_bound(member->inbox.begin(), member->inbox.end(), msg.tick,
                                   [](uint64_t t, const Message& m) { return t < m.tick; });
        member->inbox.insert(at, std::move(msg));
		++member->inbox_events;
        // A future-stamped message cannot let this endpoint through the clock
        // barrier yet. Waking it only makes it leave and immediately re-enter
        // the same wait, which turns busy multiplayer traffic into an N-way
        // condition-variable storm. The pending inbox edge is retained and is
        // reported with the ordinary horizon grant that reaches its timestamp.
        actionable |= member->origin + member->local_delta >= delivery_tick;
        ++sender->sent;   // counted where a message is actually QUEUED, not attempted
        delivered = true;
    }

	/* The draft interface lets Advance return specifically for an actionable
	 * inbox edge. Wake once after all recipients have been queued; future edges
	 * remain recorded and ride the next horizon grant instead. */
	if (actionable)
	{
		WakeBusLocked(*sender);
	}
    return delivered;
}

bool LinkCoordinator::Recv(retro_link_port_t *handle, uint64_t* tick, unsigned* from, void* buf,
                           size_t* len)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    Endpoint* ep = Resolve(handle);
    if (!ep || !ep->attached || ep->inbox.empty())
    {
        return false;
    }

    // Handed over as soon as it is queued, in tick order, and NOT held back
    // until this machine reaches the tick on it.
    //
    // The tick says when the event LANDS, not when the message may be read. A
    // master stamps a transfer at its own horizon exactly so the slave learns
    // about it in advance and can schedule its own completion for the same tick;
    // waiting until the slave got there would deliver the news too late to act
    // on, and in practice never at all, since the barrier lets a peer reach that
    // horizon and not one tick further.
    //
    // What keeps a message from arriving in this machine's past is the horizon
    // itself: the sender promised not to originate before it, and this machine
    // cannot have run beyond it.
    const Message& head = ep->inbox.front();

    const size_t capacity = len ? *len : 0;
    if (head.size > capacity)
    {
        LogError("Recv: buffer of " + std::to_string(capacity) + " bytes is too small for a " +
                 std::to_string(head.size) + " byte message; dropping it.");
        ep->inbox.pop_front();
        return false;
    }

    if (tick)
    {
        *tick = head.tick;
    }
    if (from)
    {
        *from = head.from;
    }
    if (head.size > 0 && buf)
    {
        std::copy_n(head.Data(), head.size, static_cast<uint8_t*>(buf));
    }
    if (len)
    {
        *len = head.size;
    }

    ep->inbox.pop_front();
    ++ep->delivered;
    return true;
}

uint64_t LinkCoordinator::CeilingLocked(const Endpoint& ep) const
{
    if (!ep.bus)
    {
        return RETRO_LINK_UNBOUNDED;
    }

    uint64_t ceiling = RETRO_LINK_UNBOUNDED;
    for (const Endpoint* member : ep.bus->members)
    {
        if (member == &ep || !member->attached)
        {
            // Only an unattached peer is ignored. That is a machine whose guest
            // is not driving its serial hardware, which is the cable-present,
            // nobody-listening case and must not hold anyone up.
            continue;
        }

        // An attached peer that has not published yet still bounds this
        // machine, at the instant it joined. Skipping it instead would let this
        // core race away before its partner ever reaches its first rendezvous,
        // and the partner would then be stuck behind a horizon that had already
        // been left behind.
        const uint64_t bound = member->published
            ? ep.origin + ConvertDelta(member->safe_delta, member->clock_rate, ep.clock_rate)
            : ep.origin;

        if (ceiling == RETRO_LINK_UNBOUNDED || bound < ceiling)
        {
            ceiling = bound;
        }
    }
    return ceiling;
}

uint64_t LinkCoordinator::Advance(retro_link_port_t *handle, uint64_t local_tick,
                                  uint64_t safe_tick, uint64_t request_tick,
	                              uint32_t* wake_flags)
{
    std::unique_lock<std::mutex> lock(m_mutex);
	if (wake_flags)
	{
		*wake_flags = RETRO_LINK_WAKE_NONE;
	}

    Endpoint* ep = Resolve(handle);
    if (!ep || !ep->attached)
    {
		if (wake_flags)
		{
			*wake_flags = RETRO_LINK_WAKE_DETACHED;
		}
        return RETRO_LINK_UNBOUNDED;
    }

    // The horizon is a promise, so treat a core that offers less than its own
    // position as having promised exactly its position; the API requires
    // safe >= local and this keeps a sloppy core from moving the bound backwards.
    if (safe_tick < local_tick)
    {
        safe_tick = local_tick;
    }

    // Anchoring, which happens on the way in AND again after every wake.
    //
    // Doing it only on the way in is what froze a room. Cabling a third machine
    // in rebuilds the buses, and a rebuild takes every endpoint off its bus and
    // back on, which clears `published` so the whole party re-aligns from that
    // instant. But the two machines already playing are asleep INSIDE this
    // function, and this is the only place that re-anchors. Their flag was
    // cleared under them, their deltas went to zero while their origins stayed
    // where they were, and the loop below then measured them against a ceiling
    // pinned to an origin they had left minutes ago. Both sat there for ever.
    //
    // From the room it looked exactly like what the user reported: plug in the
    // third handheld and the other two freeze, unplug it and they carry on.
    // Nothing was wrong with the third machine, and turning IT off is what let
    // the other two go, because that rebuilt the buses again and handed them
    // back an unbounded grant.
    //
    // Re-anchoring at `local_tick` is right rather than merely expedient: this
    // core has not run an instruction since it called in, so where it is parked
    // IS where it is. And it is a published tick rather than a clock reading, so
    // two peers replaying the same session anchor at the same place.
    // Returns whether this publish changed anything a PEER could act on, which
    // is the only reason to wake anybody.
    //
    // It used to return "did I re-anchor", and every call notified regardless.
    // Measured on Four Swords Adventures with four handhelds: 77 million
    // advance calls produced 126 million condvar wakeups, and the GameCube's
    // ports were woken 59 times per call they made. Nearly all of it was
    // threads waking, recomputing a ceiling that had not moved, and going back
    // to sleep -- while the emulation they were supposed to be doing waited.
    // That is what the crackling was.
    //
    // A peer's ceiling is a function of this endpoint's SAFE horizon and
    // nothing else, so a publish that does not move it cannot unblock anyone.
    auto anchor = [&](Endpoint* e) -> bool
    {
        const uint64_t safe_before = e->safe_delta;
        const bool fresh = !e->published;
        if (fresh)
        {
            e->published = true;
            e->origin = local_tick;
            e->local_delta = 0;
            e->safe_delta = 0;
            // Never backwards. A grant that retreated would be asking a machine
            // to un-run instructions it has already run.
            e->last_grant = std::max(e->last_grant, local_tick);
        }
        const uint64_t ld = (local_tick > e->origin) ? (local_tick - e->origin) : 0;
        const uint64_t sd = (safe_tick > e->origin) ? (safe_tick - e->origin) : 0;
        e->local_delta = std::max(e->local_delta, ld);
        e->safe_delta = std::max(e->safe_delta, sd);
        // Re-anchoring counts too: it moves the origin the horizon is measured
        // from, so a peer's ceiling can change even if the delta did not.
        return fresh || e->safe_delta != safe_before;
    };

#if XENU_MEASURE_LINK_WAITS
    if (!m_session_started)
    {
        m_session_started = true;
        m_session_start = std::chrono::steady_clock::now();
    }
#endif

    const bool moved = anchor(ep);
    ++ep->advance_calls;
    ++m_counters.advance_calls;

    // Publish before waiting: a peer parked on this machine's horizon cannot
    // move until it has been told the horizon moved. Only when it MOVED, and
    // only when there is somebody on this wire to hear it.
    if (moved)
    {
        WakeBusLocked(*ep);
    }

	auto report = [&](uint32_t reasons, uint64_t grant) -> uint64_t
	{
		if (reasons & RETRO_LINK_WAKE_MESSAGE)
		{
			ep->observed_inbox_events = ep->inbox_events;
		}
		if (reasons & RETRO_LINK_WAKE_TOPOLOGY)
		{
			ep->observed_topology_generation = ep->topology_generation;
		}
		if (wake_flags)
		{
			*wake_flags = reasons;
		}
		return grant;
	};

    for (;;)
    {
		uint32_t reasons = RETRO_LINK_WAKE_NONE;
		if (wake_flags)
		{
			if (!ep->inbox.empty() && ep->observed_inbox_events != ep->inbox_events)
			{
				reasons |= RETRO_LINK_WAKE_MESSAGE;
			}
			if (ep->observed_topology_generation != ep->topology_generation)
			{
				reasons |= RETRO_LINK_WAKE_TOPOLOGY;
			}
		}

        const uint64_t ceiling = CeilingLocked(*ep);
        if (ceiling == RETRO_LINK_UNBOUNDED)
        {
			return report(reasons, RETRO_LINK_UNBOUNDED);
        }
        if (ceiling >= request_tick)
        {
            // Grant exactly what was asked for, never the whole headroom that
            // happens to be available. How far a peer has run by this instant
            // is a wall-clock accident, so handing it back would make the
            // return value differ between two machines replaying identical
            // inputs. The core never wanted more than it asked for anyway.
            //
            // Never less than where the core already stands, and never less
            // than last time: a grant that went backwards would be asking a
            // machine to un-run instructions it has already run.
            const uint64_t grant = std::max({request_tick, local_tick, ep->last_grant});
            ep->last_grant = grant;
			return report(reasons, grant);
        }

		/* A queued event is a reason to stop waiting only once this core can act
		 * on it. Future-stamped serial traffic remains in the inbox while the
		 * ordinary horizon barrier advances the receiver toward its timestamp;
		 * treating mere queue occupancy as progress starves the peer that must
		 * publish that clearance. Topology is actionable immediately. */
		const bool actionable_message =
			(reasons & RETRO_LINK_WAKE_MESSAGE) && !ep->inbox.empty() &&
			ep->inbox.front().tick <= local_tick;
		if ((reasons & RETRO_LINK_WAKE_TOPOLOGY) || actionable_message)
		{
			return report(reasons, std::max(local_tick, ep->last_grant));
		}

        // No timeout, on purpose. Giving up after a while would make the grant
        // depend on wall-clock time, and two peers replaying identical inputs
        // would then diverge the first time one of them hit a slow frame.
        //
        // The clock either side of the wait is READ ONLY, and only added to a
        // counter. It decides nothing.
        ++ep->advance_waits;
        ++m_counters.advance_waits;
#if XENU_MEASURE_LINK_WAITS
        const auto parked_at = std::chrono::steady_clock::now();
#endif
        retro_link_port_t *const waiting_on = handle;

        // The slot is read now and remembered, not looked up again after the
        // wait: the bus this endpoint is on may be gone by then, and the count
        // has to come back off the same slot it went on to.
        //
        // A bus is guaranteed here -- CeilingLocked returns unbounded without
        // one, and that path has already returned above -- but the pool is
        // grown defensively so a slot always exists to park on.
        EnsureCvSlotsLocked(ep->bus ? ep->bus->cv_slot + 1 : 1);
        const size_t slot = ep->bus ? ep->bus->cv_slot : 0;

        ++m_cv_parked[slot];
        m_cv_slots[slot]->wait(lock);
        --m_cv_parked[slot];

#if XENU_MEASURE_LINK_WAITS
        const uint64_t slept = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - parked_at).count());
#endif

        // The endpoint may have been torn down while this thread slept, so it
        // is looked up again rather than held across the wait. By ID: the memory
        // is freed on teardown and may since have been handed to a different
        // endpoint, so an address would quietly resolve to a live stranger.
        ep = Resolve(waiting_on);
        if (!ep || !ep->attached || ep->shutting_down)
        {
			if (wake_flags)
			{
				*wake_flags = RETRO_LINK_WAKE_DETACHED;
			}
            return RETRO_LINK_UNBOUNDED;
        }
#if XENU_MEASURE_LINK_WAITS
        ep->blocked_ns += slept;
        if (slept > 20000000ull)
        {
            ++ep->stalls_over_20ms;
        }
        if (slept > 100000000ull)
        {
            ++ep->stalls_over_100ms;
        }
        if (slept > ep->worst_block_ns)
        {
            ep->worst_block_ns = slept;
            // Wall clock, and diagnostic only, like everything else here. The
            // zero is the first time anybody called in (see m_session_start), so
            // this reads as "how far into the session".
            ep->worst_at_ms = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - m_session_start).count());
        }
#endif

        // Put back on a bus while this thread slept, which is what a cable being
        // seated anywhere on this wire does. Say where this machine is, or the
        // rest of the party waits on a member that never speaks again.
        if (anchor(ep))
        {
            WakeBusLocked(*ep);
        }
    }
}
}
