#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "LinkInterface.hpp"
#include "SinkClock.hpp"

namespace Xenu
{
class Wrapper;

/// The instance running on the calling thread, or null.
///
/// Declared here rather than reaching for Wrapper.hpp so the bus stays clear of
/// Godot entirely: it only ever needs the pointer as an identity and never the
/// type, and keeping it that way is what lets the coordinator be tested on its
/// own. Defined in Wrapper.cpp.
Wrapper* CurrentThreadWrapper();

/// The link bus, shared by every core instance in this process.
///
/// A process-wide singleton on purpose. Core::Load copies each core to its own
/// temp path before opening it, so two instances of the same core share no
/// globals at all and cannot reach each other; the host is the only thing they
/// have in common. Endpoints are keyed by (Wrapper*, port), and the caller is
/// recovered from the emulation thread via Wrapper::GetCurrentThreadWrapper,
/// the same trick EnvironmentHandler::Callback uses, since the libretro
/// function-pointer signatures carry no userdata.
///
/// DETERMINISM. Under replicated netplay every peer runs both cores and must
/// produce bit-identical output, so a grant must be a pure function of what the
/// participants have published. Nothing here may consult wall-clock time or
/// observe which thread arrived first, and there is deliberately no timeout: a
/// peer that is behind is waited for, never guessed at. The one thing this
/// class cannot enforce is that Connect and Disconnect happen at the same
/// emulated frame on every peer; that is the caller's job, and the reason the
/// VR cable schedules plug and unplug LINK_LEAD frames out rather than applying
/// them the instant a hand moves.
class LinkCoordinator
{
public:
    static LinkCoordinator& Get();

    /// The interface handed to a core answering RETRO_ENVIRONMENT_GET_LINK_INTERFACE.
    /// Stable for the life of the process; the trampolines resolve the caller
    /// themselves, so one shared instance serves every core.
    static const retro_link_interface* Interface();

    // ── Host side: the cable ─────────────────────────────────────────────────
    // Connect is what one seated link cable means: a single wire between two
    // ports. Either end may still be unattached (the guest has not touched its
    // serial port yet); the link just stays inert until both sides publish.
    //
    // Cables CHAIN. A GBA lead carries an inline junction so a third and fourth
    // machine can join, and the set that ends up sharing one wire is whatever
    // the cables transitively join together. So each call records one wire and
    // the buses are derived from them, rather than a call declaring a whole bus:
    // that is the only way unplugging the middle cable of A-B-C-D can leave A-B
    // and C-D instead of one severed heap.

    bool Connect(Wrapper* a, unsigned port_a, Wrapper* b, unsigned port_b);

    /// Every machine port sharing one wire, as the room worked it out.
    ///
    /// This is the general form and Connect is the two-machine case of it. The
    /// room has to do the walking because the junction moulded into a link cable
    /// is a scene object with no core behind it: it conducts, so it belongs in
    /// the graph, but it can never be an endpoint here. So the room follows the
    /// cables through their junctions, decides which machines end up on the same
    /// wire, and states the answer.
    ///
    /// Listed ports leave whatever bus they were on. A group of fewer than two
    /// is a lead going nowhere and leaves them on no bus at all.
    bool ConnectGroup(const std::vector<std::pair<Wrapper*, unsigned>>& ports);
    void Disconnect(Wrapper* owner, unsigned port);

    /// Unblock and forget everything owned by `owner`. Must be called when an
    /// instance is torn down, because an emulation thread parked in Advance
    /// would otherwise never return and the join would hang.
    void DropOwner(Wrapper* owner);

    // ── Core side: reached through the trampolines ───────────────────────────
    //
    // Everything after Attach takes the ENDPOINT the core was given, not the
    // (instance, port) pair it was found by. The core hands back what it was
    // handed, so nothing has to be worked out from the calling thread -- which
    // is the only way this survives a core that runs on more than one.
    retro_link_port_t *Attach(Wrapper* owner, unsigned port, const char* protocol_id,
                               uint64_t clock_rate);
    void     Detach(retro_link_port_t *handle);
    int      Peers(retro_link_port_t *handle, unsigned* count);
    bool     Send(retro_link_port_t *handle, uint64_t tick, unsigned to, const void* buf, size_t len);
    bool     Recv(retro_link_port_t *handle, uint64_t* tick, unsigned* from, void* buf, size_t* len);
    uint64_t Advance(retro_link_port_t *handle, uint64_t local_tick, uint64_t safe_tick,
                     uint64_t request_tick, uint32_t* wake_flags = nullptr);

    // Read by the ROOM rather than by a core, so these look an endpoint up by
    // machine and port the way everything else on the host side does. A cable
    // is a thing in a room; it has no handle and no business holding one.
    int      PeersFor(Wrapper* owner, unsigned port, unsigned* count);
    uint64_t Delivered(Wrapper* owner, unsigned port);
    uint64_t Sent(Wrapper* owner, unsigned port);

    /// What `port` has cost the bus so far, copied from the Endpoint fields of
    /// the same names. ReportCostLocked says this once, as a machine is torn
    /// down; a probe needs it WHILE they run, because which console the others
    /// were parked on during one bad second is the question, and a total cannot
    /// answer it. Diagnostic only, like everything it copies, and all zero past
    /// the two counts unless the waits are being timed.
    struct Cost
    {
        uint64_t advance_calls = 0;
        uint64_t advance_waits = 0;
        uint64_t blocked_ns = 0;
        uint64_t worst_block_ns = 0;
        uint64_t stalls_over_20ms = 0;
        uint64_t stalls_over_100ms = 0;
    };
    bool CostFor(Wrapper* owner, unsigned port, Cost& out);

    // ── Pacing: whose audio sinks share a wire ───────────────────────────────
    // The bus decides nothing here and reads none of it. It only knows who is
    // cabled to whom, which is the one thing a machine's pacing loop cannot work
    // out for itself, so it keeps each machine's SinkClock and hands back the
    // ones on the same wire. What a loop does with them is AudioHandler's.

    /// The sink `owner` paces against, replacing any it gave before. Forgotten
    /// by DropOwner with everything else of that machine's.
    void SetSinkClock(Wrapper* owner, std::shared_ptr<SinkClock> clock);

    /// The sinks of every OTHER machine attached to a bus `owner` is attached
    /// to. Empty for a machine cabled to nothing, which is nearly all of them.
    void BusSinkClocks(Wrapper* owner, std::vector<std::shared_ptr<SinkClock>>& out);

    /// Moves whenever the answer BusSinkClocks would give may have: a cable, an
    /// attach or detach, a machine switched off, a sink registered. A pacing
    /// loop runs hundreds of passes a second and the bus mutex is the one every
    /// rendezvous takes, so the loop keeps the list and asks again only when
    /// this has changed -- one atomic read a pass in a room with no cables.
    uint64_t TopologyEpoch() const { return m_topology_epoch.load(std::memory_order_acquire); }

    /// Largest payload a single Send will carry. Link protocols move a handful
    /// of bytes per transfer; the cap exists so a malformed core cannot make
    /// the host allocate without bound.
    static constexpr size_t MAX_PAYLOAD = 4096;

    struct Message
    {
        static constexpr size_t INLINE_CAPACITY = 32;

        uint64_t tick = 0;   ///< already converted into the RECEIVER's ticks
        unsigned from = 0;   ///< sender's index on the bus
        size_t size = 0;
        std::array<uint8_t, INLINE_CAPACITY> inline_data{};
        std::vector<uint8_t> heap_data;

        void Assign(const void* source, size_t length)
        {
            size = length;
            const auto* first = static_cast<const uint8_t*>(source);
            if (length <= INLINE_CAPACITY)
            {
                std::copy_n(first, length, inline_data.begin());
                heap_data.clear();
            }
            else
            {
                heap_data.assign(first, first + length);
            }
        }

        const uint8_t* Data() const
        {
            return size <= INLINE_CAPACITY ? inline_data.data() : heap_data.data();
        }
    };

    /// Deterministic bus state that lives outside a core savestate. A linked
    /// netplay late join copies one of these per endpoint after every core is
    /// paused at the same frame boundary.
    struct EndpointState
    {
        bool published = false;
        uint64_t origin = 0;
        uint64_t local_delta = 0;
        uint64_t safe_delta = 0;
        uint64_t last_grant = 0;
        std::vector<Message> inbox;
    };

    bool CaptureGroup(const std::vector<std::pair<Wrapper*, unsigned>>& ports,
                      std::vector<EndpointState>& out);
    bool RestoreGroup(const std::vector<std::pair<Wrapper*, unsigned>>& ports,
                      const std::vector<EndpointState>& states);

    struct Bus;

    struct Endpoint
    {
        /// What a core is handed at attach, and hands back on every call.
        ///
        /// An id rather than the address, and never reused. A thread parked in
        /// Advance sleeps with the lock released, and the endpoint it was
        /// waiting on can be torn down in that moment -- a machine switched off
        /// erases them -- so it has to look the endpoint up again after waking.
        /// An address could not survive that: the memory may be freed, and worse
        /// may be handed to the NEXT endpoint, so a stale handle would quietly
        /// resolve to a live stranger. An id cannot be mistaken for another.
        uint64_t id = 0;

        Wrapper* owner = nullptr;
        unsigned port = 0;

        /// A short stable name for the log, "m1:0" for the first machine's port
        /// zero. The bus knows a machine only as a pointer, and a pointer in a
        /// log line is unreadable and different every run; the room's own
        /// [LinkCable] lines carry the scene names, and these two are read
        /// together.
        std::string label;
        std::string protocol_id;
        uint64_t clock_rate = 0;
        bool attached = false;

        Bus* bus = nullptr;
        int index = -1;

        /// Where the ROOM says this machine sits on the wire, or -1 if nobody
        /// said. Player one is seat zero.
        ///
        /// This is a physical fact and the coordinator cannot work it out: on
        /// real hardware the parent is the unit whose SI line the CABLE pulls
        /// low, so it is decided by which connector went into which console and
        /// by nothing else. The room knows that -- it walks the leads from the
        /// head of the chain and takes its purple end first -- and ConnectGroup
        /// is how it says so.
        int seat = -1;

        /// An endpoint counts toward other members' grants only once it has
        /// published, and its origin is the first tick it published after
        /// joining the bus. Two machines cabled together mid-session are
        /// aligned from that instant, which is what happens on real hardware:
        /// the link protocol is self-synchronizing and does not care how long
        /// either console has been powered on.
        bool published = false;
        uint64_t origin = 0;
        uint64_t local_delta = 0;
        uint64_t safe_delta = 0;

        uint64_t last_grant = 0;
        bool shutting_down = false;

        /* Incremented whenever the room rebuilds this endpoint's wire. Advance
         * consumes it so a core blocked on an otherwise idle cable can refresh
         * membership immediately. */
        uint64_t topology_generation = 0;
        uint64_t observed_topology_generation = 0;
		uint64_t inbox_events = 0;
		uint64_t observed_inbox_events = 0;


        std::deque<Message> inbox;

        /// Messages this endpoint has taken off the bus. Only ever counted up,
        /// and only so a test can tell a link that NEGOTIATED from a link that
        /// is actually carrying a game: peer counts prove the cable, traffic
        /// proves the guests are talking over it.
        uint64_t delivered = 0;
        uint64_t sent = 0;

        /// What this endpoint costs the bus, reported when it is torn down.
        ///
        /// Diagnostic only, and it MUST stay that way: `blocked_ns` is wall
        /// clock, and a grant that depended on wall clock would make two peers
        /// replaying identical inputs diverge the first time one of them hit a
        /// slow frame. Nothing here is ever read by a decision.
        ///
        /// It exists because the grain was sized for a PAIR of machines and
        /// Four Swords Adventures puts five on one bus, and nobody could say
        /// what that actually cost without counting it.
        uint64_t advance_calls = 0;
        uint64_t advance_waits = 0;
        uint64_t blocked_ns = 0;
        /// The single LONGEST park. The average says nothing about audio: a
        /// sink holding 100 ms of samples survives any number of short stalls
        /// and dies on one long one, so this is the number that decides whether
        /// the sound breaks up.
        uint64_t worst_block_ns = 0;
        /// WHEN the worst one happened, as milliseconds since this coordinator
        /// first saw anybody, and how many stalls were long enough to empty a
        /// 100 ms audio sink. One long stall while a machine loads is a
        /// different fault from a hundred of them spread through play, and the
        /// totals cannot tell those apart.
        uint64_t worst_at_ms = 0;
        uint64_t stalls_over_100ms = 0;
        uint64_t stalls_over_20ms = 0;
    };

    struct Bus
    {
        std::vector<Endpoint*> members;

        /// Which condvar in the coordinator's pool this wire's members park on.
        ///
        /// An index rather than a condvar of its own, because a Bus is destroyed
        /// and rebuilt every time a cable moves, and a thread parked on a condvar
        /// that is then destroyed is undefined behaviour. The pool outlives every
        /// bus; see m_cv_slots.
        size_t cv_slot = 0;
    };

    /// One seated cable. The wires are what the room actually tells us about;
    /// everything else here is worked out from them.
    struct Link
    {
        Endpoint* a = nullptr;
        Endpoint* b = nullptr;
    };

private:
    LinkCoordinator() = default;

    Endpoint* Find(Wrapper* owner, unsigned port);
    Endpoint& FindOrCreate(Wrapper* owner, unsigned port);

    /// Forget every wire touching `ep`. A machine's port holds one plug, so in
    /// practice that is one wire, but a junction on a cable can hold more.
    void CutLinksAt(const Endpoint& ep);

    /// The endpoint a handle names, or null if it has been torn down since.
    /// Caller holds m_mutex.
    Endpoint* Resolve(retro_link_port_t *handle);

    /// Rebuild every bus from the wires. Called after any change to them.
    ///
    /// A machine left alone in its own component is on no bus at all: a cable
    /// has two ends, and one machine holding a lead that goes nowhere is not
    /// cabled to anything.
    void RebuildBuses();

    /// Reset one endpoint's bus-side state. Shared so every way off a bus agrees.
    static void Detached(Endpoint& ep);

    /// Say what the buses look like now. Caller holds m_mutex.
    ///
    /// Logged after every change to the wires because the symptom it explains is
    /// silent: a machine only takes part in a link if its CORE called attach,
    /// which happens at load behind a core option, so a console switched on
    /// before the option was turned on is cabled in every visible respect while
    /// never joining the bus at all. The room cannot see that and reports the
    /// cable as seated. Here it reads as a member marked "off".
    void LogBusesLocked(const char* why) const;
    /// One line of what an endpoint cost, written as it is torn down.
    void ReportCostLocked(const Endpoint& ep) const;

    /// How many endpoints exist, used to number them for the log.
    unsigned m_next_label = 0;

    /// Next endpoint id. Starts at 1 so that a zero handle is always invalid.
    uint64_t m_next_id = 1;

    /// When the first core called into the bus, so worst_at_ms reads as "how far
    /// into the session". Diagnostic only, like everything it feeds.
    ///
    /// This used to be a static local declared inside the branch that records a
    /// new worst block, so it was initialised by the first STALL rather than the
    /// first call -- while its comment claimed otherwise. Every "at N ms in"
    /// figure was therefore measured from an unknown origin, which is worse than
    /// no figure: it invites exactly the correlation with a log timestamp that it
    /// cannot support, and one was drawn from it and had to be retracted.
    bool m_session_started = false;
    std::chrono::steady_clock::time_point m_session_start;

    /// Ceiling for `ep` in its own ticks, or RETRO_LINK_UNBOUNDED when nothing
    /// bounds it. Caller holds m_mutex.
    uint64_t CeilingLocked(const Endpoint& ep) const;

public:
    /// Coordinator-wide totals, for a bench rather than for a decision.
    ///
    /// Same rule as the per-endpoint numbers: diagnostic only, never read by
    /// anything that decides a grant. `wakes` is the one that matters — a
    /// condvar broadcast is a syscall per sleeping thread, and on a four-handheld
    /// bus it was measured at 23.67% of all CPU cycles, against 6.49% for the
    /// four emulators put together.
    struct Counters
    {
        uint64_t advance_calls = 0;
        uint64_t advance_waits = 0;
        uint64_t wakes = 0;
    };

    Counters CountersSnapshot() const;
    void ResetCounters();

private:
    Counters m_counters;

    /// Wake the machines on `ep`'s wire, and nobody else. Caller holds m_mutex.
    ///
    /// The only thing that ever parks in here is Advance, and the only thing it
    /// is waiting for is its ceiling to rise -- which is a function of its own
    /// bus members' safe horizons and of nothing else. So a publish can only
    /// ever unblock somebody on the same wire, and waking anyone else is work
    /// spent to have a thread recompute a ceiling that did not move and park
    /// again.
    ///
    /// That used to be the whole room. One process-wide condvar meant every
    /// publish woke every parked emulation thread there was, each of which then
    /// queued for the one global mutex and rescanned its bus. With N machines on
    /// a wire rendezvousing every 256 cycles that is N-squared wakeups per
    /// grain, all of it serialised, which is why a party got slower with each
    /// handheld that joined it rather than merely busier.
    void WakeBusLocked(const Endpoint& ep);

    /// Wake every parked thread, wherever it is. Caller holds m_mutex.
    ///
    /// For the topology changes only. A rebuild reassigns slots, so a thread
    /// parked under the old arrangement can be waiting on a slot that no longer
    /// belongs to its bus; broadcasting is what guarantees it is heard and gets
    /// to re-park on the right one. Rare enough that the cost does not matter,
    /// and skipping it would strand exactly the machine whose cable just moved.
    void WakeAllLocked();

    /// Grow the condvar pool to cover `count` buses. Caller holds m_mutex.
    void EnsureCvSlotsLocked(size_t count);

    mutable std::mutex m_mutex;

    /// One condvar per bus, held by the coordinator rather than by the bus.
    ///
    /// Only ever appended to: a slot is never destroyed while this singleton
    /// lives, so a thread that parked on one before a cable moved still has a
    /// valid condvar to be woken on afterwards. Slots ARE reused across
    /// rebuilds, which can wake a straggler that has moved to another wire --
    /// harmless, it rechecks and parks again.
    std::vector<std::unique_ptr<std::condition_variable>> m_cv_slots;

    /// How many threads are parked on each slot, so a publish with nobody
    /// listening can skip the notify altogether. Same index as m_cv_slots.
    std::vector<unsigned> m_cv_parked;
    std::vector<std::unique_ptr<Endpoint>> m_endpoints;
    /// Endpoint id -> live endpoint. IDs are never reused, so a removed entry
    /// stays null and a stale core handle cannot resolve to a later machine.
    /// The table trades one pointer per attachment for O(1) core-facing calls.
    std::vector<Endpoint*> m_handles{nullptr};
    std::vector<std::unique_ptr<Bus>> m_buses;
    std::vector<Link> m_links;

    /// See SetSinkClock. Keyed by machine rather than by endpoint: a sink
    /// belongs to the machine, which may have several ports or none yet.
    std::vector<std::pair<Wrapper*, std::shared_ptr<SinkClock>>> m_sink_clocks;
    /// See TopologyEpoch. Starts at 1 so a loop that has never asked, holding 0,
    /// asks on its first pass.
    std::atomic<uint64_t> m_topology_epoch{1};
};
}
