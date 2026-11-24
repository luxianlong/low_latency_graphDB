// RocksDB-backed BFS benchmark.
//
// Usage: rocksdb_bfs_benchmark [--nodes N] [--fanout F] [--depth D]
//          [--threads T1,T2,...] [--queries Q] [--pin-batch B]
//          [--cache-mb M] [--path DIR] [--keep] [--no-baseline]
//          [--hot-nodes K] [--no-snapshot] [--point-get N]
//          [--hot-tier K] [--hot-pct P] [--collect] [--batch B]
//
// Rows: `rocksdb` reads every hop from RocksDB; `tiered` serves vertex IDs
// [0, K) (the top of the tree) from the in-memory hot tier and the rest from
// RocksDB; `in-memory` is the full-graph baseline. P% of queries start at a
// vertex whose whole depth-D neighborhood is hot (a skewed workload); the
// rest start uniformly at random.
// --collect also materializes visited IDs (default: count only). --batch B
// times B queries per sample and reports the per-query mean, which removes
// timer overhead from sub-microsecond results.
//
// Builds a deterministic fanout-F tree (node v has children F*v+1..F*v+F plus a
// back-edge to its parent), bulk-loads it, reopens the DB, warms the block
// cache, then times Q depth-D BFS queries per thread. The in-memory BFS on the
// same graph is measured as a baseline.
#include "low_latency_graphdb/storage/storage_bfs.hpp"
#include "low_latency_graphdb/storage/tiered_graph.hpp"
#include "low_latency_graphdb/testing/breadth_first_search.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace low_latency_graphdb;
using namespace low_latency_graphdb::storage;
using Clock = std::chrono::steady_clock;

namespace {

struct Config {
    std::uint64_t nodes{200'000};
    std::uint64_t fanout{8};
    std::size_t depth{3};
    std::vector<std::size_t> threads{1, 4};
    std::size_t queries{2000};
    std::size_t pin_batch{1024};
    std::size_t cache_mb{256};
    std::string path;
    bool keep{false};
    bool baseline{true};
    bool snapshot{true};
    std::size_t point_get{0};
    std::uint64_t hot_nodes{0};  // 0 = all internal nodes
    std::uint64_t hot_tier{std::numeric_limits<std::uint64_t>::max()};  // default nodes/20
    std::uint64_t hot_pct{90};
    std::uint64_t hot_start_range{1};  // starts whose full neighborhood is hot
    bool collect{false};
    std::size_t batch{1};  // queries per timed sample
};

[[noreturn]] void usage(const char* message) {
    std::cerr << "error: " << message << "\nsee header comment for usage\n";
    std::exit(2);
}

std::vector<std::size_t> parse_list(const std::string& text) {
    std::vector<std::size_t> out;
    std::stringstream stream(text);
    std::string item;
    while (std::getline(stream, item, ',')) {
        out.push_back(std::stoull(item));
    }
    if (out.empty()) usage("empty --threads");
    return out;
}

Config parse(int argc, char** argv) {
    Config config;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) usage("missing value");
            return argv[++i];
        };
        if (arg == "--nodes") config.nodes = std::stoull(next());
        else if (arg == "--fanout") config.fanout = std::stoull(next());
        else if (arg == "--depth") config.depth = std::stoull(next());
        else if (arg == "--threads") config.threads = parse_list(next());
        else if (arg == "--queries") config.queries = std::stoull(next());
        else if (arg == "--pin-batch") config.pin_batch = std::stoull(next());
        else if (arg == "--cache-mb") config.cache_mb = std::stoull(next());
        else if (arg == "--path") config.path = next();
        else if (arg == "--keep") config.keep = true;
        else if (arg == "--no-baseline") config.baseline = false;
        else if (arg == "--no-snapshot") config.snapshot = false;
        else if (arg == "--hot-nodes") config.hot_nodes = std::stoull(next());
        else if (arg == "--hot-tier") config.hot_tier = std::stoull(next());
        else if (arg == "--hot-pct") config.hot_pct = std::stoull(next());
        else if (arg == "--collect") config.collect = true;
        else if (arg == "--batch") config.batch = std::stoull(next());
        else if (arg == "--point-get") config.point_get = std::stoull(next());
        else usage(("unknown option " + arg).c_str());
    }
    if (config.nodes < 2 || config.fanout < 1 || config.queries == 0) {
        usage("invalid sizes");
    }
    if (config.hot_tier == std::numeric_limits<std::uint64_t>::max()) {
        config.hot_tier = config.nodes;
    }
    // Hot tier = vertex IDs [0, K), the top of the tree. A start below
    // K / fanout^depth has its whole depth-D neighborhood inside the tier.
    std::uint64_t reach = 1;
    for (std::size_t d = 0; d < config.depth && reach <= config.hot_tier; ++d) {
        reach *= config.fanout;
    }
    config.hot_start_range = std::max<std::uint64_t>(1, config.hot_tier / reach);
    return config;
}

std::uint64_t next_random(std::uint64_t& state) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

// Starts are drawn from internal nodes so every query has work to do.
std::uint64_t pick_start(const Config& config, std::uint64_t& state) {
    const std::uint64_t internal = config.hot_nodes != 0
        ? config.hot_nodes
        : std::max<std::uint64_t>(1, config.nodes / config.fanout);
    const std::uint64_t draw = next_random(state);
    if (config.hot_nodes == 0 && config.hot_tier != 0 && draw % 100 < config.hot_pct) {
        return (draw >> 8) % std::min(config.hot_start_range, internal);
    }
    return (draw >> 8) % internal;
}

void build_database(const Config& config) {
    RocksDbGraphStoreOptions options;
    options.disable_wal = true;
    RocksDbGraphStore store(config.path, options);
    std::vector<std::uint64_t> targets;
    for (std::uint64_t v = 0; v < config.nodes; ++v) {
        targets.clear();
        for (std::uint64_t c = config.fanout * v + 1;
             c <= config.fanout * v + config.fanout && c < config.nodes; ++c) {
            targets.push_back(c);
        }
        if (v > 0) targets.push_back((v - 1) / config.fanout);
        std::vector<EdgeLightMeta> metas(targets.size());
        store.write_adjacency(v, targets, metas);
    }
    store.flush();
}

struct Sample {
    std::vector<double> latencies_ns;
    StorageBfsStats stats;
    std::size_t visited{0};
    std::size_t hot_rows{0};
    std::size_t cold_rows{0};
};

struct Summary {
    double p50, p95, p99, max, mean, qps;
};

Summary summarize(std::vector<double> all, double wall_seconds, std::size_t batch) {
    std::sort(all.begin(), all.end());
    auto pct = [&](double q) {
        return all[std::min(all.size() - 1, static_cast<std::size_t>(q * all.size()))];
    };
    double sum = 0;
    for (double v : all) sum += v;
    return {pct(0.50), pct(0.95), pct(0.99), all.back(), sum / all.size(),
            all.size() * batch / wall_seconds};
}

void print_row(const std::string& label, std::size_t threads, const Summary& s,
               const std::string& extra) {
    std::cout << std::left << std::setw(10) << label << std::right << std::setw(4)
              << threads << "T  p50=" << std::fixed << std::setprecision(0) << std::setw(8)
              << s.p50 << "ns  p95=" << std::setw(8) << s.p95 << "ns  p99=" << std::setw(8)
              << s.p99 << "ns  max=" << std::setw(9) << s.max << "ns  mean=" << std::setw(8)
              << s.mean << "ns  qps=" << std::setprecision(0) << std::setw(9) << s.qps
              << extra << '\n';
}

template <typename QueryFn>
std::pair<std::vector<Sample>, double> run_threads(const Config& config, std::size_t threads,
                                                   QueryFn&& query) {
    std::vector<Sample> samples(threads);
    std::atomic<bool> go{false};
    std::vector<std::thread> workers;
    for (std::size_t t = 0; t < threads; ++t) {
        workers.emplace_back([&, t] {
            std::uint64_t rng = 0x9E3779B97F4A7C15ull ^ (t + 1) * 0xD1B54A32D192ED03ull;
            samples[t].latencies_ns.reserve(config.queries);
            while (!go.load(std::memory_order_acquire)) {
            }
            for (std::size_t q = 0; q < config.queries; q += config.batch) {
                const std::size_t n = std::min(config.batch, config.queries - q);
                std::uint64_t starts[256];
                const std::size_t m = std::min<std::size_t>(n, 256);
                for (std::size_t i = 0; i < m; ++i) starts[i] = pick_start(config, rng);
                const auto begin = Clock::now();
                for (std::size_t i = 0; i < m; ++i) query(starts[i], samples[t]);
                const auto end = Clock::now();
                samples[t].latencies_ns.push_back(
                    std::chrono::duration<double, std::nano>(end - begin).count() / m);
            }
        });
    }
    const auto begin = Clock::now();
    go.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();
    const double wall = std::chrono::duration<double>(Clock::now() - begin).count();
    return {std::move(samples), wall};
}

std::vector<double> merge(const std::vector<Sample>& samples) {
    std::vector<double> all;
    for (const auto& s : samples) all.insert(all.end(), s.latencies_ns.begin(), s.latencies_ns.end());
    return all;
}

}  // namespace

int main(int argc, char** argv) {
    const Config config = parse(argc, argv);
    const bool temp = config.path.empty();
    Config run_config = config;
    if (temp) {
        run_config.path =
            (std::filesystem::temp_directory_path() / "llg_rocksdb_bfs_bench").string();
    }
    std::filesystem::remove_all(run_config.path);

    std::cout << "nodes=" << config.nodes << " fanout=" << config.fanout
              << " depth=" << config.depth << " queries/thread=" << config.queries
              << " pin_batch=" << config.pin_batch << " cache_mb=" << config.cache_mb
              << " hw_threads=" << std::thread::hardware_concurrency() << '\n';

    const auto load_begin = Clock::now();
    build_database(run_config);
    std::cout << "bulk load + flush: " << std::fixed << std::setprecision(2)
              << std::chrono::duration<double>(Clock::now() - load_begin).count() << "s\n";

    RocksDbGraphStoreOptions options;
    options.topo_block_cache_bytes = config.cache_mb << 20;
    RocksDbGraphStore store(run_config.path, options);  // reopened: cold cache
    StorageBreadthFirstSearch bfs(store);
    StorageBfsOptions bfs_options;
    bfs_options.max_depth = config.depth;
    bfs_options.pin_batch_size = config.pin_batch;
    bfs_options.max_visited = 10'000'000;
    bfs_options.use_snapshot = config.snapshot;
    store.set_point_get_threshold(config.point_get);

    // Warm the block cache with a single pass over randomly chosen queries.
    {
        std::uint64_t rng = 12345;
        for (std::size_t q = 0; q < config.queries; ++q) {
            (void)bfs.run(pick_start(config, rng), bfs_options);
        }
    }

    for (const std::size_t threads : config.threads) {
        auto [samples, wall] = run_threads(config, threads, [&](std::uint64_t start, Sample& s) {
            StorageBfsStats stats;
            thread_local StorageBfsState state;
            s.visited += bfs.run(start, bfs_options, state, &stats).visited.size();
            s.stats.rows_fetched += stats.rows_fetched;
            s.stats.rows_pinned += stats.rows_pinned;
            s.stats.rows_copied += stats.rows_copied;
            s.stats.aligned_views += stats.aligned_views;
        });
        std::size_t visited = 0, fetched = 0, pinned = 0, aligned = 0;
        for (const auto& s : samples) {
            visited += s.visited;
            fetched += s.stats.rows_fetched;
            pinned += s.stats.rows_pinned;
            aligned += s.stats.aligned_views;
        }
        const double queries = static_cast<double>(config.queries * threads);
        std::ostringstream extra;
        extra << std::fixed << std::setprecision(1) << "  visited/q=" << visited / queries
              << " pinned=" << (fetched ? 100.0 * pinned / fetched : 0.0) << "%"
              << " aligned=" << (fetched ? 100.0 * aligned / fetched : 0.0) << "%";
        print_row("rocksdb", threads, summarize(merge(samples), wall, config.batch), extra.str());
    }

    if (config.hot_tier != 0) {
        TieredGraph tiered(store, config.hot_tier);
        TieredBfsOptions tiered_options;
        tiered_options.max_depth = config.depth;
        tiered_options.pin_batch_size = config.pin_batch;
        tiered_options.max_visited = 10'000'000;
        for (const std::size_t threads : config.threads) {
            auto [samples, wall] = run_threads(config, threads, [&](std::uint64_t start, Sample& s) {
                thread_local TieredBfsState state;
                thread_local std::vector<NodeId> ids;
                TieredBfsStats stats;
                ids.clear();
                s.visited += tiered.run(start, tiered_options, state,
                                        config.collect ? &ids : nullptr, &stats);
                s.hot_rows += stats.hot_rows;
                s.cold_rows += stats.cold_rows;
            });
            std::size_t visited = 0, hot = 0, cold = 0;
            for (const auto& s : samples) {
                visited += s.visited;
                hot += s.hot_rows;
                cold += s.cold_rows;
            }
            const double queries = static_cast<double>(config.queries * threads);
            std::ostringstream extra;
            extra << std::fixed << std::setprecision(1) << "  visited/q=" << visited / queries
                  << " hot_rows=" << (hot + cold ? 100.0 * hot / (hot + cold) : 0.0) << "%";
            print_row("tiered", threads, summarize(merge(samples), wall, config.batch), extra.str());
        }
        const auto tier_stats = tiered.hot_tier().stats();
        std::cout << "hot tier (read-through FixedSlabPool): " << tier_stats.loaded_nodes
                  << " loaded / " << tier_stats.registered_nodes << " registered / "
                  << tier_stats.pool_capacity << " slots, " << std::fixed << std::setprecision(1)
                  << tier_stats.bytes / 1048576.0 << " MiB\n";
    }

    if (config.baseline) {
        InMemoryGraph graph;
        store.load_into(graph);
        const BreadthFirstSearch memory_bfs(graph);
        for (const std::size_t threads : config.threads) {
            auto [samples, wall] = run_threads(config, threads, [&](std::uint64_t start, Sample& s) {
                s.visited += memory_bfs.run(start, config.depth).visited.size();
            });
            print_row("in-memory", threads, summarize(merge(samples), wall, config.batch), "");
        }
    }

    if (temp && !config.keep) {
        std::filesystem::remove_all(run_config.path);
    }
    return 0;
}
