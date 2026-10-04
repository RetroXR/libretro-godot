// Standalone harness for LinkCoordinator. Build and run with tests/run_tests.py.
//
// Stubs the two things LinkCoordinator.cpp reaches outside itself (the
// per-thread wrapper lookup and the logger) so the bus can be exercised
// without a Godot runtime or a real core. Endpoints are keyed on Wrapper*
// purely as an identity, so fake, never-dereferenced pointers work.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <map>
#include <utility>
#include <vector>

#include "Debug.hpp"
#include "LinkCoordinator.hpp"

// ── stubs ───────────────────────────────────────────────────────────────────
namespace Xenu
{
thread_local Wrapper* g_test_current = nullptr;
Wrapper* CurrentThreadWrapper() { return g_test_current; }

void Debug::Log_(const std::string&, const char*) {}
void Debug::LogOK_(const std::string&, const char*) {}
void Debug::LogWarning_(const std::string&, const char*) {}
void Debug::LogError_(const std::string& m, const char* c) { std::printf("      [log] %s: %s\n", c, m.c_str()); }
}

using Xenu::LinkCoordinator;
using Xenu::Wrapper;

static int g_failures = 0;
/* The handle each attached port was given.
 *
 * A core keeps its own; the cases here talk about machines and ports, so they
 * are remembered on the side and looked up. Attaching twice on one port is not
 * something a core does, so a plain map is enough. */
static std::map<std::pair<Wrapper*, unsigned>, retro_link_port_t *> g_handles;

static retro_link_port_t *H(Wrapper* w, unsigned port)
{
    auto it = g_handles.find({w, port});
    return it == g_handles.end() ? nullptr : it->second;
}

static retro_link_port_t *DoAttach(LinkCoordinator& c, Wrapper* w, unsigned port,
                                    const char* protocol, uint64_t hz)
{
    retro_link_port_t *h = c.Attach(w, port, protocol, hz);
    g_handles[{w, port}] = h;
    return h;
}

static void Check(bool ok, const char* what)
{
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

static void SetCurrent(Wrapper* w) { Xenu::g_test_current = w; }

static Wrapper* Fake(uintptr_t id) { return reinterpret_cast<Wrapper*>(id); }

static const uint64_t GBA_HZ = 0x1000000ull;
static const uint64_t GRAIN  = 1024;

// ── T1: nothing cabled means nothing bounds you ─────────────────────────────
static void TestUnbounded()
{
    std::printf("T1 solo endpoint runs free\n");
    auto& c = LinkCoordinator::Get();
    Wrapper* a = Fake(0x1001);
    Check(DoAttach(c, a, 0, "gba-sio-1", GBA_HZ) != nullptr, "attach succeeds");
    Check(c.Advance(H(a, 0), 1000, 1000 + GRAIN, 1000 + GRAIN) == RETRO_LINK_UNBOUNDED,
          "advance is unbounded with no peer");
    unsigned n = 99;
    Check(c.Peers(H(a, 0), &n) == -1 && n == 0, "peers reports not cabled");
    c.DropOwner(a);
}

// ── T2/T3: two cabled machines make progress, identically every run ─────────
struct RunResult { std::vector<uint64_t> grants_a, grants_b; bool timed_out = false; };

static RunResult RunPair(int steps, unsigned seed, bool jitter)
{
    auto& c = LinkCoordinator::Get();
    Wrapper* a = Fake(0x2001);
    Wrapper* b = Fake(0x2002);
    DoAttach(c, a, 0, "gba-sio-1", GBA_HZ);
    DoAttach(c, b, 0, "gba-sio-1", GBA_HZ);
    c.Connect(a, 0, b, 0);

    RunResult out;
    std::atomic<bool> done{false};
    std::atomic<int> finished{0};

    auto body = [&](Wrapper* self, std::vector<uint64_t>& grants, unsigned s) {
        SetCurrent(self);
        std::mt19937 rng(s);
        uint64_t now = 0;
        for (int i = 0; i < steps; ++i)
        {
            if (jitter && (rng() % 4) == 0)
                std::this_thread::sleep_for(std::chrono::microseconds(rng() % 200));
            const uint64_t grant = c.Advance(H(self, 0), now, now + GRAIN, now + GRAIN);
            grants.push_back(grant);
            // An unbounded answer means nothing is holding this machine up, so
            // it takes the whole grain it asked for and keeps going.
            now = (grant != RETRO_LINK_UNBOUNDED && grant < now + GRAIN) ? grant : now + GRAIN;
        }

        // Keep the horizon moving until the other machine has finished too.
        // Detaching the moment this thread runs out of work would pull the
        // cable at a wall-clock-dependent instant, and the peer's remaining
        // grants would then depend on which thread got there first, which is
        // the very thing the real system avoids by scheduling plug and unplug
        // at a fixed emulated frame. A request of 0 is always satisfiable, so
        // this publishes without ever blocking.
        finished.fetch_add(1);
        while (finished.load() < 2)
        {
            c.Advance(H(self, 0), now, now + (GRAIN * 1000), 0);
            std::this_thread::yield();
        }

        // A core that stops driving its serial port says so. Without this the
        // peer would wait on a horizon that is never going to move again, which
        // is a genuine hang and not something the bus can paper over.
        c.Detach(H(self, 0));
    };

    std::thread ta(body, a, std::ref(out.grants_a), seed);
    std::thread tb(body, b, std::ref(out.grants_b), seed ^ 0x9e37);

    // Watchdog: a deadlock must fail the test rather than hang it.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    std::thread watch([&] {
        while (!done.load())
        {
            if (std::chrono::steady_clock::now() > deadline)
            {
                out.timed_out = true;
                c.DropOwner(a);
                c.DropOwner(b);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    ta.join();
    tb.join();
    done = true;
    watch.join();

    c.DropOwner(a);
    c.DropOwner(b);
    return out;
}

static void TestPairProgress()
{
    std::printf("T2 two cabled machines both advance\n");
    RunResult r = RunPair(200, 1, false);
    Check(!r.timed_out, "no deadlock");
    Check(r.grants_a.size() == 200 && r.grants_b.size() == 200, "both ran every step");
    bool monotonic = true;
    for (size_t i = 1; i < r.grants_a.size(); ++i)
        if (r.grants_a[i] < r.grants_a[i - 1]) monotonic = false;
    Check(monotonic, "grants never go backwards");
    Check(r.grants_a.back() > 0 && r.grants_b.back() > 0, "both made real progress");
}

static void TestDeterminism()
{
    std::printf("T3 identical grants regardless of thread timing\n");
    RunResult base = RunPair(120, 7, false);
    bool same = true;
    for (int run = 0; run < 12; ++run)
    {
        RunResult r = RunPair(120, 7, true);
        if (r.timed_out) { same = false; break; }
        if (r.grants_a != base.grants_a || r.grants_b != base.grants_b) { same = false; break; }
    }
    Check(!base.timed_out, "baseline did not deadlock");
    Check(same, "12 jittered runs produced the same grant sequence");
}

// ── T4: a message lands in the receiver's present, not its past or future ───
static void TestSendRecv()
{
    std::printf("T4 send/recv delivery timing\n");
    auto& c = LinkCoordinator::Get();
    Wrapper* a = Fake(0x3001);
    Wrapper* b = Fake(0x3002);
    DoAttach(c, a, 0, "gba-sio-1", GBA_HZ);
    DoAttach(c, b, 0, "gba-sio-1", GBA_HZ);
    c.Connect(a, 0, b, 0);

    // Both publish once so the bus knows where each machine starts.
    SetCurrent(a);
    c.Advance(H(a, 0), 0, GRAIN, 0);
    SetCurrent(b);
    c.Advance(H(b, 0), 0, GRAIN, 0);

    unsigned n = 0;
    Check(c.Peers(H(a, 0), &n) == 0 && n == 2, "A is index 0 of 2");
    Check(c.Peers(H(b, 0), &n) == 1 && n == 2, "B is index 1 of 2");

    const uint8_t payload[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    SetCurrent(a);
    Check(c.Send(H(a, 0), 500, RETRO_LINK_BROADCAST, payload, sizeof payload), "A sends at tick 500");

    uint8_t buf[16];
    size_t len = sizeof buf;
    uint64_t tick = 0;
    unsigned from = 99;
    SetCurrent(b);

    // Handed over straight away, NOT withheld until B reaches tick 500.
    //
    // The tick says when the event LANDS, not when the message may be read. A
    // master stamps a transfer ahead of itself exactly so the slave hears about
    // it in time to schedule its own side for the same tick; holding it back
    // until the slave got there would deliver the news too late to act on, and
    // the barrier means that moment never comes. The sender's horizon is what
    // stops it arriving in the past instead.
    Check(c.Recv(H(b, 0), &tick, &from, buf, &len), "B is told about the transfer in advance");
    Check(tick == 500 && from == 0, "and told which tick it lands on");
    Check(len == 4 && std::memcmp(buf, payload, 4) == 0, "payload survives");

    uint8_t large_payload[64];
    for (size_t i = 0; i < sizeof large_payload; ++i)
        large_payload[i] = static_cast<uint8_t>(i);
    SetCurrent(a);
    Check(c.Send(H(a, 0), 600, RETRO_LINK_BROADCAST, large_payload, sizeof large_payload),
          "payloads larger than the inline buffer still send");
    uint8_t large_buf[sizeof large_payload]{};
    len = sizeof large_buf;
    SetCurrent(b);
    Check(c.Recv(H(b, 0), &tick, &from, large_buf, &len) && len == sizeof large_payload &&
              std::memcmp(large_buf, large_payload, sizeof large_payload) == 0,
          "large payloads use the allocation fallback");

    len = sizeof buf;
    Check(!c.Recv(H(b, 0), &tick, &from, buf, &len), "inbox is drained");

    c.DropOwner(a);
    c.DropOwner(b);
}

// ── T5: a serial port that reorders its bytes is not a serial port ──────────
static void TestRecvOrdering()
{
    std::printf("T5 inbox stays in tick order\n");
    auto& c = LinkCoordinator::Get();
    Wrapper* a = Fake(0x4001);
    Wrapper* b = Fake(0x4002);
    DoAttach(c, a, 0, "gba-sio-1", GBA_HZ);
    DoAttach(c, b, 0, "gba-sio-1", GBA_HZ);
    c.Connect(a, 0, b, 0);
    SetCurrent(a);
    c.Advance(H(a, 0), 0, GRAIN, 0);
    SetCurrent(b);
    c.Advance(H(b, 0), 0, GRAIN, 0);

    // Sent later-tick first, on purpose.
    const uint8_t late = 2, early = 1;
    SetCurrent(a);
    c.Send(H(a, 0), 900, RETRO_LINK_BROADCAST, &late, 1);
    c.Send(H(a, 0), 300, RETRO_LINK_BROADCAST, &early, 1);

    SetCurrent(b);
    c.Advance(H(b, 0), 1000, 1000 + GRAIN, 1000);

    uint8_t v = 0; size_t len = 1; uint64_t tick = 0; unsigned from = 0;
    len = 1; c.Recv(H(b, 0), &tick, &from, &v, &len);
    const bool first_is_early = (v == early && tick == 300);
    len = 1; c.Recv(H(b, 0), &tick, &from, &v, &len);
    const bool second_is_late = (v == late && tick == 900);
    Check(first_is_early && second_is_late, "messages come back oldest first");

    c.DropOwner(a);
    c.DropOwner(b);
}

// ── T6: different clock rates still compare correctly ───────────────────────
static void TestCrossRate()
{
    std::printf("T6 cross-rate conversion\n");
    auto& c = LinkCoordinator::Get();
    Wrapper* slow = Fake(0x5001);   // 1 MHz
    Wrapper* fast = Fake(0x5002);   // 2 MHz
    DoAttach(c, slow, 0, "x", 1000000ull);
    DoAttach(c, fast, 0, "x", 2000000ull);
    c.Connect(slow, 0, fast, 0);

    SetCurrent(slow);
    c.Advance(H(slow, 0), 0, 1000, 0);          // horizon 1000 slow ticks
    SetCurrent(fast);
    // 1000 ticks at 1 MHz is 2000 ticks at 2 MHz, so exactly 2000 is reachable.
    Check(c.Advance(H(fast, 0), 0, 0, 2000) == 2000, "a 1000-tick horizon at 1 MHz lets a 2 MHz peer reach 2000");

    // And one tick past it is not: that request must park until the slow
    // machine extends its promise.
    std::atomic<bool> past{false};
    std::thread probe([&] {
        SetCurrent(fast);
        c.Advance(H(fast, 0), 0, 0, 2001);
        past = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    const bool blocked = !past.load();
    SetCurrent(slow);
    c.Advance(H(slow, 0), 0, 2000, 0);          // extend the slow horizon
    probe.join();
    Check(blocked, "2001 blocks until the slow machine promises further");
    SetCurrent(fast);

    // And a message keeps its place on the wall clock across the rate change.
    const uint8_t byte = 0x5A;
    SetCurrent(slow);
    c.Send(H(slow, 0), 500, RETRO_LINK_BROADCAST, &byte, 1);
    SetCurrent(fast);
    c.Advance(H(fast, 0), 2000, 2000, 2000);
    uint64_t tick = 0; unsigned from = 0; uint8_t v = 0; size_t len = 1;
    Check(c.Recv(H(fast, 0), &tick, &from, &v, &len) && tick == 1000,
          "a message at slow tick 500 arrives at fast tick 1000");

    c.DropOwner(slow);
    c.DropOwner(fast);
}

// ── T7: teardown must release a thread parked on the barrier ────────────────
static void TestTeardownReleases()
{
    std::printf("T7 teardown unblocks a parked thread\n");
    auto& c = LinkCoordinator::Get();
    Wrapper* a = Fake(0x6001);
    Wrapper* b = Fake(0x6002);
    DoAttach(c, a, 0, "gba-sio-1", GBA_HZ);
    DoAttach(c, b, 0, "gba-sio-1", GBA_HZ);
    c.Connect(a, 0, b, 0);

    // B publishes a horizon, then stops for good. A will park behind it.
    SetCurrent(b);
    c.Advance(H(b, 0), 0, GRAIN, 0);

    std::atomic<bool> returned{false};
    std::thread ta([&] {
        SetCurrent(a);
        c.Advance(H(a, 0), 0, GRAIN, 10 * GRAIN);   // far past B's horizon: must block
        returned = true;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const bool parked = !returned.load();

    c.DropOwner(a);                               // what StopEmulationThread does
    for (int i = 0; i < 200 && !returned.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const bool released = returned.load();
    ta.join();
    c.DropOwner(b);

    Check(parked, "A blocks behind B's horizon");
    Check(released, "DropOwner releases it");
}


// ── T8: a cable has two ends ────────────────────────────────────────────────
static void TestPullingOneEndFreesBoth()
{
    std::printf("T8 pulling one end frees the other\n");
    auto& c = LinkCoordinator::Get();
    Wrapper* a = Fake(0x7001);
    Wrapper* b = Fake(0x7002);
    DoAttach(c, a, 0, "gba-sio-1", GBA_HZ);
    DoAttach(c, b, 0, "gba-sio-1", GBA_HZ);
    c.Connect(a, 0, b, 0);

    unsigned n = 0;
    c.Peers(H(a, 0), &n);
    Check(n == 2, "both are on the bus to start with");

    // Only one end is pulled, which is all a hand can do.
    c.Disconnect(a, 0);

    n = 99;
    Check(c.Peers(H(a, 0), &n) == -1 && n == 0, "the pulled end is cabled to nothing");
    n = 99;
    Check(c.Peers(H(b, 0), &n) == -1 && n == 0, "so is the end still holding the lead");

    // And the survivor must genuinely be free, not merely reporting so: with
    // nothing bounding it, its advance has to come back unbounded.
    SetCurrent(b);
    Check(c.Advance(H(b, 0), 0, GRAIN, 10 * GRAIN) == RETRO_LINK_UNBOUNDED,
          "the survivor runs free rather than waiting on a ghost");

    c.DropOwner(a);
    c.DropOwner(b);
}


// -- T9: three and four machines on one chain -------------------------------
static void TestChain()
{
    std::printf("T9 cables chain into one bus\n");
    auto& c = LinkCoordinator::Get();
    Wrapper* a = Fake(0x8001);
    Wrapper* b = Fake(0x8002);
    Wrapper* d = Fake(0x8003);
    Wrapper* e = Fake(0x8004);
    for (Wrapper* w : {a, b, d, e})
    {
        DoAttach(c, w, 0, "gba-sio-1", GBA_HZ);
    }

    // The room reports which machines share one wire, as edges. A GBA chain does
    // not put two cables in one handheld: the third and fourth join through the
    // junction ON the cable, which has no core behind it and so never appears
    // here. What reaches the coordinator is a spanning tree of the machines the
    // room worked out are on the same wire.
    c.Connect(a, 0, b, 0);
    c.Connect(b, 0, d, 0);
    c.Connect(d, 0, e, 0);

    unsigned n = 0;
    c.Peers(H(a, 0), &n);
    Check(n == 4, "all four are on one bus");

    // Player numbers have to be stable, because two peers replaying the same
    // inputs must agree about who owns the clock.
    Check(c.Peers(H(a, 0), &n) == 0, "A is player one");
    Check(c.Peers(H(e, 0), &n) == 3, "E is player four");

    // And every one of them bounds the others. With four machines the ceiling
    // is the SLOWEST, so a straggler anywhere in the chain holds the rest.
    SetCurrent(a);
    c.Advance(H(a, 0), 0, GRAIN, 0);
    SetCurrent(b);
    c.Advance(H(b, 0), 0, GRAIN, 0);
    SetCurrent(d);
    c.Advance(H(d, 0), 0, GRAIN, 0);
    // E has not published, so it still pins everyone at their origin.
    SetCurrent(a);
    Check(c.Advance(H(a, 0), 0, GRAIN, 0) == 0, "an unpublished peer still bounds the chain");

    // Cutting the MIDDLE cable has to leave two buses, not one severed heap.
    // An edge-based topology is the only way that falls out correctly.
    c.Disconnect(b, 0);
    n = 99;
    Check(c.Peers(H(d, 0), &n) == 0 && n == 2, "the far half is still a pair");
    n = 99;
    Check(c.Peers(H(a, 0), &n) == -1 && n == 0, "and A is left holding nothing");

    for (Wrapper* w : {a, b, d, e})
    {
        c.DropOwner(w);
    }
}


// -- T10: the cable decides who is player one, not the power switch ---------
static void TestSeating()
{
    std::printf("T10 the cable decides who is player one\n");
    auto& c = LinkCoordinator::Get();
    Wrapper* grey = Fake(0x9001);
    Wrapper* purple = Fake(0x9002);

    // GREY attaches first, standing for the handheld switched on first. This is
    // the case that used to come out wrong: the coordinator ranked machines by
    // how long it had known them, and an endpoint is created when its CORE
    // attaches, so the console powered on first became player one and the cable
    // had no say at all.
    DoAttach(c, grey, 0, "gba-sio-1", GBA_HZ);
    DoAttach(c, purple, 0, "gba-sio-1", GBA_HZ);

    // The room names the machine holding the PURPLE connector first, because it
    // walks the lead from its purple end. On hardware that is the unit whose SI
    // line the cable pulls low, and it owns the clock.
    Check(c.ConnectGroup({{purple, 0}, {grey, 0}}), "the room cables purple then grey");

    unsigned n = 0;
    Check(c.Peers(H(purple, 0), &n) == 0 && n == 2, "the purple end is player one");
    Check(c.Peers(H(grey, 0), &n) == 1 && n == 2, "the grey end is player two");

    // And turning the lead round swaps the players, with neither machine
    // switched off. Moving a plug from one console to the other is the whole of
    // how this is done on real hardware.
    Check(c.ConnectGroup({{grey, 0}, {purple, 0}}), "the lead is turned round");
    Check(c.Peers(H(grey, 0), &n) == 0, "now the grey machine is player one");
    Check(c.Peers(H(purple, 0), &n) == 1, "and the other one is player two");

    c.DropOwner(grey);
    c.DropOwner(purple);
}

// ── T11: a third machine joining must not freeze the two already playing ────
//
// The fault this pins froze a room. Two handhelds are cabled and playing; a
// third is cabled onto the same wire and switched on; both of the first two
// stop dead and the newcomer never starts. Unplug the third and the other two
// carry on as if nothing happened.
//
// Nothing was wrong with the third machine. Cabling it rebuilds the buses, and
// a rebuild takes every endpoint off its bus and back on, which clears
// `published` so the whole party re-aligns from that instant. But the two that
// were playing were asleep INSIDE Advance, and the only code that re-anchors an
// endpoint ran on the way in. Their flag was cleared under them, their deltas
// went to zero while their origins stayed minutes in the past, and they then
// measured themselves against a ceiling pinned to where they had been when the
// cable was first seated. They never moved again.
static void TestJoiningDoesNotFreezeThePlayers()
{
    std::printf("T11 a third machine joins two that are already playing\n");
    auto& c = LinkCoordinator::Get();
    Wrapper* a = Fake(0x9001);
    Wrapper* b = Fake(0x9002);
    Wrapper* d = Fake(0x9003);
    DoAttach(c, a, 0, "gba-sio-1", GBA_HZ);
    DoAttach(c, b, 0, "gba-sio-1", GBA_HZ);
    c.Connect(a, 0, b, 0);

    // Both have been playing for a while, which is the whole point of the case:
    // their origins are a long way back and a newcomer's is not. Generous
    // horizons so nothing here rendezvouses; the blocking is arranged below, on
    // its own thread, deliberately.
    const uint64_t T = 4000 * GRAIN;
    const uint64_t FAR = T + 300 * GRAIN;
    SetCurrent(a);
    c.Advance(H(a, 0), 0, FAR, 0);
    SetCurrent(b);
    c.Advance(H(b, 0), 0, FAR, 0);
    c.Advance(H(b, 0), T, FAR, T);

    // A asks to run past what B has promised, so it parks. An ordinary
    // rendezvous, and the state the two machines are in when a player picks up
    // a third lead.
    std::atomic<bool> a_back{false};
    std::thread ta([&] {
        SetCurrent(a);
        c.Advance(H(a, 0), T, T + GRAIN, T + 400 * GRAIN);
        a_back = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    Check(!a_back.load(), "one machine is parked at a rendezvous");

    // The third handheld is cabled onto the same wire, the way the room does
    // it: one ConnectGroup naming the whole party in seat order.
    DoAttach(c, d, 0, "gba-sio-1", GBA_HZ);
    Check(c.ConnectGroup({{a, 0}, {b, 0}, {d, 0}}), "the third machine is cabled in");

    // And now everyone says where they are. The newcomer is at nought, because
    // its core has only just been switched on, and that is exactly the gap that
    // used to strand the other two.
    std::atomic<bool> b_back{false};
    std::thread tb([&] {
        SetCurrent(b);
        c.Advance(H(b, 0), T, T + 600 * GRAIN, T);
        b_back = true;
    });
    std::atomic<bool> d_back{false};
    std::thread td([&] {
        SetCurrent(d);
        c.Advance(H(d, 0), 0, 600 * GRAIN, 0);
        d_back = true;
    });

    for (int i = 0; i < 400 && !(a_back.load() && b_back.load() && d_back.load()); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    const bool freed = a_back.load();
    const bool others = b_back.load() && d_back.load();
    // Dropped before the joins, so a machine that is still parked cannot wedge
    // the test run itself. This is what switching a machine off does.
    c.DropOwner(a);
    c.DropOwner(b);
    c.DropOwner(d);
    ta.join();
    tb.join();
    td.join();

    Check(freed, "the parked machine is released rather than left behind");
    Check(others, "and the rest of the party gets its grant too");
}

// ── T12: a cable seated before boot still enforces its protocol ─────────────
static void TestLateAttachProtocolMismatch()
{
    std::printf("T12 late attachment validates the seated bus protocol\n");
    auto& c = LinkCoordinator::Get();
    Wrapper* a = Fake(0xA001);
    Wrapper* b = Fake(0xA002);

    Check(c.Connect(a, 0, b, 0), "the cable can be seated before either core attaches");
    Check(DoAttach(c, a, 0, "gb-sio-1", GBA_HZ) != nullptr,
          "the first protocol claims the wire");
    Check(DoAttach(c, b, 0, "gc-gba-1", GBA_HZ) == nullptr,
          "a later incompatible attachment is refused");
    unsigned n = 0;
    Check(c.Peers(H(a, 0), &n) == 0 && n == 1,
          "the rejected endpoint is not reported as a live peer");
    Check(DoAttach(c, b, 0, "gb-sio-1", GBA_HZ) != nullptr,
          "the matching protocol can still attach afterwards");
    Check(c.Peers(H(a, 0), &n) == 0 && n == 2,
          "both compatible endpoints are then live");

    c.DropOwner(a);
    c.DropOwner(b);
}

// ── T13: refusing a bad regroup leaves the working cable alone ──────────────
static void TestProtocolMismatchIsNonDestructive()
{
    std::printf("T13 an incompatible regroup is non-destructive\n");
    auto& c = LinkCoordinator::Get();
    Wrapper* a = Fake(0xB001);
    Wrapper* b = Fake(0xB002);
    Wrapper* alien = Fake(0xB003);
    DoAttach(c, a, 0, "gb-sio-1", GBA_HZ);
    DoAttach(c, b, 0, "gb-sio-1", GBA_HZ);
    DoAttach(c, alien, 0, "gc-gba-1", GBA_HZ);
    Check(c.Connect(a, 0, b, 0), "a compatible pair starts connected");
    Check(!c.ConnectGroup({{a, 0}, {b, 0}, {alien, 0}}),
          "adding an incompatible endpoint is refused");
    unsigned n = 0;
    Check(c.Peers(H(a, 0), &n) == 0 && n == 2,
          "the original pair remains connected after the refusal");

    c.DropOwner(a);
    c.DropOwner(b);
    c.DropOwner(alien);
}

// ── T14: linked late join carries the state outside core savestates ─────────
static void TestSnapshotRoundTrip()
{
    std::printf("T14 a link-bus snapshot restores clocks and queued messages\n");
    auto& c = LinkCoordinator::Get();
    Wrapper* a = Fake(0xC001);
    Wrapper* b = Fake(0xC002);
    DoAttach(c, a, 0, "gba-sio-1", GBA_HZ);
    DoAttach(c, b, 0, "gba-sio-1", GBA_HZ);
    c.ConnectGroup({{a, 0}, {b, 0}});
    SetCurrent(a); c.Advance(H(a, 0), 100, 300, 0);
    SetCurrent(b); c.Advance(H(b, 0), 200, 400, 0);
    const uint8_t ab[] = {1, 2, 3};
    const uint8_t ba[] = {9, 8};
    c.Send(H(a, 0), 250, 1, ab, sizeof ab);
    c.Send(H(b, 0), 350, 0, ba, sizeof ba);

    std::vector<LinkCoordinator::EndpointState> saved;
    Check(c.CaptureGroup({{a, 0}, {b, 0}}, saved), "the live bus can be captured");
    Check(saved.size() == 2 && saved[0].inbox.size() == 1 && saved[1].inbox.size() == 1,
          "both in-flight directions are present");
    c.DropOwner(a);
    c.DropOwner(b);

    Wrapper* joined_a = Fake(0xC101);
    Wrapper* joined_b = Fake(0xC102);
    DoAttach(c, joined_a, 0, "gba-sio-1", GBA_HZ);
    DoAttach(c, joined_b, 0, "gba-sio-1", GBA_HZ);
    c.ConnectGroup({{joined_a, 0}, {joined_b, 0}});
    Check(c.RestoreGroup({{joined_a, 0}, {joined_b, 0}}, saved),
          "the snapshot restores onto replacement cores");

    std::vector<LinkCoordinator::EndpointState> restored;
    Check(c.CaptureGroup({{joined_a, 0}, {joined_b, 0}}, restored),
          "the restored bus can be inspected");
    Check(restored.size() == saved.size() &&
          restored[0].origin == saved[0].origin &&
          restored[0].local_delta == saved[0].local_delta &&
          restored[0].safe_delta == saved[0].safe_delta &&
          restored[0].last_grant == saved[0].last_grant &&
          restored[1].origin == saved[1].origin &&
          restored[1].local_delta == saved[1].local_delta &&
          restored[1].safe_delta == saved[1].safe_delta &&
          restored[1].last_grant == saved[1].last_grant,
          "every deterministic clock field round-trips");

    uint8_t buf[8] = {}; size_t len = sizeof buf; uint64_t tick = 0; unsigned from = 99;
    Check(c.Recv(H(joined_b, 0), &tick, &from, buf, &len) && tick == 350 && from == 0 &&
          len == sizeof ab && std::memcmp(buf, ab, sizeof ab) == 0,
          "an in-flight message to the second core survives");
    len = sizeof buf; tick = 0; from = 99;
    Check(c.Recv(H(joined_a, 0), &tick, &from, buf, &len) && tick == 250 && from == 1 &&
          len == sizeof ba && std::memcmp(buf, ba, sizeof ba) == 0,
          "and so does the reverse direction");

    c.DropOwner(joined_a);
    c.DropOwner(joined_b);
}


// -- two consoles switched on a long way apart -------------------------------
static void TestBootedApartAlign()
{
    std::printf("T15 a machine joining a live wire re-aligns it\n");
    auto& c = LinkCoordinator::Get();
    Wrapper* a = Fake(0x3401);
    Wrapper* b = Fake(0x3402);

    // The lead is seated before either guest attaches, which is what a room
    // does: the cable is part of the scene and the machines are switched on
    // afterwards, one at a time.
    c.Connect(a, 0, b, 0);

    // A is switched on and runs for a while with nobody to talk to.
    DoAttach(c, a, 0, "gba-sio-1", GBA_HZ);
    SetCurrent(a);
    const uint64_t GAP = 1000000;
    for (uint64_t t = 0; t <= GAP; t += GRAIN)
    {
        c.Advance(H(a, 0), t, t + GRAIN, 0);
    }

    // B is switched on now, and its clock starts where every core's does.
    DoAttach(c, b, 0, "gba-sio-1", GBA_HZ);
    SetCurrent(b);
    c.Advance(H(b, 0), 0, GRAIN, 0);
    SetCurrent(a);
    c.Advance(H(a, 0), GAP, GAP + GRAIN, 0);

    const uint8_t payload[2] = {0x5A, 0xA5};
    SetCurrent(a);
    Check(c.Send(H(a, 0), GAP, RETRO_LINK_BROADCAST, payload, sizeof payload),
          "A speaks as soon as B appears");

    uint8_t buf[16];
    size_t len = sizeof buf;
    uint64_t tick = 0;
    unsigned from = 99;
    SetCurrent(b);
    Check(c.Recv(H(b, 0), &tick, &from, buf, &len), "B is told");

    // The tick is in B's own clock, and B's clock is at zero. A message placed
    // a whole GAP into B's future is one B will not act on until it has run as
    // long as A already had -- for ever, in the sense that matters, because a
    // handshake times out. Everything A says would land that far ahead, so the
    // lag never closes.
    //
    // Which is what a player used to hit: switch on the first console, switch
    // on the second, and the link is up at one end only until the lead is
    // pulled out and pushed back in. That works because a re-seat rebuilds the
    // bus and a rebuild re-anchors everybody; joining a live wire now does the
    // same thing, so it never comes to that.
    Check(tick < GRAIN * 4, "and told a moment it is about to reach, not one a gap away");

    c.DropOwner(a);
    c.DropOwner(b);
}

// -- queued traffic wakes a core parked on clock progress -------------------
static void TestMessageWake()
{
    std::printf("T16 a message wakes Advance before its clock request\n");
    auto& c = LinkCoordinator::Get();
    Wrapper* a = Fake(0x3501);
    Wrapper* b = Fake(0x3502);
    DoAttach(c, a, 0, "gba-sio-1", GBA_HZ);
    DoAttach(c, b, 0, "gba-sio-1", GBA_HZ);
    c.Connect(a, 0, b, 0);

    uint32_t wake = 0;
    c.Advance(H(a, 0), 0, GRAIN, 0, &wake);
    c.Advance(H(b, 0), 0, GRAIN, 0, &wake);

    std::atomic<bool> entered{false};
    uint64_t grant = RETRO_LINK_UNBOUNDED;
    uint32_t reasons = 0;
    std::thread waiter([&] {
        entered = true;
        grant = c.Advance(H(a, 0), 0, GRAIN, 10 * GRAIN, &reasons);
    });
    while (!entered.load())
        std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    const uint8_t byte = 0x5A;
    Check(c.Send(H(b, 0), 0, RETRO_LINK_BROADCAST, &byte, 1),
          "peer queues an actionable byte while Advance is parked");
    waiter.join();
    Check((reasons & RETRO_LINK_WAKE_MESSAGE) != 0,
          "Advance reports that recv is readable");
    Check(grant < 10 * GRAIN, "the wake does not invent the requested clock grant");

    uint8_t got = 0; size_t len = 1;
    Check(c.Recv(H(a, 0), nullptr, nullptr, &got, &len) && got == byte,
          "the waking message remains queued until recv");
    c.DropOwner(a);
    c.DropOwner(b);
}

static void TestFutureMessageKeepsBarrier()
{
    std::printf("T17 a future message does not bypass the clock barrier\n");
    auto& c = LinkCoordinator::Get();
    Wrapper* a = Fake(0x3601);
    Wrapper* b = Fake(0x3602);
    DoAttach(c, a, 0, "psx-sio-1", GBA_HZ);
    DoAttach(c, b, 0, "psx-sio-1", GBA_HZ);
    c.Connect(a, 0, b, 0);

    uint32_t wake = 0;
    c.Advance(H(a, 0), 0, GRAIN, 0, &wake);
    c.Advance(H(b, 0), 0, GRAIN, 0, &wake);

    std::atomic<bool> done{false};
    uint64_t grant = 0;
    uint32_t reasons = 0;
    std::thread waiter([&] {
        grant = c.Advance(H(a, 0), 0, GRAIN, 2 * GRAIN, &reasons);
        done = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    const uint8_t byte = 0xA5;
    Check(c.Send(H(b, 0), GRAIN, RETRO_LINK_BROADCAST, &byte, 1),
          "peer queues a future byte");
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    Check(!done.load(), "future traffic leaves Advance behind the horizon barrier");

    c.Advance(H(b, 0), 0, 2 * GRAIN, GRAIN, &wake);
    waiter.join();
    Check(grant == 2 * GRAIN, "the receiver gets its requested synchronized grant");
    Check((reasons & RETRO_LINK_WAKE_MESSAGE) != 0,
          "and the queued-message edge is reported with that grant");

    c.DropOwner(a);
    c.DropOwner(b);
}

// ── T18: a machine can find the audio sinks on its wire, and only those ─────
//
// The pacing loop of a cabled machine brakes on the neediest sink on its bus
// (AudioHandler::MsUntilBusWantsFrames), and this is where it learns whose those
// are. Handing back a stranger's would have an uncabled machine's sink deciding
// when a cabled one runs; missing a peer's is the fault this exists to remove.
static void TestBusSinkClocks()
{
    std::printf("T18 a machine is handed the sinks on its own wire, and the brake they add up to\n");
    auto& c = LinkCoordinator::Get();
    Wrapper* a = Fake(0x3701);
    Wrapper* b = Fake(0x3702);
    Wrapper* d = Fake(0x3703);   // powered on, cabled to nothing
    auto sink_a = std::make_shared<Xenu::SinkClock>();
    auto sink_b = std::make_shared<Xenu::SinkClock>();
    auto sink_d = std::make_shared<Xenu::SinkClock>();
    c.SetSinkClock(a, sink_a);
    c.SetSinkClock(b, sink_b);
    c.SetSinkClock(d, sink_d);
    DoAttach(c, a, 0, "ps2-ilink-1394", GBA_HZ);
    DoAttach(c, b, 0, "ps2-ilink-1394", GBA_HZ);
    DoAttach(c, d, 0, "ps2-ilink-1394", GBA_HZ);

    std::vector<std::shared_ptr<Xenu::SinkClock>> found{sink_d};
    c.BusSinkClocks(a, found);
    Check(found.empty(), "nothing cabled: no sink but its own, and the list is cleared");

    const uint64_t before = c.TopologyEpoch();
    c.Connect(a, 0, b, 0);
    Check(c.TopologyEpoch() != before, "seating a cable moves the epoch a loop watches");
    c.BusSinkClocks(a, found);
    Check(found.size() == 1 && found[0] == sink_b, "cabled: a is handed b's sink");
    c.BusSinkClocks(b, found);
    Check(found.size() == 1 && found[0] == sink_a, "and b is handed a's");
    c.BusSinkClocks(d, found);
    Check(found.empty(), "the machine beside them, on no wire, is handed neither");

    // What a peer's sink is worth to the brake. A sink 30 ms over its target as
    // of `t0` wants audio 30 ms later, and less the longer ago that was said.
    const int64_t t0 = 1'000'000'000;
    const int64_t ms = 1'000'000;
    sink_b->Publish(30.0, t0, true);
    Check(sink_b->Votes(t0 + 10 * ms), "a fresh reading from a pacing sink counts");
    Check(sink_b->RemainingMs(t0 + 10 * ms) == 20.0, "and is worth what it was, less the time since");
    Check(sink_b->RemainingMs(t0 + 40 * ms) == 0.0, "run out, it wants audio now and never goes negative");
    Check(!sink_b->Votes(t0 + 501 * ms), "a reading half a second old says nothing about anybody");
    sink_b->Publish(30.0, t0, false);
    Check(!sink_b->Votes(t0 + 10 * ms), "a sink with no target to be over has no say");
    Check(!sink_d->Votes(t0), "nor one that has never published");

    const uint64_t attached = c.TopologyEpoch();
    c.Detach(H(b, 0));
    Check(c.TopologyEpoch() != attached, "a guest dropping its port moves the epoch too");
    c.BusSinkClocks(a, found);
    Check(found.empty(), "and a peer that holds nobody up is not braked on");

    DoAttach(c, b, 0, "ps2-ilink-1394", GBA_HZ);
    c.DropOwner(b);
    c.BusSinkClocks(a, found);
    Check(found.empty(), "a machine switched off takes its sink with it");

    c.DropOwner(a);
    c.DropOwner(d);
}

int main()
{
    // Unbuffered, because this binary is run with its output on a pipe and a
    // crash would otherwise take every line it had already printed with it,
    // leaving a failure with no output at all to say where it got to.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    TestUnbounded();
    TestPairProgress();
    TestDeterminism();
    TestSendRecv();
    TestRecvOrdering();
    TestCrossRate();
    TestTeardownReleases();
    TestPullingOneEndFreesBoth();
    TestChain();
    TestSeating();
    TestJoiningDoesNotFreezeThePlayers();
    TestLateAttachProtocolMismatch();
    TestProtocolMismatchIsNonDestructive();
    TestSnapshotRoundTrip();
    TestBootedApartAlign();
    TestMessageWake();
    TestFutureMessageKeepsBarrier();
    TestBusSinkClocks();
    std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASS",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
