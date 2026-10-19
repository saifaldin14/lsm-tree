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
#include "lsm_lab/runtime.hpp"

#include <rocksdb/db.h>
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
        bool verify = true;
        std::uint64_t seed = 1;
    };

    std::uint64_t mix64(std::uint64_t value)
    {
        value += 0x9e3779b97f4a7c15ULL;
        value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
        return value ^ (value >> 31);
    }

    // Bytes 0-7: hash of the index (spreads keys), bytes 8-15: big-endian index.
    void make_key(const Config &config, std::uint64_t index, std::string &key)
    {
        key.assign(config.key_bytes, '\0');
        const auto prefix = mix64(index ^ config.seed);
        for (std::size_t offset = 0; offset < key.size(); ++offset)
        {
            const auto source = offset < 8 ? prefix : index;
            key[offset] = static_cast<char>((source >> (8 * (7 - offset % 8))) & 255);
        }
    }

    std::uint64_t key_index(const rocksdb::Slice &key)
    {
        std::uint64_t index = 0;
        for (std::size_t offset = 8; offset < 16; ++offset)
            index = (index << 8) | static_cast<unsigned char>(key[offset]);
        return index;
    }

    // Half pseudorandom, half repeating pattern (compresses roughly 2:1 with LZ4).
    void make_value(const Config &config, std::uint64_t index, std::uint64_t version, std::string &value)
    {
        value.resize(config.value_bytes);
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

    rocksdb::Options make_options(const Config &config, const std::shared_ptr<lsm_lab::BatchTransform> &transform)
    {
        rocksdb::Options options;
        options.write_buffer_size = config.write_buffer_bytes;
        options.max_write_buffer_number = static_cast<int>(config.max_write_buffer_number);
        options.target_file_size_base = config.target_file_bytes;
        options.max_bytes_for_level_base = config.max_bytes_for_level_base;
        options.max_background_jobs = static_cast<int>(config.background_jobs);
        options.max_subcompactions = config.subcompactions;
        options.num_levels = static_cast<int>(config.levels);
        options.paranoid_checks = true;
        // With more writers than cores, adaptive yield spins waiting writers and burns the CPU budget.
        options.enable_write_thread_adaptive_yield = config.adaptive_yield;
        options.statistics = rocksdb::CreateDBStatistics();
        options.statistics->set_stats_level(rocksdb::StatsLevel::kExceptDetailedTimers);
        if (config.compression == "lz4")
            options.compression = rocksdb::kLZ4Compression;
        else if (config.compression == "zstd")
            options.compression = rocksdb::kZSTD;
        else if (config.compression == "none")
            options.compression = rocksdb::kNoCompression;
        else
            throw std::invalid_argument("compression must be lz4, zstd or none");
        rocksdb::BlockBasedTableOptions table;
        table.format_version = 6;
        table.checksum = rocksdb::kCRC32c;
        table.block_size = config.block_bytes;
        table.block_cache = rocksdb::NewLRUCache(config.block_cache_bytes);
        table.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10));
        table.compaction_block_transform = transform;
        options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table));
        return options;
    }

    std::shared_ptr<lsm_lab::BatchTransform> make_backend(const std::string &mode)
    {
        if (mode == "native_cpu")
            return nullptr;
        if (mode == "gpu_output")
            return lsm_lab::make_transform("cuda_hybrid");
        if (mode == "cuda_merge" || mode == "cpu_merge")
            return lsm_lab::make_transform(mode);
        throw std::invalid_argument("mode must be native_cpu, gpu_output, cpu_merge or cuda_merge");
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
            if (key.size() != config.key_bytes)
                throw std::runtime_error("verification: unexpected key size");
            const auto index = key_index(key);
            if (index >= config.records || seen[index])
                throw std::runtime_error("verification: unexpected or duplicate key " + std::to_string(index));
            seen[index] = true;
            make_value(config, index, versions[index], expected);
            if (iterator->value() != rocksdb::Slice(expected))
                throw std::runtime_error("verification: wrong value for key " + std::to_string(index) + " (expected version " +
                                         std::to_string(versions[index]) + ")");
            ++count;
        }
        check(iterator->status(), "verification iterator");
        if (count != config.records)
            throw std::runtime_error("verification: found " + std::to_string(count) + " of " + std::to_string(config.records) + " keys");
        return {{"keys", count}, {"seconds", std::chrono::duration<double>(Clock::now() - started).count()}};
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

    struct alignas(64) ThreadState
    {
        std::atomic<std::uint64_t> ops{0};
        std::uint64_t window_reads = 0;
        std::uint64_t window_writes = 0;
        Histogram read_latency;
        Histogram write_latency;
        std::string error;
    };

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
        std::unique_ptr<rocksdb::DB> db;
        check(rocksdb::DB::Open(options, config.dir.string(), &db), "open");
        const auto statistics = options.statistics;
        const auto levels_before = level_shape(db.get());

        std::vector<std::uint32_t> versions(config.records, 0);
        std::vector<ThreadState> states(config.threads);
        std::atomic<int> phase{0}; // 0 warmup, 1 measurement, 2 stop
        const auto per_thread = config.records / config.threads;

        auto worker = [&](std::uint32_t thread)
        {
            auto &state = states[thread];
#if defined(__linux__)
            pthread_setname_np(pthread_self(), "lsm-foreground");
#endif
            try
            {
                std::mt19937_64 random(mix64(config.seed * 7919 + thread));
                std::uniform_int_distribution<std::uint64_t> pick(0, per_thread - 1);
                std::uniform_int_distribution<std::uint32_t> percent(0, 99);
                rocksdb::WriteOptions write;
                write.disableWAL = config.disable_wal;
                rocksdb::ReadOptions read;
                std::string key, value, expected;
                rocksdb::PinnableSlice found;
                rocksdb::WriteBatch batch;
                while (true)
                {
                    const auto current = phase.load(std::memory_order_relaxed);
                    if (current == 2)
                        break;
                    const auto index = thread + config.threads * pick(random);
                    const bool is_read = percent(random) < config.read_percent;
                    make_key(config, index, key);
                    if (is_read)
                    {
                        const auto begin = Clock::now();
                        found.Reset();
                        check(db->Get(read, db->DefaultColumnFamily(), key, &found), "get");
                        const auto ns = static_cast<std::uint64_t>((Clock::now() - begin).count());
                        make_value(config, index, versions[index], expected);
                        if (found != rocksdb::Slice(expected))
                            throw std::runtime_error("online read returned a wrong value for key " + std::to_string(index));
                        if (current == 1)
                        {
                            state.read_latency.add(ns);
                            ++state.window_reads;
                        }
                    }
                    else if (config.batch_size == 1)
                    {
                        const auto version = versions[index] + 1;
                        make_value(config, index, version, value);
                        const auto begin = Clock::now();
                        check(db->Put(write, key, value), "put");
                        const auto ns = static_cast<std::uint64_t>((Clock::now() - begin).count());
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
                            make_value(config, owned, ++versions[owned], value);
                            check(batch.Put(key, value), "batch put");
                        }
                        const auto begin = Clock::now();
                        check(db->Write(write, &batch), "write batch");
                        const auto ns = static_cast<std::uint64_t>((Clock::now() - begin).count());
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
                        {"compactions", totals->snapshot()}};
        };
        const auto total_ops = [&]
        {
            std::uint64_t sum = 0;
            for (const auto &state : states)
                sum += state.ops.load(std::memory_order_relaxed);
            return sum;
        };

        const auto run_cpu_start = process_cpu();
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
            timeline.push_back({{"second", second},
                                {"phase", second <= config.warmup_seconds ? "warmup" : "measurement"},
                                {"ops", ops - previous_ops},
                                {"stall_micros", stall - previous_stall},
                                {"l0_files", property(db.get(), "rocksdb.num-files-at-level0")},
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
                         return Json{{"ops", all_ops}, {"load_seconds", load_seconds}, {"drain_seconds", drain_seconds},
                                     {"seconds", total}, {"ops_per_second", all_ops / total},
                                     {"cpu_seconds", total_cpu}, {"drain_cpu_seconds", drain_cpu},
                                     {"cpu_microseconds_per_op", all_ops > 0 ? total_cpu * 1e6 / all_ops : 0.0},
                                     {"compaction_cpu_seconds", (run_counters_end.at("compactions").at("cpu_micros").get<double>() -
                                                                 run_counters_start.at("compactions").at("cpu_micros").get<double>()) / 1e6},
                                     {"write_amplification", written > 0 ? rewritten / written : 0.0}};
                     }()},
                    {"levels_before", levels_before},
                    {"levels_after_run", level_shape(db.get())},
                    {"transform", backend ? backend->statistics() : Json(nullptr)}};
        if (config.verify)
        {
            result["verification"] = verify_all(db.get(), config, versions);
            result["levels_after_verification"] = level_shape(db.get());
            result["transform_after_verification"] = backend ? backend->statistics() : Json(nullptr);
        }
        check(db->Close(), "close");
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
        if (config.key_bytes != 16)
            throw std::invalid_argument("--key-bytes must be 16");
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
