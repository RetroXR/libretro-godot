// What a party of linked machines costs the coordinator, with no Godot, no
// core and no headset. Build and run with tests/run_tests.py --only link_bench,
// or cross-compile it for the Quest (see the header comment in that script).
//
// This exists because the symptom is a scaling one: a pair of cabled machines
// is fine and a party of four is slow, and neither the unit tests nor a profile
// of the whole app can say why on their own. The tests prove the bus is
// CORRECT; this measures what it COSTS, at two, three and four machines, so the
// shape of the curve is visible rather than inferred.
//
// The metric that matters is `wakes per advance call`, not wall time. A condvar
// broadcast is a futex syscall per sleeping thread, and it was measured at
// 23.67% of all CPU cycles on a four-handheld Quest session against 6.49% for
// the four emulators put together. Wall time here depends on how many cores the
// machine running the bench has spare; the wake count does not, which makes it
// the number to compare across a change.
//
// Each thread reproduces GBASIONetlink's _netlinkEvent exactly: publish where it
// is and how far it promises not to originate past, ask for one grain, then
// advance by whatever it was granted and do it again. Nothing else about the
// driver is modelled, because nothing else about it takes the lock.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
thread_local Wrapper* g_bench_current = nullptr;
Wrapper* CurrentThreadWrapper() { return g_bench_current; }

void Debug::Log_(const std::string&, const char*) {}
void Debug::LogOK_(const std::string&, const char*) {}
void Debug::LogWarning_(const std::string&, const char*) {}
void Debug::LogError_(const std::string& m, const char* c) { std::printf("  [log] %s: %s\n", c, m.c_str()); }
}

using Xenu::LinkCoordinator;
using Xenu::Wrapper;

static const uint64_t GBA_HZ = 0x1000000ull;

// The multiplayer grain the shipped driver uses, and the thing most worth
// varying: it sets how OFTEN the party rendezvouses, and every cost here is a
// multiple of that. NETLINK_GRAIN is 256, sized against the 5755-cycle transfer
// of a PAIR; four machines at the same baud take 10486 cycles per transfer, so
// a four-way party has headroom the constant does not use.
static uint64_t g_grain = 256;

static Wrapper* Fake(uintptr_t id) { return reinterpret_cast<Wrapper*>(id); }

/// Stand-in for emulating one grain's worth of cycles.
///
/// Zero by default, which is the pure-contention case and the clearest signal.
/// A real core does roughly 15 us of work per 256 GBA cycles, so --work-ns is
/// there to check that a conclusion drawn at zero still holds when the threads
/// are not purely fighting over the lock.
static void BurnNanos(uint64_t ns)
{
    if (!ns)
    {
        return;
    }
    const auto until = std::chrono::steady_clock::now() + std::chrono::nanoseconds(ns);
    while (std::chrono::steady_clock::now() < until)
    {
        // Spin. sleep_for would hand the core away and measure the scheduler
        // instead of the coordinator.
    }
}

struct Result
{
    double wall_ms = 0.0;
    uint64_t advance_calls = 0;
    uint64_t advance_waits = 0;
    uint64_t wakes = 0;
    bool bus_formed = false;
};

static Result RunParty(unsigned machines, uint64_t cycles, uint64_t work_ns)
{
    LinkCoordinator& c = LinkCoordinator::Get();

    std::vector<Wrapper*> owners;
    std::vector<retro_link_port_t *> handles(machines, nullptr);
    std::vector<std::pair<Wrapper*, unsigned>> ports;

    for (unsigned i = 0; i < machines; ++i)
    {
        Wrapper* w = Fake(0x9000 + i);
        owners.push_back(w);
        Xenu::g_bench_current = w;
        handles[i] = c.Attach(w, 0, "gba-sio", GBA_HZ);
        ports.push_back({w, 0});
    }
    Xenu::g_bench_current = nullptr;

    Result r;
    r.bus_formed = c.ConnectGroup(ports);

    // Every machine on the wire before anybody starts, which is what a room
    // that was cabled up before the games began looks like.
    c.ResetCounters();

    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    threads.reserve(machines);

    for (unsigned i = 0; i < machines; ++i)
    {
        threads.emplace_back([&, i]() {
            while (!go.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
            uint64_t now = 0;
            uint64_t step = g_grain;
            while (now < cycles)
            {
                now += step;
                BurnNanos(work_ns);

                // Exactly _netlinkEvent: publish, ask for one grain, then take
                // the smaller of the grain and what was granted.
                const uint64_t grant = c.Advance(handles[i], now, now + g_grain, now + g_grain);
                step = g_grain;
                if (grant != RETRO_LINK_UNBOUNDED && grant > now && (grant - now) < step)
                {
                    step = grant - now;
                }
                if (!step)
                {
                    step = 1;
                }
            }
        });
    }

    const auto started = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);
    for (auto& t : threads)
    {
        t.join();
    }
    const auto ended = std::chrono::steady_clock::now();

    r.wall_ms = std::chrono::duration<double, std::milli>(ended - started).count();

    const LinkCoordinator::Counters k = c.CountersSnapshot();
    r.advance_calls = k.advance_calls;
    r.advance_waits = k.advance_waits;
    r.wakes = k.wakes;

    // Back to an empty room, or the next party joins this one's bus.
    for (Wrapper* w : owners)
    {
        c.DropOwner(w);
    }
    return r;
}

int main(int argc, char** argv)
{
    uint64_t cycles = 2000000;   // ~0.12 s of emulated GBA time
    uint64_t work_ns = 0;
    unsigned only = 0;
    unsigned repeat = 1;

    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a.rfind("--cycles=", 0) == 0)        cycles = std::strtoull(a.c_str() + 9, nullptr, 10);
        else if (a.rfind("--work-ns=", 0) == 0)  work_ns = std::strtoull(a.c_str() + 10, nullptr, 10);
        else if (a.rfind("--machines=", 0) == 0) only = static_cast<unsigned>(std::strtoul(a.c_str() + 11, nullptr, 10));
        else if (a.rfind("--repeat=", 0) == 0)   repeat = static_cast<unsigned>(std::strtoul(a.c_str() + 9, nullptr, 10));
        else if (a.rfind("--grain=", 0) == 0)    g_grain = std::strtoull(a.c_str() + 8, nullptr, 10);
        else
        {
            std::printf("unknown argument '%s'\n", a.c_str());
            return 2;
        }
    }

    std::printf("link_bench: %llu emulated cycles per machine, grain %llu, work %llu ns/grain\n",
                static_cast<unsigned long long>(cycles),
                static_cast<unsigned long long>(g_grain),
                static_cast<unsigned long long>(work_ns));
    std::printf("%-9s %10s %12s %12s %12s %9s %9s\n",
                "machines", "wall ms", "advances", "parks", "wakes",
                "park/adv", "wake/adv");

    int failures = 0;
    for (unsigned n = 2; n <= 4; ++n)
    {
        if (only && n != only)
        {
            continue;
        }
        for (unsigned pass = 0; pass < repeat; ++pass)
        {
            const Result r = RunParty(n, cycles, work_ns);
            if (!r.bus_formed)
            {
                std::printf("%-9u  BUS DID NOT FORM\n", n);
                ++failures;
                continue;
            }
            const double per_adv = r.advance_calls ? static_cast<double>(r.advance_waits) / static_cast<double>(r.advance_calls) : 0.0;
            const double wake_adv = r.advance_calls ? static_cast<double>(r.wakes) / static_cast<double>(r.advance_calls) : 0.0;
            std::printf("%-9u %10.1f %12llu %12llu %12llu %9.3f %9.3f\n",
                        n, r.wall_ms,
                        static_cast<unsigned long long>(r.advance_calls),
                        static_cast<unsigned long long>(r.advance_waits),
                        static_cast<unsigned long long>(r.wakes),
                        per_adv, wake_adv);
        }
    }

    if (failures)
    {
        std::printf("\n%d configuration(s) failed to set up\n", failures);
        return 1;
    }
    return 0;
}
