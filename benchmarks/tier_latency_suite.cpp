// 5-Tier Latency Suite: measures depth-1 BFS latency across the full memory
// and storage hierarchy on a single degree-8 graph:
//   1. L1 cache      (100 hot starts,      ~30 KiB active working set)
//   2. L2 cache      (10,000 hot starts,   ~3 MiB active working set)
//   3. L3 / SLC      (150,000 hot starts,  ~30 MiB active working set)
//   4. SlabPool DRAM (2,000,000 starts,    ~240 MiB active, 100% FixedSlabPool)
//   5. RocksDB hit   (StorageBreadthFirstSearch, 100% pinned BlockCache hits)
//
// Usage:
//   ./build/low_latency_graphdb_tier_latency_suite [--nodes N] [--threads 1,2]
//       [--queries Q] [--batch B] [--depth D] [--cache-mb M]

#include "low_latency_graphdb/storage/rocksdb_graph_store.hpp"
#include "low_latency_graphdb/storage/storage_bfs.hpp"
#include "low_latency_graphdb/storage/tiered_graph.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using low_latency_graphdb::EdgeLightMeta;
using low_latency_graphdb::NodeId;
using low_latency_graphdb::storage::RocksDbGraphStore;
using low_latency_graphdb::storage::RocksDbGraphStoreOptions;
using low_latency_graphdb::storage::StorageBfsOptions;
using low_latency_graphdb::storage::StorageBfsState;
using low_latency_graphdb::storage::StorageBfsStats;
using low_latency_graphdb::storage::StorageBreadthFirstSearch;
using low_latency_graphdb::storage::TieredBfsOptions;
using low_latency_graphdb::storage::TieredBfsState;
using low_latency_graphdb::storage::TieredBfsStats;
using low_latency_graphdb::storage::TieredGraph;

struct Config {
    std::uint64_t nodes{2'000'000};
    std::uint64_t degree{8};
    std::size_t depth{1};
    std::size_t queries{40'000};
    std::size_t batch{64};
    std::size_t cache_mb{512};
    std::vector<std::size_t> threads{1, 2};
};

std::vector<std::size_t> parse_list(const std::string& text) {
    std::vector<std::size_t> out;
    std::stringstream ss(text);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (!item.empty()) out.push_back(std::stoull(item));
    }
    return out;
}

Config parse(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) std::exit(2);
            return argv[++i];
        };
        if (arg == "--nodes") cfg.nodes = std::stoull(next());
        else if (arg == "--degree") cfg.degree = std::stoull(next());
        else if (arg == "--depth") cfg.depth = std::stoull(next());
        else if (arg == "--queries") cfg.queries = std::stoull(next());
        else if (arg == "--batch") cfg.batch = std::stoull(next());
        else if (arg == "--cache-mb") cfg.cache_mb = std::stoull(next());
        else if (arg == "--threads") cfg.threads = parse_list(next());
        else {
            std::cerr << "unknown option: " << arg << '\n';
            std::exit(2);
        }
    }
    return cfg;
}

inline std::uint64_t splitmix64(std::uint64_t x) noexcept {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

inline std::uint64_t xorshift64(std::uint64_t& s) noexcept {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
}

// Regular degree-8 graph where every vertex [0, N) has 8 distinct out-neighbors
// spread across the graph, so any working-set size K <= N has K full internal
// nodes (unlike a tree where 87.5% of vertices are leaves).
void build_graph(const std::string& path, std::uint64_t nodes, std::uint64_t degree) {
    RocksDbGraphStoreOptions opts;
    opts.disable_wal = true;
    RocksDbGraphStore store(path, opts);
    std::vector<NodeId> targets(degree);
    std::vector<EdgeLightMeta> metas(degree);
    for (std::uint64_t v = 0; v < nodes; ++v) {
        for (std::uint64_t e = 0; e < degree; ++e) {
            // Keep neighbors within the same prefix window when v is small?
            // Use deterministic spread across [0, nodes) with no self-loop.
            NodeId dst = (v + 1 + e * 7919ull) % nodes;
            if (dst == v) dst = (dst + 1) % nodes;
            targets[e] = dst;
        }
        store.write_adjacency(v, targets, metas);
    }
    store.flush();
}

struct Summary {
    double p50{0}, p95{0}, p99{0}, qps{0};
    double hot_pct{0};
    double pinned_pct{0};
    double visited_per_q{0};
};

template <typename Fn>
Summary run_parallel(std::size_t threads, std::size_t queries_per_thread, std::size_t batch,
                     std::uint64_t start_range, Fn&& fn) {
    struct alignas(128) ThreadData {
        std::vector<double> samples_ns;
        std::size_t visited{0};
        std::size_t hot{0};
        std::size_t cold{0};
        std::size_t pinned{0};
        std::size_t fetched{0};
    };
    std::vector<ThreadData> data(threads);
    std::atomic<std::size_t> ready{0};
    std::atomic<std::size_t> finished{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> workers;
    for (std::size_t t = 0; t < threads; ++t) {
        workers.emplace_back([&, t] {
            std::uint64_t rng = splitmix64(t + 1);
            auto& d = data[t];
            d.samples_ns.reserve(queries_per_thread / std::max<std::size_t>(1, batch) + 1);
            std::size_t visited = 0, hot = 0, cold = 0, pinned = 0, fetched = 0;
            // Initialize thread_local state (QueryArena + ReaderHandle)
            // before timing starts.
            std::size_t dummy = 0;
            fn(0, dummy, dummy, dummy, dummy, dummy);
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) {
            }
            for (std::size_t q = 0; q < queries_per_thread; q += batch) {
                const std::size_t m = std::min<std::size_t>(batch, queries_per_thread - q);
                NodeId starts[256];
                const std::size_t n = std::min<std::size_t>(m, 256);
                for (std::size_t i = 0; i < n; ++i) {
                    starts[i] = xorshift64(rng) % start_range;
                }
                const auto t0 = Clock::now();
                for (std::size_t i = 0; i < n; ++i) {
                    fn(starts[i], visited, hot, cold, pinned, fetched);
                }
                const auto t1 = Clock::now();
                d.samples_ns.push_back(
                    std::chrono::duration<double, std::nano>(t1 - t0).count() / n);
            }
            d.visited = visited;
            d.hot = hot;
            d.cold = cold;
            d.pinned = pinned;
            d.fetched = fetched;
            // Mark completion before thread_local destructor (munmap) runs.
            if (finished.fetch_add(1, std::memory_order_acq_rel) + 1 == threads) {
                finished.notify_one();
            }
        });
    }
    while (ready.load(std::memory_order_acquire) < threads) {
        std::this_thread::yield();
    }
    const auto wall0 = Clock::now();
    go.store(true, std::memory_order_release);
    for (std::size_t done = finished.load(std::memory_order_acquire); done < threads;
         done = finished.load(std::memory_order_acquire)) {
        finished.wait(done, std::memory_order_acquire);
    }
    const double wall_s = std::chrono::duration<double>(Clock::now() - wall0).count();
    for (auto& w : workers) w.join();

    std::vector<double> all;
    std::size_t visited = 0, hot = 0, cold = 0, pinned = 0, fetched = 0;
    for (const auto& d : data) {
        all.insert(all.end(), d.samples_ns.begin(), d.samples_ns.end());
        visited += d.visited;
        hot += d.hot;
        cold += d.cold;
        pinned += d.pinned;
        fetched += d.fetched;
    }
    std::sort(all.begin(), all.end());
    auto pct = [&](double p) {
        return all[std::min(all.size() - 1, static_cast<std::size_t>(p * all.size()))];
    };
    const double total_q = static_cast<double>(threads * queries_per_thread);
    Summary s;
    s.p50 = pct(0.50);
    s.p95 = pct(0.95);
    s.p99 = pct(0.99);
    s.qps = total_q / wall_s;
    s.hot_pct = (hot + cold) ? 100.0 * hot / (hot + cold) : 0.0;
    s.pinned_pct = fetched ? 100.0 * pinned / fetched : 0.0;
    s.visited_per_q = visited / total_q;
    return s;
}

void print_row(const std::string& tier, std::uint64_t starts, std::size_t threads,
               const Summary& s, const std::string& source) {
    std::cout << std::left << std::setw(16) << tier
              << std::right << std::setw(10) << starts
              << std::setw(5) << threads << "T"
              << "  p50=" << std::fixed << std::setprecision(0) << std::setw(6) << s.p50 << "ns"
              << "  p95=" << std::setw(6) << s.p95 << "ns"
              << "  p99=" << std::setw(6) << s.p99 << "ns"
              << "  qps=" << std::setw(10) << s.qps
              << "  " << source << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    const Config cfg = parse(argc, argv);
    const auto path = std::filesystem::temp_directory_path() / "llg_tier_latency_suite";
    std::filesystem::remove_all(path);

    std::cout << "5-Tier Latency Suite: nodes=" << cfg.nodes << " degree=" << cfg.degree
              << " depth=" << cfg.depth << " queries/thread=" << cfg.queries
              << " batch=" << cfg.batch << " rocksdb_cache_mb=" << cfg.cache_mb << '\n';

    const auto t_build = Clock::now();
    build_graph(path.string(), cfg.nodes, cfg.degree);
    std::cout << "RocksDB built in " << std::fixed << std::setprecision(2)
              << std::chrono::duration<double>(Clock::now() - t_build).count() << "s\n";

    {
        RocksDbGraphStoreOptions opts;
        opts.topo_block_cache_bytes = cfg.cache_mb << 20;
        RocksDbGraphStore store(path.string(), opts);

        TieredGraph tiered(store, cfg.nodes);
        TieredBfsOptions t_opts;
        t_opts.max_depth = cfg.depth;
        t_opts.max_visited = 1'000'000;

        // Read-through populate all N vertices via normal depth-1 queries so
        // every tier starts in 100% steady-state FixedSlabPool residency.
        const auto t_load = Clock::now();
        {
            TieredBfsState load_state;
            for (NodeId v = 0; v < cfg.nodes; ++v) {
                (void)tiered.run(v, t_opts, load_state);
            }
        }
        const auto t_stats = tiered.hot_tier().stats();
        std::cout << "FixedSlabPool populated on-demand in "
                  << std::chrono::duration<double>(Clock::now() - t_load).count()
                  << "s (" << t_stats.loaded_nodes << " slabs, " << std::setprecision(1)
                  << t_stats.bytes / 1048576.0 << " MiB)\n\n";

        struct TierCase {
            const char* name;
            std::uint64_t starts;
            std::size_t query_mult;
        };
        const std::vector<TierCase> slab_cases = {
            {"1. L1 cache", std::min<std::uint64_t>(100, cfg.nodes), 10},
            {"2. L2 cache", std::min<std::uint64_t>(10'000, cfg.nodes), 5},
            {"3. L3 / SLC", std::min<std::uint64_t>(150'000, cfg.nodes), 2},
            {"4. SlabPool DRAM", cfg.nodes, 1},
        };

        for (const auto& tc : slab_cases) {
            // Short warmup of the target working set into its natural cache level.
            {
                TieredBfsState warm_state;
                std::uint64_t rng = 0x123456789ABCDEF0ull;
                const std::size_t warm_q = std::min<std::size_t>(tc.starts * 2, 200'000);
                for (std::size_t i = 0; i < warm_q; ++i) {
                    (void)tiered.run(xorshift64(rng) % tc.starts, t_opts, warm_state);
                }
            }
            for (const std::size_t threads : cfg.threads) {
                const Summary s = run_parallel(
                    threads, cfg.queries * tc.query_mult, cfg.batch, tc.starts,
                    [&](NodeId start, std::size_t& visited, std::size_t& hot, std::size_t& cold,
                        std::size_t&, std::size_t&) {
                        thread_local TieredBfsState state;
                        TieredBfsStats stats;
                        visited += tiered.run(start, t_opts, state, nullptr, &stats);
                        hot += stats.hot_rows;
                        cold += stats.cold_rows;
                    });
                std::ostringstream src;
                src << "hot=" << std::fixed << std::setprecision(1) << s.hot_pct << "%";
                print_row(tc.name, tc.starts, threads, s, src.str());
            }
        }

        // 5. Hit RocksDB (StorageBreadthFirstSearch, BlockCache hit)
        StorageBreadthFirstSearch storage_bfs(store);
        StorageBfsOptions s_opts;
        s_opts.max_depth = cfg.depth;
        const std::uint64_t rocks_starts = std::min<std::uint64_t>(10'000, cfg.nodes);
        {
            StorageBfsState warm_state;
            for (NodeId v = 0; v < rocks_starts; ++v) {
                (void)storage_bfs.run(v, s_opts, warm_state);
            }
        }
        for (const std::size_t threads : cfg.threads) {
            const Summary s = run_parallel(
                threads, cfg.queries, cfg.batch, rocks_starts,
                [&](NodeId start, std::size_t& visited, std::size_t&, std::size_t&,
                    std::size_t& pinned, std::size_t& fetched) {
                    thread_local StorageBfsState state;
                    StorageBfsStats stats;
                    visited += storage_bfs.run(start, s_opts, state, &stats).visited.size();
                    pinned += stats.rows_pinned;
                    fetched += stats.rows_fetched;
                });
            std::ostringstream src;
            src << "pinned=" << std::fixed << std::setprecision(1) << s.pinned_pct << "%";
            print_row("5. RocksDB hit", rocks_starts, threads, s, src.str());
        }
    }

    std::filesystem::remove_all(path);
    return 0;
}
