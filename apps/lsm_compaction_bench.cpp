// Isolated compaction benchmark and differential checker.
//
// For each requested mode it builds the same database deterministically
// (identical keys, values, sequence numbers and held snapshots), runs one full
// CompactRange, and reports wall time, whole-process CPU time, per-job
// statistics and a digest of every internal version (user key, sequence,
// type, value). Modes that implement compaction correctly must produce
// identical digests.

#include "lsm_lab/block_transform.hpp"
#include "lsm_lab/runtime.hpp"

#include <rocksdb/db.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/listener.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>

#include <sys/resource.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "db/db_impl/db_impl.h"
#include "db/dbformat.h"
#include "memory/arena.h"
#include "table/internal_iterator.h"
#include "util/cast_util.h"

namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace
{
    struct Config
    {
        fs::path dir;
        std::vector<std::string> modes{"native_cpu"};
        std::uint64_t records = 200000;
        std::uint32_t key_bytes = 16;
        std::uint32_t value_bytes = 128;
        std::uint32_t l0_files = 8;
        bool base_level = true;
        double overwrite_fraction = 0.5;
        double delete_fraction = 0.05;
        std::uint32_t snapshots = 0;
        std::string compression = "lz4";
        std::uint64_t block_bytes = 4096;
        std::uint64_t target_file_bytes = 64ULL << 20;
        std::uint32_t subcompactions = 1;
        std::uint32_t repetitions = 1;
        std::uint64_t seed = 1;
        bool keep = false;
    };

    std::uint64_t mix64(std::uint64_t value)
    {
        value += 0x9e3779b97f4a7c15ULL;
        value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
        return value ^ (value >> 31);
    }

    std::string make_key(const Config &config, std::uint64_t index)
    {
        std::string key(config.key_bytes, '\0');
        const auto prefix = mix64(index ^ config.seed);
        for (std::size_t offset = 0; offset < key.size(); ++offset)
        {
            const auto source = offset < 8 ? prefix : index;
            key[offset] = static_cast<char>((source >> (8 * (7 - offset % 8))) & 255);
        }
        return key;
    }

    // Half pseudorandom, half repeating pattern, like the dataset generator.
    std::string make_value(const Config &config, std::uint64_t index, std::uint64_t generation)
    {
        std::string value(config.value_bytes, '\0');
        std::uint64_t word = 0;
        const auto base = mix64(index * 1315423911ULL + generation);
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
                value[offset] = static_cast<char>('a' + (offset + generation) % 16);
            }
        }
        return value;
    }

    class JobRecorder final : public rocksdb::EventListener
    {
    public:
        void OnCompactionCompleted(rocksdb::DB *, const rocksdb::CompactionJobInfo &info) override
        {
            const std::lock_guard guard(mutex_);
            jobs.push_back(Json{{"status", info.status.ToString()},
                            {"output_level", info.output_level},
                            {"elapsed_micros", info.stats.elapsed_micros},
                            {"cpu_micros", info.stats.cpu_micros},
                            {"input_files", info.stats.num_input_files},
                            {"output_files", info.stats.num_output_files},
                            {"input_records", info.stats.num_input_records},
                            {"output_records", info.stats.num_output_records},
                            {"input_bytes", info.stats.total_input_bytes},
                            {"output_bytes", info.stats.total_output_bytes}});
        }
        std::mutex mutex_;
        std::vector<Json> jobs;
    };

    rocksdb::Options make_options(const Config &config, const std::string &mode,
                                  std::shared_ptr<lsm_lab::BatchTransform> &transform)
    {
        rocksdb::Options options;
        options.create_if_missing = true;
        options.error_if_exists = true;
        options.disable_auto_compactions = true;
        options.write_buffer_size = 1ULL << 30;
        options.max_write_buffer_number = 4;
        options.target_file_size_base = config.target_file_bytes;
        options.max_subcompactions = config.subcompactions;
        options.max_background_jobs = 4;
        options.paranoid_checks = true;
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
        table.block_cache = rocksdb::NewLRUCache(64ULL << 20);
        table.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10));
        if (mode == "native_cpu")
        {
        }
        else if (mode == "gpu_output")
        {
            if (!transform)
                transform = lsm_lab::make_transform("cuda_hybrid");
        }
        else if (mode == "cpu_merge" || mode == "cuda_merge")
        {
            if (!transform)
                transform = lsm_lab::make_transform(mode);
        }
        else
        {
            throw std::invalid_argument("unknown mode: " + mode);
        }
        table.compaction_block_transform = transform;
        options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table));
        return options;
    }

    void check(const rocksdb::Status &status, const char *what)
    {
        if (!status.ok())
            throw std::runtime_error(std::string(what) + ": " + status.ToString());
    }

    double process_cpu_seconds()
    {
        rusage usage{};
        getrusage(RUSAGE_SELF, &usage);
        return usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6 + usage.ru_stime.tv_sec +
               usage.ru_stime.tv_usec / 1e6;
    }

    struct Digest
    {
        std::string hex;
        std::uint64_t versions = 0;
        std::uint64_t deletions = 0;
        std::uint64_t bytes = 0;
    };

    Digest digest_versions(rocksdb::DB *db)
    {
        auto *impl = rocksdb::static_cast_with_check<rocksdb::DBImpl>(db->GetRootDB());
        rocksdb::ReadOptions read;
        rocksdb::Arena arena;
        rocksdb::ScopedArenaPtr<rocksdb::InternalIterator> iter(
            impl->NewInternalIterator(read, &arena, rocksdb::kMaxSequenceNumber, db->DefaultColumnFamily()));
        Digest digest;
        std::string chunk;
        std::string running;
        for (iter->SeekToFirst(); iter->Valid(); iter->Next())
        {
            rocksdb::ParsedInternalKey ikey;
            check(rocksdb::ParseInternalKey(iter->key(), &ikey, true), "parse internal key");
            chunk.append(ikey.user_key.data(), ikey.user_key.size());
            chunk += '\x1f';
            chunk += std::to_string(ikey.sequence);
            chunk += '\x1f';
            chunk += std::to_string(static_cast<int>(ikey.type));
            chunk += '\x1f';
            chunk.append(iter->value().data(), iter->value().size());
            chunk += '\x1e';
            ++digest.versions;
            digest.deletions += ikey.type == rocksdb::kTypeDeletion;
            digest.bytes += iter->key().size() + iter->value().size();
            if (chunk.size() > (8U << 20))
            {
                running = lsm_lab::digest_bytes(running + lsm_lab::digest_bytes(chunk));
                chunk.clear();
            }
        }
        check(iter->status(), "internal iterator");
        running = lsm_lab::digest_bytes(running + lsm_lab::digest_bytes(chunk));
        digest.hex = lsm_lab::digest_hex(running);
        return digest;
    }

    // Point lookups (which consult the Bloom filters, unlike iterators) of
    // every key index (strided to at most 200000 lookups), hashed together
    // with their outcome. Modes that build correct filters and indexes agree.
    std::string lookup_digest(rocksdb::DB *db, const Config &config, std::uint64_t *found)
    {
        rocksdb::ReadOptions read;
        read.verify_checksums = true;
        const auto stride = std::max<std::uint64_t>(1, config.records / 200000);
        std::uint64_t hash = 1469598103934665603ULL;
        const auto mix = [&](const rocksdb::Slice &bytes) {
            for (std::size_t index = 0; index < bytes.size(); ++index)
                hash = (hash ^ static_cast<unsigned char>(bytes[index])) * 1099511628211ULL;
        };
        *found = 0;
        rocksdb::PinnableSlice value;
        for (std::uint64_t index = 0; index < config.records; index += stride)
        {
            const auto key = make_key(config, index);
            value.Reset();
            const auto status = db->Get(read, db->DefaultColumnFamily(), key, &value);
            if (!status.ok() && !status.IsNotFound())
                throw std::runtime_error("lookup: " + status.ToString());
            mix(key);
            mix(status.ok() ? rocksdb::Slice("+") : rocksdb::Slice("-"));
            if (status.ok())
            {
                ++*found;
                mix(value);
            }
        }
        return std::to_string(hash);
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

    // Builds the same database for every mode: optional bottom level holding
    // every key, then l0_files overlapping flushes of overwrites and deletes.
    void build(rocksdb::DB *db, const Config &config, std::vector<const rocksdb::Snapshot *> &snapshots)
    {
        rocksdb::WriteOptions write;
        write.disableWAL = true;
        std::mt19937_64 random(config.seed);
        std::vector<std::uint64_t> generation(config.records, 0);
        if (config.base_level)
        {
            rocksdb::WriteBatch batch;
            for (std::uint64_t index = 0; index < config.records; ++index)
            {
                check(batch.Put(make_key(config, index), make_value(config, index, 0)), "batch put");
                if (batch.Count() >= 10000)
                {
                    check(db->Write(write, &batch), "write");
                    batch.Clear();
                }
            }
            check(db->Write(write, &batch), "write");
            check(db->Flush(rocksdb::FlushOptions()), "flush base");
            rocksdb::CompactRangeOptions compact;
            compact.bottommost_level_compaction = rocksdb::BottommostLevelCompaction::kForce;
            check(db->CompactRange(compact, nullptr, nullptr), "compact base");
        }
        const auto per_file = static_cast<std::uint64_t>(
            static_cast<double>(config.records) * (config.base_level ? config.overwrite_fraction : 1.0) /
            config.l0_files);
        std::uniform_int_distribution<std::uint64_t> pick(0, config.records - 1);
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        const auto snapshot_every = config.snapshots == 0 ? 0 : std::max<std::uint32_t>(1, config.l0_files / (config.snapshots + 1));
        for (std::uint32_t file = 0; file < config.l0_files; ++file)
        {
            rocksdb::WriteBatch batch;
            for (std::uint64_t written = 0; written < per_file; ++written)
            {
                const auto index = config.base_level ? pick(random) : (file * per_file + written) % config.records;
                if (unit(random) < config.delete_fraction)
                {
                    check(batch.Delete(make_key(config, index)), "batch delete");
                }
                else
                {
                    check(batch.Put(make_key(config, index), make_value(config, index, ++generation[index])),
                          "batch put");
                }
                if (batch.Count() >= 10000)
                {
                    check(db->Write(write, &batch), "write");
                    batch.Clear();
                }
            }
            check(db->Write(write, &batch), "write");
            check(db->Flush(rocksdb::FlushOptions()), "flush");
            if (snapshot_every != 0 && (file + 1) % snapshot_every == 0 && snapshots.size() < config.snapshots)
                snapshots.push_back(db->GetSnapshot());
        }
    }

    // The transform (and its device/pinned buffers) is reused across
    // repetitions of a mode, as in a long-running database; repetition 0 is cold.
    Json run_once(const Config &config, const std::string &mode, std::uint32_t repetition,
                  std::shared_ptr<lsm_lab::BatchTransform> &transform)
    {
        const auto path = config.dir / (mode + "-" + std::to_string(repetition));
        fs::remove_all(path);
        auto options = make_options(config, mode, transform);
        auto recorder = std::make_shared<JobRecorder>();
        options.listeners.push_back(recorder);
        std::unique_ptr<rocksdb::DB> db;
        check(rocksdb::DB::Open(options, path.string(), &db), "open");
        std::vector<const rocksdb::Snapshot *> snapshots;
        build(db.get(), config, snapshots);
        const auto before = digest_versions(db.get());
        const auto shape_before = level_shape(db.get());
        {
            const std::lock_guard guard(recorder->mutex_);
            recorder->jobs.clear();
        }
        Json transform_before = transform ? transform->statistics() : Json(nullptr);

        rocksdb::CompactRangeOptions compact;
        compact.bottommost_level_compaction = rocksdb::BottommostLevelCompaction::kSkip;
        const auto cpu_start = process_cpu_seconds();
        const auto wall_start = std::chrono::steady_clock::now();
        check(db->CompactRange(compact, nullptr, nullptr), "compact");
        const auto wall_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_start).count();
        const auto cpu_seconds = process_cpu_seconds() - cpu_start;

        check(db->VerifyChecksum(), "verify checksum");
        const auto after = digest_versions(db.get());
        std::uint64_t lookups_found = 0;
        const auto lookups = lookup_digest(db.get(), config, &lookups_found);
        Json result = {{"mode", mode},
                       {"repetition", repetition},
                       {"wall_seconds", wall_seconds},
                       {"process_cpu_seconds", cpu_seconds},
                       {"levels_before", shape_before},
                       {"levels_after", level_shape(db.get())},
                       {"versions_before", before.versions},
                       {"versions_after", after.versions},
                       {"deletions_after", after.deletions},
                       {"logical_bytes_after", after.bytes},
                       {"digest_after", after.hex + "/" + lookups},
                       {"lookups_found", lookups_found},
                       {"snapshots_held", snapshots.size()},
                       {"transform", transform ? transform->statistics() : Json(nullptr)},
                       {"transform_before", transform_before}};
        {
            const std::lock_guard guard(recorder->mutex_);
            result["jobs"] = recorder->jobs;
            std::uint64_t input_bytes = 0, input_records = 0, cpu_micros = 0, elapsed = 0;
            for (const auto &job : recorder->jobs)
            {
                input_bytes += job["input_bytes"].get<std::uint64_t>();
                input_records += job["input_records"].get<std::uint64_t>();
                cpu_micros += job["cpu_micros"].get<std::uint64_t>();
                elapsed += job["elapsed_micros"].get<std::uint64_t>();
            }
            result["input_bytes"] = input_bytes;
            result["input_records"] = input_records;
            result["rocksdb_cpu_seconds"] = cpu_micros / 1e6;
            result["rocksdb_elapsed_seconds"] = elapsed / 1e6;
            result["input_mib_per_wall_second"] = input_bytes / 1048576.0 / wall_seconds;
            result["process_cpu_seconds_per_gib"] = cpu_seconds / (input_bytes / 1073741824.0);
        }
        for (const auto *snapshot : snapshots)
            db->ReleaseSnapshot(snapshot);
        check(db->Close(), "close");
        db.reset();
        if (!config.keep)
            fs::remove_all(path);
        return result;
    }

    Config parse(int argc, char **argv)
    {
        Config config;
        for (int index = 1; index < argc; ++index)
        {
            const std::string name = argv[index];
            const auto value = [&]() -> std::string {
                if (index + 1 >= argc)
                    throw std::invalid_argument("missing value for " + name);
                return argv[++index];
            };
            if (name == "--dir")
                config.dir = value();
            else if (name == "--modes")
            {
                config.modes.clear();
                std::string list = value();
                std::size_t start = 0;
                while (start <= list.size())
                {
                    const auto end = list.find(',', start);
                    config.modes.push_back(list.substr(start, end - start));
                    if (end == std::string::npos)
                        break;
                    start = end + 1;
                }
            }
            else if (name == "--records")
                config.records = std::stoull(value());
            else if (name == "--key-bytes")
                config.key_bytes = static_cast<std::uint32_t>(std::stoul(value()));
            else if (name == "--value-bytes")
                config.value_bytes = static_cast<std::uint32_t>(std::stoul(value()));
            else if (name == "--l0-files")
                config.l0_files = static_cast<std::uint32_t>(std::stoul(value()));
            else if (name == "--no-base")
                config.base_level = false;
            else if (name == "--overwrite-fraction")
                config.overwrite_fraction = std::stod(value());
            else if (name == "--delete-fraction")
                config.delete_fraction = std::stod(value());
            else if (name == "--snapshots")
                config.snapshots = static_cast<std::uint32_t>(std::stoul(value()));
            else if (name == "--compression")
                config.compression = value();
            else if (name == "--block-bytes")
                config.block_bytes = std::stoull(value());
            else if (name == "--target-file-bytes")
                config.target_file_bytes = std::stoull(value());
            else if (name == "--subcompactions")
                config.subcompactions = static_cast<std::uint32_t>(std::stoul(value()));
            else if (name == "--repetitions")
                config.repetitions = static_cast<std::uint32_t>(std::stoul(value()));
            else if (name == "--seed")
                config.seed = std::stoull(value());
            else if (name == "--keep")
                config.keep = true;
            else
                throw std::invalid_argument("unknown argument: " + name);
        }
        if (config.dir.empty())
            throw std::invalid_argument("--dir is required");
        if (config.key_bytes < 8 || config.records == 0 || config.l0_files == 0 || config.repetitions == 0)
            throw std::invalid_argument("key-bytes >= 8 and records, l0-files, repetitions > 0 are required");
        return config;
    }
}

int main(int argc, char **argv)
{
    try
    {
        const auto config = parse(argc, argv);
        fs::create_directories(config.dir);
        std::optional<std::string> reference;
        bool consistent = true;
        std::map<std::string, std::shared_ptr<lsm_lab::BatchTransform>> transforms;
        for (std::uint32_t repetition = 0; repetition < config.repetitions; ++repetition)
        {
            for (const auto &mode : config.modes)
            {
                auto result = run_once(config, mode, repetition, transforms[mode]);
                if (!reference)
                    reference = result["digest_after"].get<std::string>();
                result["matches_reference"] = result["digest_after"] == *reference;
                consistent = consistent && result["matches_reference"].get<bool>();
                std::cout << result.dump() << std::endl;
            }
        }
        return consistent ? 0 : 3;
    }
    catch (const std::exception &error)
    {
        std::cerr << "lsm_compaction_bench: " << error.what() << std::endl;
        return 1;
    }
}
