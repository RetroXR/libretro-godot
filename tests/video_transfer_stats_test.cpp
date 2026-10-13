#include "VideoTransferStats.hpp"
#include <cassert>
#include <iostream>
#include <thread>
#include <vector>

int main()
{
    using namespace Xenu;
    VideoTransferStats stats;
    { VideoTransferTimer timer(&stats, VideoTransferStats::READBACK_TOTAL); }
    assert(stats.stages[VideoTransferStats::READBACK_TOTAL].calls.load() == 0);
    stats.enabled.store(true);
    { VideoTransferTimer timer(&stats, VideoTransferStats::READBACK_TOTAL); }
    assert(stats.stages[VideoTransferStats::READBACK_TOTAL].calls.load() == 1);
    stats.enabled.store(false);
    { VideoTransferTimer timer(&stats, VideoTransferStats::READBACK_TOTAL); }
    assert(stats.stages[VideoTransferStats::READBACK_TOTAL].calls.load() == 1);
    { VideoTransferTimer timer(nullptr, VideoTransferStats::READBACK_TOTAL); }

    // Exercise contention between native frame producers. Totals and peaks
    // must survive concurrent updates rather than overwrite each other.
    std::vector<std::thread> workers;
    for (uint64_t n = 1; n <= 4; ++n)
        workers.emplace_back([&stats, n]() {
            for (int i = 0; i < 10000; ++i)
                stats.stages[VideoTransferStats::READBACK_COPY].Add(n);
        });
    for (auto& worker : workers) worker.join();
    const auto& counter = stats.stages[VideoTransferStats::READBACK_COPY];
    assert(counter.calls.load() == 40000);
    assert(counter.total_ns.load() == 100000);
    assert(counter.max_ns.load() == 4);
    assert(stats.stages[VideoTransferStats::TEXTURE_UPDATE].calls.load() == 0);
    std::cout << "video transfer stats: disabled/enabled scopes and concurrent totals passed\n";
}
