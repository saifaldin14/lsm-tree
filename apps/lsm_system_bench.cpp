// In-process system benchmark (db_bench style) for compaction offload.
//
// preload: builds a key space of --records keys (version 0) with the WAL
//   disabled, waits until compaction is idle, and verifies every key.
// run: opens a copy of a preloaded database with the selected compaction
//   backend and runs closed-loop writer/reader threads inside the process for
//   --warmup-seconds + --duration-seconds with auto compaction enabled. Each
//   thread owns the keys congruent to its index and remembers the version it
//   last wrote, so reads are checked online and every key is checked exactly
//   after the run.
//
// The whole process (foreground threads and RocksDB background threads) is
// expected to be confined to a core budget by the caller (numactl).

#include "lsm_lab/block_transform.hpp"
#include "lsm_lab/db_options.hpp"
#include "lsm_lab/process_accounting.hpp"
#include "lsm_lab/replication.hpp"
#include "lsm_lab/runtime.hpp"

#include <rocksdb/db.h>
#include <rocksdb/env.h>
#include <rocksdb/iostats_context.h>
#include <rocksdb/perf_context.h>
#include <rocksdb/perf_level.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/listener.h>
#include <rocksdb/options.h>
#include <rocksdb/statistics.h>
#include <rocksdb/table.h>

#include <pthread.h>
#include <sys/resource.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace
{
    struct Config
    {
        std::string phase;
        fs::path dir;
        std::string mode = "native_cpu";
        std::uint64_t records = 1000000;
        std::uint32_t key_bytes = 16;
        std::uint32_t value_bytes = 128;
        std::string compression = "lz4";
        std::uint64_t block_bytes = 4096;
        std::uint64_t write_buffer_bytes = 64ULL << 20;
        std::uint64_t target_file_bytes = 64ULL << 20;
        std::uint64_t max_bytes_for_level_base = 256ULL << 20;
        std::uint32_t max_write_buffer_number = 4;
        std::uint32_t background_jobs = 4;
        std::uint32_t subcompactions = 1;
        std::uint32_t levels = 7;
        std::uint64_t block_cache_bytes = 256ULL << 20;
        std::uint32_t threads = 4;
        std::uint32_t read_percent = 0;
        std::uint32_t warmup_seconds = 30;
        std::uint32_t duration_seconds = 120;
        bool disable_wal = false;
        bool adaptive_yield = true;
        // Puts per WriteBatch; write latency is then per batch.
        std::uint32_t batch_size = 1;
        // Operation mix (percent of operations); the rest are puts. YCSB A: 50 reads;
        // B: 95 reads; F: 50 read-modify-writes.
        std::uint32_t rmw_percent = 0;
        std::uint32_t delete_percent = 0;
        std::string distribution = "uniform";
        double zipf_theta = 0.99;
        // Workload shape (Phase 6 workloads; see docs/workloads.md). Keys and values may vary in
        // size per key (key_bytes..key_bytes_max, value_bytes..value_bytes_max; 0 = fixed); keys
        // may start with one of key_families prefix bytes; scan_percent of operations read
        // scan_length consecutive keys. workload "timeseries" stores records / retention series
        // of `retention` points each: an append writes the next point of a series and deletes
        // the point that leaves the retention window.
        std::string workload = "kv";
        std::uint32_t key_bytes_max = 0;
        std::uint32_t value_bytes_max = 0;
        std::uint32_t key_families = 0;
        std::uint32_t scan_percent = 0;
        std::uint32_t scan_length = 16;
        std::uint64_t retention = 0;
        bool use_direct_io = false;
        // Compaction shape (0 keeps RocksDB defaults).
        std::uint32_t l0_trigger = 0;
        double level_multiplier = 0;
        // RocksDB's integrated key/value separation (BlobDB): values of at least this many bytes
        // go to blob files, with blob garbage collection (0 disables).
        std::uint32_t blob_min_bytes = 0;
        bool pipelined_write = false;
        bool concurrent_memtable = true;
        std::uint32_t flush_jobs = 0;
        bool allow_fallocate = true;
        std::string bottommost_compression;
        // Primary-backup replication (lsm_lab/replication.hpp): none, physical or logical, to the
        // comma-separated lsm_replica_server endpoints; logical replicas compact with replica_backend.
        std::string replication = "none";
        std::string replicate_to;
        std::string replica_backend = "native_cpu";
        bool verify = true;
        std::uint64_t seed = 1;
        // Open loop: operations (keys) per second over all clients, issued on a fixed schedule;
        // latency is measured from each operation's scheduled time, so time spent behind
        // schedule counts (no coordinated omission). 0 runs closed loop.
        double rate = 0;
    };

    std::uint64_t mix64(std::uint64_t value)
    {
        value += 0x9e3779b97f4a7c15ULL;
        value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
        return value ^ (value >> 31);
    }

    std::uint32_t key_size(const Config &config, std::uint64_t index)
    {
        if (config.key_bytes_max <= config.key_bytes)
            return config.key_bytes;
        return config.key_bytes + static_cast<std::uint32_t>(mix64(index ^ 0x6b6579ULL) % (config.key_bytes_max - config.key_bytes + 1));
    }

    // Bytes 0-7: hash of the index (spreads keys), bytes 8-15: big-endian index, then padding
    // up to the key's size. With key families, byte 0 is the family (index % families), so each
    // family is a contiguous key range.
    void make_key(const Config &config, std::uint64_t index, std::string &key)
    {
        key.assign(key_size(config, index), '\0');
        const auto prefix = mix64(index ^ config.seed);
        for (std::size_t offset = 0; offset < key.size(); ++offset)
        {
            const auto source = offset < 8 ? prefix : index;
            key[offset] = static_cast<char>((source >> (8 * (7 - offset % 8))) & 255);
        }
        if (config.key_families > 0)
            key[0] = static_cast<char>('A' + index % config.key_families);
    }

    // Time series: point t of series s has key (s, t), both big-endian, so a series is stored
    // in time order. Its slot is s * retention + t % retention and its version t / retention.
    void make_series_key(std::uint64_t series, std::uint64_t time, std::string &key)
    {
        key.assign(16, '\0');
        for (std::size_t offset = 0; offset < 8; ++offset)
        {
            key[offset] = static_cast<char>((series >> (8 * (7 - offset))) & 255);
            key[8 + offset] = static_cast<char>((time >> (8 * (7 - offset))) & 255);
        }
    }

    std::uint64_t big_endian64(const char *bytes)
    {
        std::uint64_t value = 0;
        for (std::size_t offset = 0; offset < 8; ++offset)
            value = (value << 8) | static_cast<unsigned char>(bytes[offset]);
        return value;
    }

    std::uint64_t key_index(const rocksdb::Slice &key)
    {
        std::uint64_t index = 0;
        for (std::size_t offset = 8; offset < 16; ++offset)
            index = (index << 8) | static_cast<unsigned char>(key[offset]);
        return index;
    }

    // Half pseudorandom, half repeating pattern (compresses roughly 2:1 with LZ4).
    // Bit 31 of a tracked version marks the key as deleted.
    constexpr std::uint32_t kDeleted = 1U << 31;

    void make_value(const Config &config, std::uint64_t index, std::uint64_t version, std::string &value)
    {
        version &= ~std::uint64_t{kDeleted};
        value.resize(config.value_bytes_max <= config.value_bytes
                         ? config.value_bytes
                         : config.value_bytes + mix64(index * 0x9e3779b1ULL + version) % (config.value_bytes_max - config.value_bytes + 1));
        std::uint64_t word = 0;
        const auto base = mix64(index * 1315423911ULL + version);
        for (std::size_t offset = 0; offset < value.size(); ++offset)
        {
            if (offset < value.size() / 2)
            {
                if (offset % 8 == 0)
                    word = mix64(base + offset);
                value[offset] = static_cast<char>((word >> ((offset % 8) * 8)) & 255);
            }
            else
            {
                value[offset] = static_cast<char>('a' + (offset + version) % 16);
            }
        }
    }

    void check(const rocksdb::Status &status, const char *what)
    {
        if (!status.ok())
            throw std::runtime_error(std::string(what) + ": " + status.ToString());
    }

    struct CpuTimes
    {
        double user = 0;
        double system = 0;
    };

    CpuTimes process_cpu()
    {
        rusage usage{};
        getrusage(RUSAGE_SELF, &usage);
        return {usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6, usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6};
    }

    double unix_seconds()
    {
        return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    class CompactionTotals final : public rocksdb::EventListener
    {
    public:
        void OnCompactionCompleted(rocksdb::DB *, const rocksdb::CompactionJobInfo &info) override
        {
            jobs.fetch_add(1, std::memory_order_relaxed);
            if (!info.status.ok())
                failed.fetch_add(1, std::memory_order_relaxed);
            cpu_micros.fetch_add(info.stats.cpu_micros, std::memory_order_relaxed);
            elapsed_micros.fetch_add(info.stats.elapsed_micros, std::memory_order_relaxed);
            input_bytes.fetch_add(info.stats.total_input_bytes, std::memory_order_relaxed);
            output_bytes.fetch_add(info.stats.total_output_bytes, std::memory_order_relaxed);
            input_records.fetch_add(info.stats.num_input_records, std::memory_order_relaxed);
        }
        Json snapshot() const
        {
            return {{"jobs", jobs.load()}, {"failed", failed.load()}, {"cpu_micros", cpu_micros.load()},
                    {"elapsed_micros", elapsed_micros.load()}, {"input_bytes", input_bytes.load()},
                    {"output_bytes", output_bytes.load()}, {"input_records", input_records.load()}};
        }
        std::atomic<std::uint64_t> jobs{0}, failed{0}, cpu_micros{0}, elapsed_micros{0}, input_bytes{0},
            output_bytes{0}, input_records{0};
    };

    lsm_lab::DbShape db_shape(const Config &config)
    {
        lsm_lab::DbShape shape;
        shape.write_buffer_bytes = config.write_buffer_bytes;
        shape.max_write_buffer_number = config.max_write_buffer_number;
        shape.target_file_bytes = config.target_file_bytes;
        shape.max_bytes_for_level_base = config.max_bytes_for_level_base;
        shape.background_jobs = config.background_jobs;
        shape.subcompactions = config.subcompactions;
        shape.levels = config.levels;
        shape.l0_trigger = config.l0_trigger;
        shape.level_multiplier = config.level_multiplier;
        shape.use_direct_io = config.use_direct_io;
        shape.adaptive_yield = config.adaptive_yield;
        shape.compression = config.compression;
        shape.block_bytes = config.block_bytes;
        shape.block_cache_bytes = config.block_cache_bytes;
        shape.blob_min_bytes = config.blob_min_bytes;
        shape.pipelined_write = config.pipelined_write;
        shape.concurrent_memtable = config.concurrent_memtable;
        shape.flush_jobs = config.flush_jobs;
        shape.allow_fallocate = config.allow_fallocate;
        shape.bottommost_compression = config.bottommost_compression;
        return shape;
    }

    rocksdb::Options make_options(const Config &config, const std::shared_ptr<lsm_lab::BatchTransform> &transform)
    {
        return lsm_lab::make_db_options(db_shape(config), transform);
    }

    std::shared_ptr<lsm_lab::BatchTransform> make_backend(const std::string &mode)
    {
        if (mode == "native_cpu")
            return nullptr;
        if (mode == "gpu_output")
            return lsm_lab::make_transform("cuda_hybrid");
        if (mode == "cuda_merge" || mode == "cpu_merge" || mode == "remote_merge")
            return lsm_lab::make_transform(mode);
        throw std::invalid_argument("mode must be native_cpu, gpu_output, cpu_merge, cuda_merge or remote_merge");
    }

    Json level_shape(rocksdb::DB *db)
    {
        rocksdb::ColumnFamilyMetaData meta;
        db->GetColumnFamilyMetaData(&meta);
        Json levels = Json::array();
        for (const auto &level : meta.levels)
        {
            if (!level.files.empty())
                levels.push_back({{"level", level.level}, {"files", level.files.size()}, {"bytes", level.size}});
        }
        return levels;
    }

    std::uint64_t property(rocksdb::DB *db, const char *name)
    {
        std::uint64_t value = 0;
        db->GetIntProperty(name, &value);
        return value;
    }

    // For properties RocksDB reports only as strings (e.g. rocksdb.num-files-at-level<N>).
    std::uint64_t string_property(rocksdb::DB *db, const char *name)
    {
        std::string value;
        return db->GetProperty(name, &value) && !value.empty() ? std::stoull(value) : 0;
    }

    void wait_for_compactions(rocksdb::DB *db)
    {
        rocksdb::WaitForCompactOptions wait;
        wait.flush = true;
        check(db->WaitForCompact(wait), "wait for compaction");
    }

    // Every key must be present exactly once with the version its owner last wrote.
    Json verify_all(rocksdb::DB *db, const Config &config, const std::vector<std::uint32_t> &versions)
    {
        const auto started = Clock::now();
        rocksdb::ReadOptions read;
        read.verify_checksums = true;
        read.fill_cache = false;
        std::unique_ptr<rocksdb::Iterator> iterator(db->NewIterator(read));
        std::vector<bool> seen(config.records, false);
        std::uint64_t count = 0;
        std::string expected;
        for (iterator->SeekToFirst(); iterator->Valid(); iterator->Next())
        {
            const auto key = iterator->key();
            if (key.size() < 16)
                throw std::runtime_error("verification: unexpected key size");
            std::uint64_t index = 0;
            if (config.workload == "timeseries")
            {
                // Point t of series s lives in slot s * retention + t % retention at version t / retention.
                const auto series = big_endian64(key.data());
                const auto time = big_endian64(key.data() + 8);
                index = series * config.retention + time % config.retention;
                if (key.size() != 16 || index >= config.records || versions[index] != time / config.retention)
                    throw std::runtime_error("verification: unexpected time-series point " + std::to_string(series) + "/" +
                                             std::to_string(time));
            }
            else
            {
                index = key_index(key);
                if (index < config.records && key.size() != key_size(config, index))
                    throw std::runtime_error("verification: unexpected key size");
            }
            if (index >= config.records || seen[index])
                throw std::runtime_error("verification: unexpected or duplicate key " + std::to_string(index));
            seen[index] = true;
            if (versions[index] & kDeleted)
                throw std::runtime_error("verification: deleted key " + std::to_string(index) + " is present");
            make_value(config, index, versions[index], expected);
            if (iterator->value() != rocksdb::Slice(expected))
                throw std::runtime_error("verification: wrong value for key " + std::to_string(index) + " (expected version " +
                                         std::to_string(versions[index]) + ")");
            ++count;
        }
        check(iterator->status(), "verification iterator");
        std::uint64_t live = 0;
        for (const auto version : versions)
            live += (version & kDeleted) ? 0 : 1;
        if (count != live)
            throw std::runtime_error("verification: found " + std::to_string(count) + " of " + std::to_string(live) + " live keys");
        return {{"keys", count}, {"deleted_keys", config.records - live},
                {"seconds", std::chrono::duration<double>(Clock::now() - started).count()}};
    }

    // Log-linear latency histogram: 16 sub-buckets per power of two of nanoseconds.
    struct Histogram
    {
        std::array<std::uint64_t, 64 * 16> buckets{};
        std::uint64_t count = 0;
        std::uint64_t maximum = 0;
        static std::size_t bucket(std::uint64_t ns)
        {
            if (ns < 16)
                return static_cast<std::size_t>(ns);
            const int top = 63 - __builtin_clzll(ns);
            return static_cast<std::size_t>(top - 3) * 16 + ((ns >> (top - 4)) & 15);
        }
        static std::uint64_t upper(std::size_t index)
        {
            if (index < 16)
                return index;
            const auto top = index / 16 + 3;
            const auto sub = index % 16;
            return ((16 + sub + 1) << (top - 4)) - 1;
        }
        void add(std::uint64_t ns)
        {
            ++buckets[bucket(ns)];
            ++count;
            maximum = std::max(maximum, ns);
        }
        void merge(const Histogram &other)
        {
            for (std::size_t index = 0; index < buckets.size(); ++index)
                buckets[index] += other.buckets[index];
            count += other.count;
            maximum = std::max(maximum, other.maximum);
        }
        std::uint64_t percentile(double fraction) const
        {
            if (count == 0)
                return 0;
            const auto target = static_cast<std::uint64_t>(std::ceil(fraction * static_cast<double>(count)));
            std::uint64_t seen = 0;
            for (std::size_t index = 0; index < buckets.size(); ++index)
            {
                seen += buckets[index];
                if (seen >= target)
                    return std::min(upper(index), maximum);
            }
            return maximum;
        }
        Json json() const
        {
            return {{"count", count}, {"p50_ns", percentile(0.50)}, {"p95_ns", percentile(0.95)}, {"p99_ns", percentile(0.99)},
                    {"p999_ns", percentile(0.999)}, {"max_ns", maximum}, {"unit", "nanoseconds"}};
        }
    };

    // YCSB ZipfianGenerator (Gray et al.): ranks 0..n-1, rank 0 most popular.
    class Zipfian
    {
    public:
        Zipfian(std::uint64_t items, double theta) : items_(items)
        {
            if (items < 2 || theta <= 0 || theta >= 1)
                throw std::invalid_argument("zipfian needs >= 2 items and 0 < theta < 1");
            double zeta = 0;
            for (std::uint64_t index = 1; index <= items; ++index)
                zeta += 1.0 / std::pow(static_cast<double>(index), theta);
            zetan_ = zeta;
            const double zeta2 = 1.0 + 1.0 / std::pow(2.0, theta);
            alpha_ = 1.0 / (1.0 - theta);
            eta_ = (1.0 - std::pow(2.0 / static_cast<double>(items), 1.0 - theta)) / (1.0 - zeta2 / zetan_);
            half_pow_ = 1.0 + std::pow(0.5, theta);
        }
        std::uint64_t next(double u) const
        {
            const double uz = u * zetan_;
            if (uz < 1.0)
                return 0;
            if (uz < half_pow_)
                return 1;
            const auto rank = static_cast<std::uint64_t>(static_cast<double>(items_) * std::pow(eta_ * u - eta_ + 1.0, alpha_));
            return std::min(rank, items_ - 1);
        }

    private:
        std::uint64_t items_;
        double zetan_ = 0, alpha_ = 0, eta_ = 0, half_pow_ = 0;
    };

    struct alignas(64) ThreadState
    {
        std::atomic<std::uint64_t> ops{0};
        std::uint64_t window_reads = 0;
        std::uint64_t window_writes = 0;
        Histogram read_latency;
        Histogram write_latency;
        // Open loop: how far behind its schedule the client was at the end, in nanoseconds.
        std::int64_t schedule_lag_ns = 0;
        std::string error;
        // Where sampled foreground writes spend their time (RocksDB PerfContext), nanoseconds.
        std::uint64_t sampled_writes = 0;
        std::array<std::uint64_t, 9> write_breakdown{};
    };

    // PerfContext fields reported for sampled foreground writes (see rocksdb/perf_context.h). The
    // timers overlap (write_pre_and_post_process encloses the others); "total" is the wall time
    // of the whole Write call.
    constexpr std::array<const char *, 9> kWriteBreakdown{
        "write_thread_wait", "write_wal", "write_memtable", "write_delay", "write_pre_and_post_process",
        "write_scheduling_flushes_compactions", "db_mutex_lock", "db_condition_wait", "total"};

    Json preload(const Config &config)
    {
        if (config.records % config.threads != 0)
            throw std::invalid_argument("--records must be a multiple of --threads");
        fs::remove_all(config.dir);
        fs::create_directories(config.dir.parent_path());
        auto options = make_options(config, nullptr);
        options.create_if_missing = true;
        options.error_if_exists = true;
        options.max_background_jobs = std::max<int>(options.max_background_jobs, 16);
        options.max_subcompactions = std::max<std::uint32_t>(options.max_subcompactions, 8);
        std::unique_ptr<rocksdb::DB> db;
        check(rocksdb::DB::Open(options, config.dir.string(), &db), "open");
        const auto started = Clock::now();
        const unsigned producers = std::min<unsigned>(16, std::max(1U, std::thread::hardware_concurrency()));
        std::vector<std::thread> workers;
        std::atomic<bool> failed{false};
        std::string failure;
        std::mutex failure_mutex;
        // Random insertion order so the tree has a natural multi-level shape.
        for (unsigned producer = 0; producer < producers; ++producer)
        {
            workers.emplace_back([&, producer]
                                 {
                try
                {
                    rocksdb::WriteOptions write;
                    write.disableWAL = true;
                    rocksdb::WriteBatch batch;
                    std::string key, value;
                    const auto stride = config.records / producers + 1;
                    // A bijective permutation of [0, records) via a multiplicative step coprime with records.
                    std::uint64_t step = 0x9e3779b97f4a7c15ULL % config.records;
                    while (std::gcd(step, config.records) != 1)
                        ++step;
                    const auto begin = producer * stride;
                    const auto end = std::min<std::uint64_t>(config.records, begin + stride);
                    for (std::uint64_t position = begin; position < end && !failed; ++position)
                    {
                        // records < 2^32, so position * step fits in 64 bits.
                        const auto index = position * step % config.records;
                        if (config.workload == "timeseries")
                            make_series_key(index / config.retention, index % config.retention, key);
                        else
                            make_key(config, index, key);
                        make_value(config, index, 0, value);
                        check(batch.Put(key, value), "batch put");
                        if (batch.GetDataSize() >= (1U << 20))
                        {
                            check(db->Write(write, &batch), "write");
                            batch.Clear();
                        }
                    }
                    check(db->Write(write, &batch), "write");
                }
                catch (const std::exception &error)
                {
                    const std::lock_guard guard(failure_mutex);
                    failure = error.what();
                    failed = true;
                } });
        }
        for (auto &worker : workers)
            worker.join();
        if (failed)
            throw std::runtime_error(failure);
        check(db->Flush(rocksdb::FlushOptions()), "flush");
        wait_for_compactions(db.get());
        const auto loaded = Clock::now();
        const std::vector<std::uint32_t> versions(config.records, 0);
        const auto verification = verify_all(db.get(), config, versions);
        Json result{{"phase", "preload"},
                    {"records", config.records},
                    {"load_seconds", std::chrono::duration<double>(loaded - started).count()},
                    {"verification", verification},
                    {"levels", level_shape(db.get())}};
        check(db->Close(), "close");
        return result;
    }

    Json run(const Config &config)
    {
        if (config.records % config.threads != 0)
            throw std::invalid_argument("--records must be a multiple of --threads");
        auto backend = make_backend(config.mode);
        auto options = make_options(config, backend);
        auto totals = std::make_shared<CompactionTotals>();
        options.listeners.push_back(totals);
        std::shared_ptr<lsm_lab::ReplicaSet> replicas;
        std::unique_ptr<rocksdb::Env> replicating_env;
        double bootstrap_seconds = 0;
        const bool logical = config.replication == "logical";
        if (config.replication != "none")
        {
            std::vector<std::string> endpoints;
            std::size_t start = 0;
            while (start < config.replicate_to.size())
            {
                const auto end = std::min(config.replicate_to.find(',', start), config.replicate_to.size());
                endpoints.push_back(config.replicate_to.substr(start, end - start));
                start = end + 1;
            }
            if (endpoints.empty())
                throw std::invalid_argument("--replication needs --replicate-to");
            replicas = std::make_shared<lsm_lab::ReplicaSet>(endpoints);
            // Backups start from a copy of the primary's files (both modes), sent before the run.
            const auto bootstrap_started = Clock::now();
            check(lsm_lab::bootstrap_replicas(config.dir.string(), *replicas), "bootstrap replicas");
            bootstrap_seconds = std::chrono::duration<double>(Clock::now() - bootstrap_started).count();
            if (config.replication == "physical")
            {
                // RocksDB names files from the path exactly as given, so match that (not a canonical path).
                auto mirrored = lsm_lab::make_replicating_file_system(rocksdb::FileSystem::Default(),
                                                                       config.dir.lexically_normal().string(), replicas);
                replicating_env = rocksdb::NewCompositeEnv(mirrored);
                options.env = replicating_env.get();
            }
            else if (logical)
            {
                const auto request = Json{{"shape", db_shape(config)},
                                          {"backend", config.replica_backend},
                                          {"disable_wal", config.disable_wal}}
                                         .dump();
                check(replicas->broadcast(lsm_lab::ReplicaOp::open_database, "", 0, request.data(), request.size()),
                      "open replicas");
            }
            else
            {
                throw std::invalid_argument("--replication must be none, physical or logical");
            }
        }
        std::vector<std::string> replica_statistics_start;
        if (replicas)
            check(replicas->broadcast(lsm_lab::ReplicaOp::statistics, "", 0, nullptr, 0, &replica_statistics_start),
                  "replica statistics");
        // Logical replication: every write reaches the backups before it is applied here.
        const auto replicate = [&](const rocksdb::WriteBatch &batch) {
            if (logical)
                check(replicas->broadcast(lsm_lab::ReplicaOp::write_batch, "", 0, batch.Data().data(),
                                          batch.Data().size()),
                      "replicate");
        };
        std::unique_ptr<rocksdb::DB> db;
        check(rocksdb::DB::Open(options, config.dir.string(), &db), "open");
        const auto statistics = options.statistics;
        const auto levels_before = level_shape(db.get());

        std::vector<std::uint32_t> versions(config.records, 0);
        std::vector<ThreadState> states(config.threads);
        std::atomic<int> phase{0}; // 0 warmup, 1 measurement, 2 stop
        const auto per_thread = config.records / config.threads;
        const bool timeseries = config.workload == "timeseries";
        // Time series: thread t owns series t, t + threads, ...; heads[s] is the next point of series s.
        const auto series_count = timeseries ? config.records / config.retention : 0;
        std::vector<std::uint64_t> heads(series_count, config.retention);
        // Keys (or series) each thread chooses among.
        const auto items = timeseries ? series_count / config.threads : per_thread;

        std::unique_ptr<Zipfian> zipf;
        if (config.distribution == "zipfian")
            zipf = std::make_unique<Zipfian>(items, config.zipf_theta);
        else if (config.distribution != "uniform")
            throw std::invalid_argument("--distribution must be uniform or zipfian");

        // Common start of every client's open-loop schedule.
        const auto schedule_start = Clock::now() + std::chrono::milliseconds(10);
        auto worker = [&](std::uint32_t thread)
        {
            auto &state = states[thread];
#if defined(__linux__)
            pthread_setname_np(pthread_self(), "lsm-foreground");
#endif
            try
            {
                std::mt19937_64 random(mix64(config.seed * 7919 + thread));
                std::uniform_int_distribution<std::uint64_t> uniform(0, items - 1);
                std::uniform_real_distribution<double> unit(0.0, 1.0);
                // Scrambled Zipfian over this thread's keys (YCSB): rank r maps to a hashed slot.
                const auto pick = [&](auto &generator) -> std::uint64_t {
                    if (!zipf)
                        return uniform(generator);
                    const auto rank = zipf->next(unit(generator));
                    return mix64(rank ^ 0x5bd1e9955bd1e995ULL) % items;
                };
                std::uniform_int_distribution<std::uint32_t> percent(0, 99);
                rocksdb::WriteOptions write;
                write.disableWAL = config.disable_wal;
                rocksdb::ReadOptions read;
                std::string key, value, expected, expected_key;
                rocksdb::PinnableSlice found;
                rocksdb::WriteBatch batch;
                rocksdb::WriteBatch single;
                // Every 64th write in the measurement window is timed by RocksDB's PerfContext.
                std::uint64_t write_counter = 0;
                Clock::time_point sample_start;
                const auto sample_begin = [&](bool measuring) {
                    const bool sample = measuring && (++write_counter % 64) == 0;
                    if (sample)
                    {
                        sample_start = Clock::now();
                        rocksdb::SetPerfLevel(rocksdb::PerfLevel::kEnableTimeExceptForMutex);
                        rocksdb::get_perf_context()->Reset();
                    }
                    return sample;
                };
                const auto sample_end = [&](bool sampled) {
                    if (!sampled)
                        return;
                    const auto *perf = rocksdb::get_perf_context();
                    const std::uint64_t values[] = {perf->write_thread_wait_nanos, perf->write_wal_time,
                                                    perf->write_memtable_time, perf->write_delay_time,
                                                    perf->write_pre_and_post_process_time,
                                                    perf->write_scheduling_flushes_compactions_time,
                                                    perf->db_mutex_lock_nanos, perf->db_condition_wait_nanos,
                                                    static_cast<std::uint64_t>((Clock::now() - sample_start).count())};
                    for (std::size_t index = 0; index < kWriteBreakdown.size(); ++index)
                        state.write_breakdown[index] += values[index];
                    ++state.sampled_writes;
                    rocksdb::SetPerfLevel(rocksdb::PerfLevel::kDisable);
                };
                const auto put = [&](const std::string &k, const std::string &v) {
                    if (!logical)
                        return check(db->Put(write, k, v), "put");
                    single.Clear();
                    check(single.Put(k, v), "batch put");
                    replicate(single);
                    check(db->Write(write, &single), "put");
                };
                const auto remove = [&](const std::string &k) {
                    if (!logical)
                        return check(db->Delete(write, k), "delete");
                    single.Clear();
                    check(single.Delete(k), "batch delete");
                    replicate(single);
                    check(db->Write(write, &single), "delete");
                };
                // Open loop: this client's operations are due every per_op ns from a common start.
                const auto per_op = config.rate > 0 ? std::chrono::nanoseconds(static_cast<std::int64_t>(
                                                          1e9 * config.threads / config.rate))
                                                    : std::chrono::nanoseconds(0);
                std::uint64_t issued = 0;
                Clock::time_point scheduled = schedule_start;
                const auto due = [&](std::uint64_t operations) {
                    if (config.rate <= 0)
                        return;
                    scheduled = schedule_start + per_op * static_cast<std::int64_t>(issued);
                    issued += operations;
                    if (scheduled > Clock::now())
                        std::this_thread::sleep_until(scheduled);
                };
                // Start of an operation for its latency: the scheduled time in open loop.
                const auto start_of = [&] { return config.rate > 0 ? scheduled : Clock::now(); };
                // Timed reads: point reads and scans share the read counters and latency.
                const auto timed_read = [&](bool measuring, auto &&body) {
                    const auto begin = start_of();
                    body();
                    const auto ns = static_cast<std::uint64_t>((Clock::now() - begin).count());
                    if (measuring)
                    {
                        state.read_latency.add(ns);
                        ++state.window_reads;
                    }
                    state.ops.fetch_add(1, std::memory_order_relaxed);
                };
                const auto expect_point = [&](const rocksdb::Slice &value, std::uint64_t series, std::uint64_t time) {
                    const auto slot = series * config.retention + time % config.retention;
                    make_value(config, slot, time / config.retention, expected);
                    if (value != rocksdb::Slice(expected))
                        throw std::runtime_error("online read returned a wrong time-series point " + std::to_string(series) + "/" +
                                                 std::to_string(time));
                };
                while (true)
                {
                    const auto current = phase.load(std::memory_order_relaxed);
                    if (current == 2)
                    {
                        if (config.rate > 0)
                            state.schedule_lag_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                        Clock::now() - (schedule_start + per_op * static_cast<std::int64_t>(issued)))
                                                        .count();
                        break;
                    }
                    due(config.batch_size);
                    if (timeseries)
                    {
                        const auto series = thread + config.threads * pick(random);
                        const auto roll = percent(random);
                        const auto head = heads[series];
                        if (roll < config.read_percent)
                        {
                            // One live point of the series, anywhere in the retention window.
                            const auto time = head - 1 - random() % config.retention;
                            timed_read(current == 1, [&] {
                                make_series_key(series, time, key);
                                found.Reset();
                                check(db->Get(read, db->DefaultColumnFamily(), key, &found), "get");
                                expect_point(found, series, time);
                            });
                            continue;
                        }
                        if (roll < config.read_percent + config.scan_percent)
                        {
                            // The latest scan_length points of the series, in time order.
                            const auto length = std::min<std::uint64_t>(config.scan_length, config.retention);
                            timed_read(current == 1, [&] {
                                std::unique_ptr<rocksdb::Iterator> iterator(db->NewIterator(read));
                                make_series_key(series, head - length, key);
                                iterator->Seek(key);
                                for (std::uint64_t time = head - length; time < head; ++time, iterator->Next())
                                {
                                    make_series_key(series, time, expected_key);
                                    if (!iterator->Valid() || iterator->key() != rocksdb::Slice(expected_key))
                                        throw std::runtime_error("time-series scan missed point " + std::to_string(series) + "/" +
                                                                 std::to_string(time));
                                    expect_point(iterator->value(), series, time);
                                }
                                check(iterator->status(), "scan");
                            });
                            continue;
                        }
                        // Appends: each writes the next point of a series and deletes the point that
                        // leaves the retention window, batch_size appends per WriteBatch.
                        batch.Clear();
                        for (std::uint32_t item = 0; item < config.batch_size; ++item)
                        {
                            const auto target = item == 0 ? series : thread + config.threads * pick(random);
                            const auto time = heads[target];
                            const auto slot = target * config.retention + time % config.retention;
                            make_series_key(target, time - config.retention, key);
                            check(batch.Delete(key), "batch delete");
                            versions[slot] = static_cast<std::uint32_t>(time / config.retention);
                            make_value(config, slot, versions[slot], value);
                            make_series_key(target, time, key);
                            check(batch.Put(key, value), "batch put");
                            heads[target] = time + 1;
                        }
                        const bool sampled = sample_begin(current == 1);
                        const auto begin = start_of();
                        replicate(batch);
                        check(db->Write(write, &batch), "write batch");
                        const auto ns = static_cast<std::uint64_t>((Clock::now() - begin).count());
                        sample_end(sampled);
                        if (current == 1)
                        {
                            state.write_latency.add(ns);
                            state.window_writes += config.batch_size;
                        }
                        state.ops.fetch_add(config.batch_size, std::memory_order_relaxed);
                        continue;
                    }
                    const auto index = thread + config.threads * pick(random);
                    const auto roll = percent(random);
                    const bool is_read = roll < config.read_percent;
                    const bool is_rmw = !is_read && roll < config.read_percent + config.rmw_percent;
                    const bool is_delete = !is_read && !is_rmw &&
                                           roll < config.read_percent + config.rmw_percent + config.delete_percent;
                    const bool is_scan = !is_read && !is_rmw && !is_delete &&
                                         roll < config.read_percent + config.rmw_percent + config.delete_percent + config.scan_percent;
                    make_key(config, index, key);
                    // Reads (and the read half of read-modify-writes) are checked against the
                    // version this thread last wrote; a deleted key must be absent.
                    const auto checked_get = [&]() {
                        found.Reset();
                        const auto status = db->Get(read, db->DefaultColumnFamily(), key, &found);
                        if (versions[index] & kDeleted)
                        {
                            if (!status.IsNotFound())
                                throw std::runtime_error("online read found deleted key " + std::to_string(index));
                            return;
                        }
                        check(status, "get");
                        make_value(config, index, versions[index], expected);
                        if (found != rocksdb::Slice(expected))
                            throw std::runtime_error("online read returned a wrong value for key " + std::to_string(index));
                    };
                    if (is_scan)
                    {
                        // scan_length keys from this key on; keys this thread owns are checked (the
                        // others change concurrently).
                        timed_read(current == 1, [&] {
                            std::unique_ptr<rocksdb::Iterator> iterator(db->NewIterator(read));
                            iterator->Seek(key);
                            for (std::uint32_t item = 0; item < config.scan_length && iterator->Valid(); ++item, iterator->Next())
                            {
                                const auto seen = iterator->key();
                                if (seen.size() < 16)
                                    throw std::runtime_error("scan found an unexpected key");
                                const auto owner = key_index(seen);
                                if (owner >= config.records || seen.size() != key_size(config, owner))
                                    throw std::runtime_error("scan found an unexpected key");
                                if (owner % config.threads != thread)
                                    continue;
                                if (versions[owner] & kDeleted)
                                    throw std::runtime_error("scan found deleted key " + std::to_string(owner));
                                make_value(config, owner, versions[owner], expected);
                                if (iterator->value() != rocksdb::Slice(expected))
                                    throw std::runtime_error("scan returned a wrong value for key " + std::to_string(owner));
                            }
                            check(iterator->status(), "scan");
                        });
                        continue;
                    }
                    if (is_read)
                    {
                        const auto begin = start_of();
                        checked_get();
                        const auto ns = static_cast<std::uint64_t>((Clock::now() - begin).count());
                        if (current == 1)
                        {
                            state.read_latency.add(ns);
                            ++state.window_reads;
                        }
                    }
                    else if (is_rmw || is_delete)
                    {
                        const auto version = (versions[index] & ~kDeleted) + 1;
                        const auto begin = start_of();
                        if (is_rmw)
                        {
                            checked_get();
                            make_value(config, index, version, value);
                            put(key, value);
                            versions[index] = version;
                        }
                        else
                        {
                            remove(key);
                            versions[index] = version | kDeleted;
                        }
                        const auto ns = static_cast<std::uint64_t>((Clock::now() - begin).count());
                        if (current == 1)
                        {
                            (is_rmw ? state.read_latency : state.write_latency).add(ns);
                            ++(is_rmw ? state.window_reads : state.window_writes);
                        }
                    }
                    else if (config.batch_size == 1)
                    {
                        const auto version = (versions[index] & ~kDeleted) + 1;
                        make_value(config, index, version, value);
                        const bool sampled = sample_begin(current == 1);
                        const auto begin = start_of();
                        put(key, value);
                        const auto ns = static_cast<std::uint64_t>((Clock::now() - begin).count());
                        sample_end(sampled);
                        versions[index] = version;
                        if (current == 1)
                        {
                            state.write_latency.add(ns);
                            ++state.window_writes;
                        }
                    }
                    else
                    {
                        // One WriteBatch of batch_size puts to keys owned by this thread; a key repeated
                        // within a batch ends at its last (highest) version, as in RocksDB.
                        batch.Clear();
                        for (std::uint32_t item = 0; item < config.batch_size; ++item)
                        {
                            const auto owned = item == 0 ? index : thread + config.threads * pick(random);
                            make_key(config, owned, key);
                            versions[owned] = (versions[owned] & ~kDeleted) + 1;
                            make_value(config, owned, versions[owned], value);
                            check(batch.Put(key, value), "batch put");
                        }
                        const bool sampled = sample_begin(current == 1);
                        const auto begin = start_of();
                        replicate(batch);
                        check(db->Write(write, &batch), "write batch");
                        const auto ns = static_cast<std::uint64_t>((Clock::now() - begin).count());
                        sample_end(sampled);
                        if (current == 1)
                        {
                            state.write_latency.add(ns);
                            state.window_writes += config.batch_size;
                        }
                        state.ops.fetch_add(config.batch_size, std::memory_order_relaxed);
                        continue;
                    }
                    state.ops.fetch_add(1, std::memory_order_relaxed);
                }
            }
            catch (const std::exception &error)
            {
                state.error = error.what();
                phase.store(2);
            }
        };

        const auto ticker = [&](rocksdb::Tickers name) { return statistics->getTickerCount(name); };
        const auto counters = [&]
        {
            return Json{{"user_bytes_written", ticker(rocksdb::BYTES_WRITTEN)},
                        {"flush_write_bytes", ticker(rocksdb::FLUSH_WRITE_BYTES)},
                        {"compact_read_bytes", ticker(rocksdb::COMPACT_READ_BYTES)},
                        {"compact_write_bytes", ticker(rocksdb::COMPACT_WRITE_BYTES)},
                        {"compaction_cpu_micros", ticker(rocksdb::COMPACTION_CPU_TOTAL_TIME)},
                        {"stall_micros", ticker(rocksdb::STALL_MICROS)},
                        {"wal_bytes", ticker(rocksdb::WAL_FILE_BYTES)},
                        {"wal_syncs", ticker(rocksdb::WAL_FILE_SYNCED)},
                        {"writes_done_by_self", ticker(rocksdb::WRITE_DONE_BY_SELF)},
                        {"writes_done_by_other", ticker(rocksdb::WRITE_DONE_BY_OTHER)},
                        {"compactions", totals->snapshot()}};
        };
        const auto total_ops = [&]
        {
            std::uint64_t sum = 0;
            for (const auto &state : states)
                sum += state.ops.load(std::memory_order_relaxed);
            return sum;
        };

        const lsm_lab::ProcessAccounting accounting(config.dir.string());
        auto previous_sample = accounting.sample();
        const auto run_sample_start = previous_sample;
        lsm_lab::ProcessSample window_sample_start, window_sample_end;
        const auto run_cpu_start = process_cpu();
        const double run_start_unix = unix_seconds();
        const auto run_counters_start = counters();
        const auto started = Clock::now();
        std::vector<std::thread> workers;
        for (std::uint32_t thread = 0; thread < config.threads; ++thread)
            workers.emplace_back(worker, thread);

        Json timeline = Json::array();
        std::uint64_t previous_ops = 0;
        std::uint64_t previous_stall = 0;
        CpuTimes cpu_start{}, cpu_end{};
        Json counters_start, counters_end;
        double window_start_unix = 0, window_end_unix = 0;
        Clock::time_point window_start{}, window_end{};
        const auto total_seconds = config.warmup_seconds + config.duration_seconds;
        const auto open_window = [&]
        {
            cpu_start = process_cpu();
            counters_start = counters();
            window_sample_start = accounting.sample();
            window_start = Clock::now();
            window_start_unix = unix_seconds();
            phase.store(1);
        };
        if (config.warmup_seconds == 0)
            open_window();
        for (std::uint32_t second = 1; second <= total_seconds; ++second)
        {
            const auto target = started + std::chrono::seconds(second);
            while (Clock::now() < target && phase.load() != 2)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (phase.load() == 2)
                break;
            const auto ops = total_ops();
            const auto stall = ticker(rocksdb::STALL_MICROS);
            const auto current_sample = accounting.sample();
            const auto usage = lsm_lab::ProcessAccounting::delta(previous_sample, current_sample);
            previous_sample = current_sample;
            timeline.push_back({{"second", second},
                                {"phase", second <= config.warmup_seconds ? "warmup" : "measurement"},
                                {"ops", ops - previous_ops},
                                {"stall_micros", stall - previous_stall},
                                {"cores_by_role", usage.at("cores_by_role")},
                                {"process_write_mib_s", usage.at("process_write_mib_s")},
                                {"syscall_write_mib_s", usage.at("syscall_write_mib_s")},
                                {"device", usage.value("device", Json(nullptr))},
                                {"immutable_memtables", property(db.get(), "rocksdb.num-immutable-mem-table")},
                                {"running_flushes", property(db.get(), "rocksdb.num-running-flushes")},
                                {"l0_files", string_property(db.get(), "rocksdb.num-files-at-level0")},
                                // Non-zero while RocksDB throttles writes (bytes/s); 1 while writes are stopped.
                                {"delayed_write_rate", property(db.get(), "rocksdb.actual-delayed-write-rate")},
                                {"write_stopped", property(db.get(), "rocksdb.is-write-stopped")},
                                {"running_compactions", property(db.get(), "rocksdb.num-running-compactions")},
                                {"pending_compaction_bytes", property(db.get(), "rocksdb.estimate-pending-compaction-bytes")}});
            previous_ops = ops;
            previous_stall = stall;
            if (second == config.warmup_seconds)
                open_window();
        }
        if (phase.load() == 1)
        {
            window_end = Clock::now();
            window_end_unix = unix_seconds();
            cpu_end = process_cpu();
            counters_end = counters();
            window_sample_end = accounting.sample();
        }
        phase.store(2);
        for (auto &thread : workers)
            thread.join();
        const auto workers_end = Clock::now();
        const auto all_ops = total_ops();
        for (const auto &state : states)
        {
            if (!state.error.empty())
                throw std::runtime_error("worker failed: " + state.error);
        }
        // Finish all compaction the run created, so a backend cannot look faster by
        // deferring compaction past the end of the measurement.
        const auto drain_started = Clock::now();
        const auto drain_cpu_start = process_cpu();
        wait_for_compactions(db.get());
        const auto drain_end = Clock::now();
        const double drain_end_unix = unix_seconds();
        const auto drain_cpu_end = process_cpu();
        const auto run_counters_end = counters();
        if (window_end == Clock::time_point{})
            throw std::runtime_error("measurement window did not complete");

        Histogram reads, writes;
        std::uint64_t window_reads = 0, window_writes = 0;
        for (const auto &state : states)
        {
            reads.merge(state.read_latency);
            writes.merge(state.write_latency);
            window_reads += state.window_reads;
            window_writes += state.window_writes;
        }
        const auto window_seconds = std::chrono::duration<double>(window_end - window_start).count();
        Json delta;
        for (const auto &[name, value] : counters_end.items())
        {
            if (value.is_number())
                delta[name] = value.get<std::uint64_t>() - counters_start.at(name).get<std::uint64_t>();
        }
        Json compaction_delta;
        for (const auto &[name, value] : counters_end.at("compactions").items())
            compaction_delta[name] = value.get<std::uint64_t>() - counters_start.at("compactions").at(name).get<std::uint64_t>();
        delta["compactions"] = compaction_delta;
        const double cpu_seconds = (cpu_end.user - cpu_start.user) + (cpu_end.system - cpu_start.system);
        const double ops = static_cast<double>(window_reads + window_writes);
        const double user_bytes = delta.at("user_bytes_written").get<double>();

        Json result{{"phase", "run"},
                    {"mode", config.mode},
                    {"threads", config.threads},
                    {"read_percent", config.read_percent},
                    {"window", {{"seconds", window_seconds}, {"start_unix", window_start_unix}, {"end_unix", window_end_unix}}},
                    {"throughput", {{"ops_per_second", ops / window_seconds},
                                    {"writes_per_second", window_writes / window_seconds},
                                    {"reads_per_second", window_reads / window_seconds},
                                    {"user_mib_per_second", user_bytes / 1048576.0 / window_seconds}}},
                    {"write_latency", writes.json()},
                    {"open_loop", config.rate > 0 ? Json{{"rate", config.rate}, {"max_schedule_lag_ms", [&] {
                                                             std::int64_t lag = 0;
                                                             for (const auto &state : states)
                                                                 lag = std::max(lag, state.schedule_lag_ns);
                                                             return lag / 1e6;
                                                         }()}}
                                                  : Json(nullptr)},
                    {"read_latency", reads.json()},
                    {"process_cpu", {{"user_seconds", cpu_end.user - cpu_start.user},
                                     {"system_seconds", cpu_end.system - cpu_start.system},
                                     {"seconds", cpu_seconds},
                                     {"cores_used", cpu_seconds / window_seconds},
                                     {"cpu_microseconds_per_op", ops > 0 ? cpu_seconds * 1e6 / ops : 0.0}}},
                    {"window_counters", delta},
                    {"write_amplification", user_bytes > 0 ? (delta.at("flush_write_bytes").get<double>() + delta.at("compact_write_bytes").get<double>()) / user_bytes : 0.0},
                    {"stall_fraction", delta.at("stall_micros").get<double>() / 1e6 / window_seconds / config.threads},
                    {"timeline", timeline},
                    {"ingest_and_compact", [&] {
                         const double load_seconds = std::chrono::duration<double>(workers_end - started).count();
                         const double drain_seconds = std::chrono::duration<double>(drain_end - drain_started).count();
                         const double total_cpu = (drain_cpu_end.user - run_cpu_start.user) + (drain_cpu_end.system - run_cpu_start.system);
                         const double drain_cpu = (drain_cpu_end.user - drain_cpu_start.user) + (drain_cpu_end.system - drain_cpu_start.system);
                         const double written = run_counters_end.at("user_bytes_written").get<double>() - run_counters_start.at("user_bytes_written").get<double>();
                         const double rewritten = run_counters_end.at("flush_write_bytes").get<double>() - run_counters_start.at("flush_write_bytes").get<double>() +
                                                  run_counters_end.at("compact_write_bytes").get<double>() - run_counters_start.at("compact_write_bytes").get<double>();
                         const double total = load_seconds + drain_seconds;
                         return Json{{"ops", all_ops}, {"start_unix", run_start_unix}, {"end_unix", drain_end_unix},
                                     {"load_seconds", load_seconds}, {"drain_seconds", drain_seconds},
                                     {"seconds", total}, {"ops_per_second", all_ops / total},
                                     {"cpu_seconds", total_cpu}, {"drain_cpu_seconds", drain_cpu},
                                     {"cpu_microseconds_per_op", all_ops > 0 ? total_cpu * 1e6 / all_ops : 0.0},
                                     {"compaction_cpu_seconds", (run_counters_end.at("compactions").at("cpu_micros").get<double>() -
                                                                 run_counters_start.at("compactions").at("cpu_micros").get<double>()) / 1e6},
                                     {"write_amplification", written > 0 ? rewritten / written : 0.0}};
                     }()},
                    {"accounting", [&] {
                         // Where CPU went and what reached storage, for the window and for the whole run.
                         Json breakdown = Json::object();
                         std::uint64_t sampled = 0;
                         for (const auto &state : states)
                             sampled += state.sampled_writes;
                         for (std::size_t index = 0; index < kWriteBreakdown.size(); ++index)
                         {
                             std::uint64_t sum = 0;
                             for (const auto &state : states)
                                 sum += state.write_breakdown[index];
                             breakdown[kWriteBreakdown[index]] = sampled ? sum / 1000.0 / sampled : 0.0;
                         }
                         std::string cf_stalls, db_stalls;
                         db->GetProperty(rocksdb::DB::Properties::kCFWriteStallStats, &cf_stalls);
                         db->GetProperty(rocksdb::DB::Properties::kDBWriteStallStats, &db_stalls);
                         return Json{{"window", lsm_lab::ProcessAccounting::delta(window_sample_start, window_sample_end)},
                                     {"run", lsm_lab::ProcessAccounting::delta(run_sample_start, accounting.sample())},
                                     {"device", accounting.device()},
                                     {"sampled_writes", sampled},
                                     {"write_breakdown_us_per_write", breakdown},
                                     {"cf_write_stall_stats", cf_stalls},
                                     {"db_write_stall_stats", db_stalls}};
                     }()},
                    {"sst_bytes_after_run", property(db.get(), "rocksdb.total-sst-files-size")},
                    {"live_sst_bytes_after_run", property(db.get(), "rocksdb.live-sst-files-size")},
                    {"levels_before", levels_before},
                    {"levels_after_run", level_shape(db.get())},
                    {"transform", backend ? backend->statistics() : Json(nullptr)}};
        if (config.verify)
        {
            result["verification"] = verify_all(db.get(), config, versions);
            result["levels_after_verification"] = level_shape(db.get());
            result["sst_bytes_after_compaction"] = property(db.get(), "rocksdb.total-sst-files-size");
            result["transform_after_verification"] = backend ? backend->statistics() : Json(nullptr);
        }
        if (!replicas)
        {
            check(db->Close(), "close");
            return result;
        }
        // Every backup must hold exactly the primary's data: compare digests of all keys and values.
        const auto primary_digest = lsm_lab::database_digest(db.get());
        check(db->Close(), "close"); // physical: ships the final file contents
        db.reset();
        const auto wait_started = Clock::now();
        check(replicas->broadcast(lsm_lab::ReplicaOp::wait, "", 0, nullptr, 0), "replica wait");
        const auto replica_drain_seconds = std::chrono::duration<double>(Clock::now() - wait_started).count();
        std::vector<std::string> replica_statistics_end, digests;
        check(replicas->broadcast(lsm_lab::ReplicaOp::statistics, "", 0, nullptr, 0, &replica_statistics_end),
              "replica statistics");
        check(replicas->broadcast(lsm_lab::ReplicaOp::digest, "", 0, nullptr, 0, &digests), "replica digest");
        Json backups = Json::array();
        bool match = true;
        for (std::size_t index = 0; index < digests.size(); ++index)
        {
            const auto before = Json::parse(replica_statistics_start.at(index));
            const auto after = Json::parse(replica_statistics_end.at(index));
            match = match && digests[index] == primary_digest;
            backups.push_back({{"endpoint", replicas->endpoints()[index]},
                               {"digest", digests[index]},
                               {"cpu_seconds", after.at("cpu_seconds").get<double>() - before.at("cpu_seconds").get<double>()},
                               {"bytes", after.at("bytes").get<std::uint64_t>() - before.at("bytes").get<std::uint64_t>()},
                               {"statistics", after}});
        }
        result["replication"] = {{"mode", config.replication},
                                 {"replica_backend", logical ? config.replica_backend : "none"},
                                 {"primary_digest", primary_digest},
                                 {"match", match},
                                 {"replica_drain_seconds", replica_drain_seconds},
                                 {"bootstrap_seconds", bootstrap_seconds},
                                 {"transport", replicas->statistics()},
                                 {"backups", backups}};
        if (!match)
            throw std::runtime_error("replication: a backup's digest differs from the primary's: " +
                                     result["replication"].dump());
        return result;
    }

    Config parse(int argc, char **argv)
    {
        Config config;
        for (int index = 1; index < argc; ++index)
        {
            const std::string name = argv[index];
            const auto value = [&]() -> std::string
            {
                if (index + 1 >= argc)
                    throw std::invalid_argument("missing value for " + name);
                return argv[++index];
            };
            const auto number = [&]() { return std::stoull(value()); };
            if (name == "--phase")
                config.phase = value();
            else if (name == "--dir")
                config.dir = value();
            else if (name == "--mode")
                config.mode = value();
            else if (name == "--records")
                config.records = number();
            else if (name == "--key-bytes")
                config.key_bytes = static_cast<std::uint32_t>(number());
            else if (name == "--value-bytes")
                config.value_bytes = static_cast<std::uint32_t>(number());
            else if (name == "--compression")
                config.compression = value();
            else if (name == "--block-bytes")
                config.block_bytes = number();
            else if (name == "--write-buffer-bytes")
                config.write_buffer_bytes = number();
            else if (name == "--target-file-bytes")
                config.target_file_bytes = number();
            else if (name == "--max-bytes-for-level-base")
                config.max_bytes_for_level_base = number();
            else if (name == "--max-write-buffer-number")
                config.max_write_buffer_number = static_cast<std::uint32_t>(number());
            else if (name == "--background-jobs")
                config.background_jobs = static_cast<std::uint32_t>(number());
            else if (name == "--subcompactions")
                config.subcompactions = static_cast<std::uint32_t>(number());
            else if (name == "--levels")
                config.levels = static_cast<std::uint32_t>(number());
            else if (name == "--block-cache-bytes")
                config.block_cache_bytes = number();
            else if (name == "--threads")
                config.threads = static_cast<std::uint32_t>(number());
            else if (name == "--read-percent")
                config.read_percent = static_cast<std::uint32_t>(number());
            else if (name == "--warmup-seconds")
                config.warmup_seconds = static_cast<std::uint32_t>(number());
            else if (name == "--duration-seconds")
                config.duration_seconds = static_cast<std::uint32_t>(number());
            else if (name == "--disable-wal")
                config.disable_wal = value() == "true";
            else if (name == "--rmw-percent")
                config.rmw_percent = static_cast<std::uint32_t>(number());
            else if (name == "--delete-percent")
                config.delete_percent = static_cast<std::uint32_t>(number());
            else if (name == "--distribution")
                config.distribution = value();
            else if (name == "--zipf-theta")
                config.zipf_theta = std::stod(value());
            else if (name == "--l0-trigger")
                config.l0_trigger = static_cast<std::uint32_t>(number());
            else if (name == "--level-multiplier")
                config.level_multiplier = std::stod(value());
            else if (name == "--blob-min-bytes")
                config.blob_min_bytes = static_cast<std::uint32_t>(number());
            else if (name == "--rate")
                config.rate = std::stod(value());
            else if (name == "--workload")
                config.workload = value();
            else if (name == "--key-bytes-max")
                config.key_bytes_max = static_cast<std::uint32_t>(number());
            else if (name == "--value-bytes-max")
                config.value_bytes_max = static_cast<std::uint32_t>(number());
            else if (name == "--key-families")
                config.key_families = static_cast<std::uint32_t>(number());
            else if (name == "--scan-percent")
                config.scan_percent = static_cast<std::uint32_t>(number());
            else if (name == "--scan-length")
                config.scan_length = static_cast<std::uint32_t>(number());
            else if (name == "--retention")
                config.retention = number();
            else if (name == "--pipelined-write")
                config.pipelined_write = value() == "true";
            else if (name == "--concurrent-memtable")
                config.concurrent_memtable = value() == "true";
            else if (name == "--flush-jobs")
                config.flush_jobs = static_cast<std::uint32_t>(number());
            else if (name == "--allow-fallocate")
                config.allow_fallocate = value() == "true";
            else if (name == "--bottommost-compression")
                config.bottommost_compression = value();
            else if (name == "--replication")
                config.replication = value();
            else if (name == "--replicate-to")
                config.replicate_to = value();
            else if (name == "--replica-backend")
                config.replica_backend = value();
            else if (name == "--use-direct-io")
                config.use_direct_io = value() == "true";
            else if (name == "--batch-size")
                config.batch_size = static_cast<std::uint32_t>(number());
            else if (name == "--adaptive-yield")
                config.adaptive_yield = value() == "true";
            else if (name == "--verify")
                config.verify = value() == "true";
            else if (name == "--seed")
                config.seed = number();
            else
                throw std::invalid_argument("unknown argument: " + name);
        }
        if (config.phase != "preload" && config.phase != "run")
            throw std::invalid_argument("--phase must be preload or run");
        if (config.dir.empty())
            throw std::invalid_argument("--dir is required");
        if (config.key_bytes < 16 || config.key_bytes > 512 || (config.key_bytes_max > 0 && (config.key_bytes_max < config.key_bytes ||
                                                                                           config.key_bytes_max > 512)))
            throw std::invalid_argument("--key-bytes (and --key-bytes-max) must be 16 to 512");
        if (config.value_bytes_max > 0 && config.value_bytes_max < config.value_bytes)
            throw std::invalid_argument("--value-bytes-max must be at least --value-bytes");
        if (config.key_families > 26)
            throw std::invalid_argument("--key-families must be at most 26");
        if (config.read_percent + config.rmw_percent + config.delete_percent + config.scan_percent > 100)
            throw std::invalid_argument("--read-percent + --rmw-percent + --delete-percent + --scan-percent must be <= 100");
        if (config.workload == "timeseries")
        {
            if (config.retention == 0 || config.records % config.retention != 0 ||
                (config.records / config.retention) % config.threads != 0)
                throw std::invalid_argument("timeseries needs --retention dividing --records into a multiple of --threads series");
            if (config.key_bytes != 16 || config.key_bytes_max > 0 || config.key_families > 0 || config.rmw_percent > 0 ||
                config.delete_percent > 0 || config.scan_length == 0)
                throw std::invalid_argument("timeseries uses 16-byte keys, no key families, reads, scans and appends only");
        }
        else if (config.workload != "kv")
            throw std::invalid_argument("--workload must be kv or timeseries");
        if (config.rate < 0 || (config.rate > 0 && config.workload != "kv"))
            throw std::invalid_argument("--rate must be positive and needs the kv workload");
        else if (config.batch_size > 1 && (config.read_percent + config.rmw_percent + config.delete_percent + config.scan_percent) > 0)
            throw std::invalid_argument("--batch-size > 1 supports write-only runs (except timeseries)");
        if (config.threads == 0 || config.read_percent > 100 || config.duration_seconds == 0 || config.records == 0 || config.batch_size == 0 ||
            config.records >= (1ULL << 32))
            throw std::invalid_argument("invalid --threads, --read-percent, --duration-seconds or --records");
        return config;
    }
}

int main(int argc, char **argv)
{
    try
    {
        const auto config = parse(argc, argv);
        Json result = config.phase == "preload" ? preload(config) : run(config);
        result["config"] = {{"records", config.records}, {"value_bytes", config.value_bytes}, {"compression", config.compression},
                            {"block_bytes", config.block_bytes}, {"write_buffer_bytes", config.write_buffer_bytes},
                            {"target_file_bytes", config.target_file_bytes}, {"max_bytes_for_level_base", config.max_bytes_for_level_base},
                            {"max_write_buffer_number", config.max_write_buffer_number}, {"background_jobs", config.background_jobs},
                            {"subcompactions", config.subcompactions}, {"levels", config.levels},
                            {"block_cache_bytes", config.block_cache_bytes}, {"disable_wal", config.disable_wal},
                            {"adaptive_yield", config.adaptive_yield}, {"batch_size", config.batch_size}, {"threads", config.threads}, {"read_percent", config.read_percent},
                            {"rmw_percent", config.rmw_percent}, {"delete_percent", config.delete_percent},
                            {"distribution", config.distribution}, {"zipf_theta", config.zipf_theta},
                            {"use_direct_io", config.use_direct_io}, {"l0_trigger", config.l0_trigger},
                            {"level_multiplier", config.level_multiplier}, {"blob_min_bytes", config.blob_min_bytes},
                            {"workload", config.workload}, {"rate", config.rate}, {"key_bytes_max", config.key_bytes_max},
                            {"value_bytes_max", config.value_bytes_max}, {"key_families", config.key_families},
                            {"scan_percent", config.scan_percent}, {"scan_length", config.scan_length},
                            {"retention", config.retention},
                            {"pipelined_write", config.pipelined_write}, {"concurrent_memtable", config.concurrent_memtable},
                            {"flush_jobs", config.flush_jobs}, {"allow_fallocate", config.allow_fallocate},
                            {"bottommost_compression", config.bottommost_compression},
                            {"replication", config.replication}, {"replicate_to", config.replicate_to},
                            {"replica_backend", config.replica_backend},
                            {"warmup_seconds", config.warmup_seconds}, {"duration_seconds", config.duration_seconds},
                            {"seed", config.seed}};
        result["engine_fingerprint"] = lsm_lab::engine_fingerprint();
        std::cout << result.dump() << std::endl;
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << Json{{"status", "error"}, {"detail", error.what()}}.dump() << std::endl;
        return 1;
    }
}
