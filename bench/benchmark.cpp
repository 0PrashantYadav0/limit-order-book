#include "lob/order_book.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <random>
#include <vector>

using namespace lob;
using Clock = std::chrono::steady_clock;

namespace {

double percentile(std::vector<std::uint64_t>& v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const double idx = (p / 100.0) * static_cast<double>(v.size() - 1);
    return static_cast<double>(v[static_cast<std::size_t>(idx)]);
}

// Estimate the cost of a std::chrono::steady_clock::now() pair, so we can
// subtract it from per-op latencies and report engine-only time honestly.
double measure_clock_overhead_ns(std::size_t iters = 200000) {
    volatile std::uint64_t sink = 0;
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < iters; ++i) {
        const auto s = Clock::now();
        const auto e = Clock::now();
        total += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(e - s).count());
        sink += total;
    }
    (void)sink;
    return static_cast<double>(total) / static_cast<double>(iters);
}

void report(const std::string& name, std::vector<std::uint64_t>& lat_ns,
            double wall_seconds, double clock_ns) {
    const std::size_t n = lat_ns.size();
    const double ops_per_sec = static_cast<double>(n) / wall_seconds;
    std::uint64_t sum = 0;
    for (auto x : lat_ns) sum += x;
    const double mean = static_cast<double>(sum) / static_cast<double>(n);
    auto adj = [clock_ns](double v) { return v > clock_ns ? v - clock_ns : 0.0; };

    std::cout << "\n== " << name << " ==\n";
    std::cout << "  operations        : " << n << '\n';
    std::cout << "  batch throughput  : " << static_cast<std::uint64_t>(ops_per_sec)
              << " ops/sec (wall " << wall_seconds * 1e3 << " ms)\n";
    std::cout << "  per-op latency (engine-only, clock overhead subtracted):\n";
    std::cout << "     mean : " << adj(mean) << " ns\n";
    std::cout << "     p50  : " << adj(percentile(lat_ns, 50)) << " ns\n";
    std::cout << "     p99  : " << adj(percentile(lat_ns, 99)) << " ns\n";
    std::cout << "     p99.9: " << adj(percentile(lat_ns, 99.9)) << " ns\n";
    std::cout << "  (raw p50 incl. clock: " << percentile(lat_ns, 50) << " ns)\n";
}

} // namespace

int main(int argc, char** argv) {
    std::size_t num_ops = 2'000'000;
    if (argc > 1) num_ops = static_cast<std::size_t>(std::stoull(argv[1]));

    const double clock_ns = measure_clock_overhead_ns();
    std::cout << "=== Limit Order Book Benchmark ===\n";
    std::cout << "operations per phase : " << num_ops << "\n";
    std::cout << "measured clock overhead (now() pair): " << clock_ns << " ns\n";

    std::mt19937_64 rng(42);
    const Price mid = 10'000;
    const Price band = 50;
    std::uniform_int_distribution<Price> price_dist(mid - band, mid + band);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<Quantity> qty_dist(1, 100);

    // ---- Phase 1: matching throughput (mixed crossing/resting limit orders) --
    {
        OrderBook book;
        std::vector<std::uint64_t> lat;
        lat.reserve(num_ops);
        std::uint64_t trades = 0;
        OrderId id = 1;

        const auto t0 = Clock::now();
        for (std::size_t i = 0; i < num_ops; ++i) {
            const Side side = side_dist(rng) ? Side::Buy : Side::Sell;
            const Price px = price_dist(rng);
            const Quantity qty = qty_dist(rng);

            const auto s = Clock::now();
            auto res = book.add_limit(id++, side, px, qty);
            const auto e = Clock::now();

            lat.push_back(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(e - s)
                    .count()));
            trades += res.trades.size();
        }
        const auto t1 = Clock::now();
        const double secs = std::chrono::duration<double>(t1 - t0).count();
        report("Phase 1: add_limit (match + rest)", lat, secs, clock_ns);
        std::cout << "  trades made       : " << trades << " ("
                  << (100.0 * static_cast<double>(trades) /
                      static_cast<double>(num_ops))
                  << "% of ops crossed)\n";
        std::cout << "  resting book      : " << book.order_count()
                  << " orders (index == physical: "
                  << (book.order_count() == book.physical_order_count() ? "yes"
                                                                        : "NO")
                  << ")\n";
    }

    // ---- Phase 2: cancel throughput on a large resting book -----------------
    {
        OrderBook book;
        std::vector<OrderId> ids;
        ids.reserve(num_ops);
        for (std::size_t i = 0; i < num_ops; ++i) {
            const OrderId oid = static_cast<OrderId>(i + 1);
            (void)book.add_limit(oid, Side::Buy,
                                 mid - 1 - static_cast<Price>(i % band),
                                 qty_dist(rng));
            ids.push_back(oid);
        }
        std::shuffle(ids.begin(), ids.end(), rng);

        std::vector<std::uint64_t> lat;
        lat.reserve(ids.size());
        const auto t0 = Clock::now();
        for (OrderId oid : ids) {
            const auto s = Clock::now();
            book.cancel(oid);
            const auto e = Clock::now();
            lat.push_back(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(e - s)
                    .count()));
        }
        const auto t1 = Clock::now();
        const double secs = std::chrono::duration<double>(t1 - t0).count();
        report("Phase 2: cancel (random order, large book)", lat, secs, clock_ns);
        std::cout << "  resting book      : " << book.order_count() << " orders\n";
    }

    return 0;
}
