#include "lsm_lab/dataset.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <rocksdb/cache.h>
#include <rocksdb/convenience.h>
#include <rocksdb/db.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/sst_file_writer.h>
#include <rocksdb/table.h>
#include <rocksdb/version.h>
#include <rocksdb/write_batch.h>

namespace lsm_lab
{
    namespace
    {
        namespace fs = std::filesystem;
        using Json = nlohmann::json;
        using SteadyClock = std::chrono::steady_clock;

        void require(bool condition, const std::string &message)
        {
            if (!condition)
            {
                throw std::runtime_error(message);
            }
        }

        void check(const rocksdb::Status &result)
        {
            require(result.ok(), result.ToString());
        }

        std::uint64_t mix64(std::uint64_t value)
        {
            value += 0x9e3779b97f4a7c15ULL;
            value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
            value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
            return value ^ (value >> 31);
        }

        void encode64(std::string &target, std::size_t offset, std::uint64_t value)
        {
            for (std::size_t byte = 0; byte < 8; ++byte)
            {
                target[offset + byte] = static_cast<char>((value >> (56 - byte * 8)) & 255);
            }
        }

        std::uint64_t decode_index(const rocksdb::Slice &key)
        {
            require(key.size() >= 16, "invalid generated key length");
            std::uint64_t index = 0;
            for (std::size_t offset = 8; offset < 16; ++offset)
            {
                index = (index << 8) | static_cast<unsigned char>(key[offset]);
            }
            return index;
        }

        class Digest
        {
        public:
            Digest() : context_(EVP_MD_CTX_new(), EVP_MD_CTX_free)
            {
                require(context_ != nullptr && EVP_DigestInit_ex(context_.get(), EVP_sha256(), nullptr) == 1,
                        "cannot initialize SHA-256");
            }

            void add(const char *bytes, std::size_t size)
            {
                require(EVP_DigestUpdate(context_.get(), bytes, size) == 1, "SHA-256 update failed");
            }

            std::string finish()
            {
                std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
                unsigned int size = 0;
                require(EVP_DigestFinal_ex(context_.get(), bytes.data(), &size) == 1 && size == 32,
                        "SHA-256 finalization failed");
                constexpr char digits[] = "0123456789abcdef";
                std::string result;
                for (unsigned int index = 0; index < size; ++index)
                {
                    result += digits[bytes[index] >> 4];
                    result += digits[bytes[index] & 15];
                }
                return result;
            }

        private:
            std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context_;
        };

        std::string sha256(const fs::path &path)
        {
            std::ifstream input(path, std::ios::binary);
            require(input.good(), "cannot read file for SHA-256: " + path.string());
            Digest digest;
            std::array<char, 65536> buffer{};
            while (input.read(buffer.data(), buffer.size()) || input.gcount() != 0)
            {
                digest.add(buffer.data(), static_cast<std::size_t>(input.gcount()));
            }
            require(input.eof() && !input.bad(), "file read failed: " + path.string());
            return digest.finish();
        }

        void sync_directory(const fs::path &path)
        {
            std::unique_ptr<rocksdb::Directory> directory;
            check(rocksdb::Env::Default()->NewDirectory(path.string(), &directory));
            check(directory->Fsync());
        }

        void publish_manifest(const fs::path &root, const Json &manifest, const std::string &name = "dataset.json")
        {
            auto *env = rocksdb::Env::Default();
            const auto temporary = root / (".dataset-" + env->GenerateUniqueId());
            std::unique_ptr<rocksdb::WritableFile> file;
            check(env->NewWritableFile(temporary.string(), &file, rocksdb::EnvOptions()));
            check(file->Append(manifest.dump(2) + "\n"));
            check(file->Fsync());
            check(file->Close());
            file.reset();
            check(env->RenameFile(temporary.string(), (root / name).string()));
            sync_directory(root);
        }

        void disk_budget(const fs::path &path, std::uint64_t minimum, std::uint64_t extra = 0)
        {
            const auto available = fs::space(path).available;
            require(available >= minimum && extra <= available - minimum,
                    "insufficient free disk space for dataset operation");
        }

        fs::path reserve_directory(const fs::path &destination, const Spec &spec)
        {
            const auto absolute = fs::absolute(destination).lexically_normal();
            require(!absolute.filename().empty() && absolute != absolute.root_path(),
                    "destination must name a new dataset directory");
            const auto parent = fs::canonical(absolute.parent_path());
            disk_budget(parent, spec.limits.minimum_free_disk_bytes);
            const auto root = parent / absolute.filename();
            require(fs::create_directory(root), "destination already exists: " + root.string());
            sync_directory(parent);
            publish_manifest(root, {{"schema_version", 1U}, {"owner", "lsm-lab"}, {"state", "building"}, {"spec", spec_json(spec)}});
            return root;
        }

        Json inventory(const fs::path &database_path)
        {
            std::vector<fs::path> files;
            for (const auto &entry : fs::directory_iterator(database_path))
            {
                const auto name = entry.path().filename().string();
                if (name == "LOCK" || name == "LOG" || name.starts_with("LOG.old."))
                {
                    continue;
                }
                require(fs::is_regular_file(entry.symlink_status()), "unexpected dataset entry: " + name);
                files.push_back(entry.path());
            }
            std::sort(files.begin(), files.end());
            Json result = Json::array();
            for (const auto &file : files)
            {
                result.push_back({{"name", file.filename().string()}, {"bytes", fs::file_size(file)}, {"sha256", sha256(file)}});
            }
            return result;
        }

        Json seal(const fs::path &root, const Spec &spec, const std::string &parent_id = {})
        {
            const auto canonical_spec = spec_json(spec).dump();
            Digest digest;
            digest.add(canonical_spec.data(), canonical_spec.size());
            Json manifest{{"schema_version", 1U}, {"owner", "lsm-lab"}, {"state", "ready"}, {"dataset_id", rocksdb::Env::Default()->GenerateUniqueId()}, {"parent_id", parent_id}, {"rocksdb_version", rocksdb::GetRocksVersionAsString()}, {"generator", "splitmix64-record-v1"}, {"spec", spec_json(spec)}, {"spec_sha256", digest.finish()}, {"files", inventory(root / "db")}, {"logical_write_bytes", logical_write_bytes(spec.dataset)}};
            publish_manifest(root, manifest);
            return manifest;
        }

        Json read_manifest(const fs::path &root)
        {
            require(fs::is_directory(fs::symlink_status(root)), "dataset directory is missing or is a symlink");
            const auto path = root / "dataset.json";
            require(fs::is_regular_file(fs::symlink_status(path)), "dataset manifest is not a regular file");
            require(fs::file_size(path) <= 4ULL << 20, "dataset manifest exceeds 4 MiB");
            std::ifstream input(path);
            auto manifest = Json::parse(input);
            require(manifest.at("schema_version") == 1 && manifest.at("owner") == "lsm-lab" &&
                        manifest.at("state") == "ready" && manifest.at("generator") == "splitmix64-record-v1",
                    "dataset is incomplete, unsupported, or not owned by lsm-lab");
            require(fs::is_directory(fs::symlink_status(root / "db")), "dataset db is not a directory");
            const auto spec = parse_spec(manifest.at("spec").dump());
            const auto encoded = spec_json(spec).dump();
            Digest digest;
            digest.add(encoded.data(), encoded.size());
            require(manifest.at("spec_sha256") == digest.finish(), "dataset spec fingerprint mismatch");
            require(manifest.at("files").is_array() && !manifest.at("files").empty(), "empty dataset inventory");
            return manifest;
        }

        class OfflineLock
        {
        public:
            explicit OfflineLock(const fs::path &root)
            {
                check(rocksdb::Env::Default()->LockFile((root / "db" / "LOCK").string(), &lock_));
            }
            ~OfflineLock()
            {
                rocksdb::Env::Default()->UnlockFile(lock_).PermitUncheckedError();
            }
            OfflineLock(const OfflineLock &) = delete;
            OfflineLock &operator=(const OfflineLock &) = delete;

        private:
            rocksdb::FileLock *lock_ = nullptr;
        };

        Json verify_locked(const fs::path &root, const Json &manifest,
                           const std::map<std::uint64_t, std::uint64_t> &versions = {}, bool check_inventory = true)
        {
            // Version entries at or beyond dataset.records are keys inserted by a trial; they must all be present.
            if (check_inventory)
            {
                require(manifest.at("files") == inventory(root / "db"), "dataset file inventory or SHA-256 mismatch");
            }
            const auto spec = parse_spec(manifest.at("spec").dump());
            auto options = engine_options(spec);
            std::unique_ptr<rocksdb::DB> database;
            check(rocksdb::DB::OpenForReadOnly(options, (root / "db").string(), &database, true));
            check(database->VerifyChecksum());
            const auto inserted = static_cast<std::uint64_t>(std::count_if(versions.begin(), versions.end(), [&](const auto &entry)
                                                                           { return entry.first >= spec.dataset.records; }));
            std::uint64_t count = 0;
            {
                rocksdb::ReadOptions scan;
                scan.fill_cache = false;
                scan.readahead_size = 2ULL << 20;
                std::unique_ptr<rocksdb::Iterator> iterator(database->NewIterator(scan));
                for (iterator->SeekToFirst(); iterator->Valid(); iterator->Next())
                {
                    const auto index = decode_index(iterator->key());
                    const auto changed = versions.find(index);
                    require((index < spec.dataset.records || changed != versions.end()) &&
                                iterator->key() == generated_key(spec.dataset, index),
                            "dataset key does not match generator");
                    const auto generation = changed == versions.end() ? spec.dataset.overwrite_rounds : changed->second;
                    require(iterator->value() == generated_value(spec.dataset, index, generation),
                            "dataset full value does not match generator");
                    ++count;
                }
                check(iterator->status());
            }
            require(count == spec.dataset.records + inserted, "dataset record count mismatch");
            check(database->Close());
            return {{"status", "ok"}, {"dataset_id", manifest.at("dataset_id")}, {"records_verified", count}, {"files_verified", check_inventory ? Json(manifest.at("files").size()) : Json(nullptr)}, {"full_payloads_verified", true}, {"native_checksums_verified", true}, {"file_hashes_verified", check_inventory}, {"inserted_keys_verified", inserted}};
        }

        double seconds_between(SteadyClock::time_point start, SteadyClock::time_point end)
        {
            return std::chrono::duration<double>(end - start).count();
        }

        // Writes only the final generation as sorted, non-overlapping SST files and ingests them. The key prefix
        // is a bijective hash of the index, so partitioning the hash space and sorting each partition yields the
        // exact generator key order without materializing all keys at once.
        Json ingest_records(rocksdb::DB &database, const Spec &spec, const fs::path &root)
        {
            const auto started = SteadyClock::now();
            const auto hardware = std::max(1U, std::thread::hardware_concurrency());
            constexpr std::uint64_t partition_records = 4ULL << 20;
            const auto workers = std::min<std::uint64_t>({hardware, 16U, std::max<std::uint64_t>(1, spec.dataset.records / 65536)});
            const auto partitions = std::max<std::uint64_t>(workers, (spec.dataset.records + partition_records - 1) / partition_records);
            const auto step = std::numeric_limits<std::uint64_t>::max() / partitions;
            const auto generation = spec.dataset.overwrite_rounds;
            const auto staging = root / "ingest-staging";
            require(fs::create_directory(staging), "cannot create ingest staging directory");
            const auto options = engine_options(spec);
            std::vector<std::vector<std::string>> files(partitions);
            std::atomic<std::uint64_t> next{0};
            std::vector<std::exception_ptr> failures(workers);
            std::vector<std::thread> threads;
            for (std::uint64_t worker = 0; worker < workers; ++worker)
            {
                threads.emplace_back([&, worker]
                                     {
                    try
                    {
                        for (auto partition = next.fetch_add(1); partition < partitions; partition = next.fetch_add(1))
                        {
                            const auto low = partition * step;
                            const auto high = (partition + 1) * step;
                            const bool last = partition + 1 == partitions;
                            std::vector<std::pair<std::uint64_t, std::uint64_t>> entries;
                            entries.reserve(spec.dataset.records / partitions + 1024);
                            for (std::uint64_t index = 0; index < spec.dataset.records; ++index)
                            {
                                const auto prefix = mix64(index ^ spec.dataset.seed);
                                if (prefix >= low && (last || prefix < high))
                                    entries.emplace_back(prefix, index);
                            }
                            std::sort(entries.begin(), entries.end());
                            std::unique_ptr<rocksdb::SstFileWriter> writer;
                            for (const auto &entry : entries)
                            {
                                if (!writer)
                                {
                                    disk_budget(root, spec.limits.minimum_free_disk_bytes, spec.engine.target_file_bytes);
                                    const auto path = staging / (std::to_string(partition) + "-" + std::to_string(files[partition].size()) + ".sst");
                                    writer = std::make_unique<rocksdb::SstFileWriter>(rocksdb::EnvOptions(), options);
                                    check(writer->Open(path.string()));
                                    files[partition].push_back(path.string());
                                }
                                check(writer->Put(record_key(spec.dataset, entry.second), record_value(spec.dataset, entry.second, generation)));
                                if (writer->FileSize() >= spec.engine.target_file_bytes)
                                {
                                    check(writer->Finish());
                                    writer.reset();
                                }
                            }
                            if (writer)
                                check(writer->Finish());
                        }
                    }
                    catch (...) { failures[worker] = std::current_exception(); } });
            }
            for (auto &thread : threads)
                thread.join();
            for (const auto &failure : failures)
            {
                if (failure)
                    std::rethrow_exception(failure);
            }
            std::vector<std::string> ordered;
            for (const auto &partition : files)
                ordered.insert(ordered.end(), partition.begin(), partition.end());
            const auto written = SteadyClock::now();
            rocksdb::IngestExternalFileOptions ingest;
            ingest.move_files = true;
            check(database.IngestExternalFile(ordered, ingest));
            fs::remove_all(staging);
            rocksdb::WaitForCompactOptions drain;
            drain.wait_for_purge = true;
            check(database.WaitForCompact(drain));
            const auto finished = SteadyClock::now();
            const auto total = seconds_between(started, finished);
            const auto bytes = spec.dataset.records * (static_cast<std::uint64_t>(spec.dataset.key_bytes) + spec.dataset.value_bytes);
            return {{"load_mode", spec.dataset.load_mode}, {"producer_threads", workers}, {"partitions", partitions},
                    {"files_ingested", ordered.size()}, {"wal", false}, {"generations_materialized", 1},
                    {"final_generation", generation}, {"records_written", spec.dataset.records}, {"logical_bytes_written", bytes},
                    {"load_seconds", seconds_between(started, written)}, {"ingest_and_compaction_wait_seconds", seconds_between(written, finished)},
                    {"total_seconds", total}, {"records_per_second", total > 0 ? spec.dataset.records / total : 0.0},
                    {"logical_mib_per_second", total > 0 ? bytes / total / 1048576.0 : 0.0}};
        }

        Json load_records(rocksdb::DB &database, const Spec &spec, const fs::path &root)
        {
            if (spec.dataset.load_mode == "ingest")
                return ingest_records(database, spec, root);
            const bool bulk = spec.dataset.load_mode == "bulk";
            rocksdb::WriteOptions writes;
            writes.sync = !bulk;
            writes.disableWAL = bulk;
            // Bulk batches stay below the write-group limit so concurrent producers share groups and insert in parallel.
            const std::size_t batch_bytes = bulk ? 1ULL << 20 : 256ULL << 10;
            const auto hardware = std::max(1U, std::thread::hardware_concurrency());
            const auto producers = bulk ? std::min<std::uint64_t>({hardware, 16U, (spec.dataset.records + 65535) / 65536}) : 1;
            const auto started = SteadyClock::now();
            for (std::uint64_t generation = 0; generation <= spec.dataset.overwrite_rounds; ++generation)
            {
                if (!bulk)
                {
                    rocksdb::WriteBatch batch;
                    for (std::uint64_t index = 0; index < spec.dataset.records; ++index)
                    {
                        check(batch.Put(record_key(spec.dataset, index), record_value(spec.dataset, index, generation)));
                        if (batch.GetDataSize() >= batch_bytes || index + 1 == spec.dataset.records)
                        {
                            disk_budget(root, spec.limits.minimum_free_disk_bytes, batch.GetDataSize());
                            check(database.Write(writes, &batch));
                            batch.Clear();
                        }
                    }
                }
                else
                {
                    // Each key is written exactly once per generation and generations are separated by a
                    // barrier, so the logical content is independent of the producer interleaving.
                    constexpr std::uint64_t chunk = 65536;
                    std::atomic<std::uint64_t> next{0};
                    std::vector<std::exception_ptr> failures(producers);
                    std::vector<std::thread> threads;
                    for (std::uint64_t producer = 0; producer < producers; ++producer)
                    {
                        threads.emplace_back([&, producer]
                                             {
                            try
                            {
                                rocksdb::WriteBatch batch;
                                const auto flush = [&]
                                {
                                    disk_budget(root, spec.limits.minimum_free_disk_bytes, batch.GetDataSize());
                                    check(database.Write(writes, &batch));
                                    batch.Clear();
                                };
                                for (auto begin = next.fetch_add(chunk); begin < spec.dataset.records; begin = next.fetch_add(chunk))
                                {
                                    const auto end = std::min(spec.dataset.records, begin + chunk);
                                    for (auto index = begin; index < end; ++index)
                                    {
                                        check(batch.Put(record_key(spec.dataset, index), record_value(spec.dataset, index, generation)));
                                        if (batch.GetDataSize() >= batch_bytes)
                                            flush();
                                    }
                                }
                                if (batch.Count() != 0)
                                    flush();
                            }
                            catch (...) { failures[producer] = std::current_exception(); } });
                    }
                    for (auto &thread : threads)
                        thread.join();
                    for (const auto &failure : failures)
                    {
                        if (failure)
                            std::rethrow_exception(failure);
                    }
                }
                rocksdb::FlushOptions flush;
                flush.wait = true;
                check(database.Flush(flush));
            }
            const auto loaded = SteadyClock::now();
            rocksdb::WaitForCompactOptions drain;
            drain.wait_for_purge = true;
            // Large bulk datasets can need far longer than the default 60 s to settle.
            drain.timeout = bulk ? std::chrono::microseconds(0) : std::chrono::seconds(60);
            check(database.WaitForCompact(drain));
            const auto compacted = SteadyClock::now();
            const auto seconds = [](auto duration)
            { return std::chrono::duration<double>(duration).count(); };
            const auto total = seconds(compacted - started);
            const auto records = spec.dataset.records * (spec.dataset.overwrite_rounds + 1ULL);
            const auto bytes = logical_write_bytes(spec.dataset);
            return {{"load_mode", spec.dataset.load_mode}, {"producer_threads", producers}, {"batch_bytes", batch_bytes},
                    {"wal", !bulk}, {"records_written", records}, {"logical_bytes_written", bytes},
                    {"load_seconds", seconds(loaded - started)}, {"compaction_wait_seconds", seconds(compacted - loaded)},
                    {"total_seconds", total}, {"records_per_second", total > 0 ? records / total : 0.0},
                    {"logical_mib_per_second", total > 0 ? bytes / total / 1048576.0 : 0.0}};
        }
    }

    std::string record_key(const DatasetSpec &spec, std::uint64_t index)
    {
        require(index < spec.records, "invalid key generator arguments");
        return generated_key(spec, index);
    }

    std::string record_value(const DatasetSpec &spec, std::uint64_t index, std::uint64_t generation)
    {
        require(index < spec.records, "invalid value generator arguments");
        return generated_value(spec, index, generation);
    }

    std::string generated_key(const DatasetSpec &spec, std::uint64_t index)
    {
        require(spec.key_bytes >= 16 && spec.key_bytes <= 1024 && index < maximum_key_index,
                "invalid key generator arguments");
        std::string key(spec.key_bytes, '\0');
        encode64(key, 0, mix64(index ^ spec.seed));
        encode64(key, 8, index);
        for (std::size_t offset = 16; offset < key.size(); ++offset)
        {
            key[offset] = static_cast<char>(mix64(spec.seed ^ index ^ offset) & 255);
        }
        return key;
    }

    std::string generated_value(const DatasetSpec &spec, std::uint64_t index, std::uint64_t generation)
    {
        require(spec.value_bytes >= 1 && spec.value_bytes <= 1ULL << 20 && index < maximum_key_index,
                "invalid value generator arguments");
        std::uint64_t counter = mix64(spec.seed) ^ mix64(index + 0x6a09e667f3bcc909ULL) ^
                                mix64(generation + 0xbb67ae8584caa73bULL);
        std::string value(spec.value_bytes, '\0');
        std::string pattern(16, '\0');
        encode64(pattern, 0, mix64(counter++));
        encode64(pattern, 8, mix64(counter++));
        std::uint64_t word = 0;
        for (std::size_t offset = 0; offset < value.size() / 2; ++offset)
        {
            if (offset % 8 == 0)
            {
                word = mix64(counter++);
            }
            value[offset] = static_cast<char>((word >> ((offset % 8) * 8)) & 255);
        }
        for (std::size_t offset = value.size() / 2; offset < value.size(); ++offset)
        {
            value[offset] = pattern[(offset - value.size() / 2) % pattern.size()];
        }
        return value;
    }

    rocksdb::Options engine_options(const Spec &spec, std::shared_ptr<rocksdb::CompactionBlockTransform> transform)
    {
        validate(spec);
        const auto &supported = rocksdb::GetSupportedCompressions();
        if (spec.engine.compression == "lz4")
        {
            require(std::find(supported.begin(), supported.end(), rocksdb::kLZ4Compression) != supported.end(),
                    "this RocksDB build does not support LZ4");
        }
        if (spec.engine.compression == "zstd")
        {
            require(std::find(supported.begin(), supported.end(), rocksdb::kZSTD) != supported.end(),
                    "this RocksDB build does not support ZSTD");
        }
        rocksdb::Options options;
        options.compression = spec.engine.compression == "lz4"    ? rocksdb::kLZ4Compression
                              : spec.engine.compression == "zstd" ? rocksdb::kZSTD
                                                                  : rocksdb::kNoCompression;
        options.write_buffer_size = spec.engine.write_buffer_bytes;
        options.target_file_size_base = spec.engine.target_file_bytes;
        options.max_bytes_for_level_base = spec.engine.max_bytes_for_level_base.value_or(spec.engine.target_file_bytes * 4);
        options.max_write_buffer_number = static_cast<int>(spec.engine.max_write_buffer_number);
        options.max_subcompactions = spec.engine.max_subcompactions;
        if (spec.engine.level0_file_num_compaction_trigger)
            options.level0_file_num_compaction_trigger = static_cast<int>(*spec.engine.level0_file_num_compaction_trigger);
        if (spec.engine.level0_slowdown_writes_trigger)
            options.level0_slowdown_writes_trigger = static_cast<int>(*spec.engine.level0_slowdown_writes_trigger);
        if (spec.engine.level0_stop_writes_trigger)
            options.level0_stop_writes_trigger = static_cast<int>(*spec.engine.level0_stop_writes_trigger);
        if (spec.engine.use_direct_io)
        {
            options.use_direct_reads = true;
            options.use_direct_io_for_flush_and_compaction = true;
        }
        options.max_background_jobs = static_cast<int>(spec.engine.background_jobs);
        options.num_levels = static_cast<int>(spec.engine.levels);
        options.paranoid_checks = true;
        options.paranoid_file_checks = spec.engine.verify_output;
        rocksdb::BlockBasedTableOptions table;
        table.format_version = 6;
        table.checksum = rocksdb::kCRC32c;
        table.compaction_block_transform = std::move(transform);
        table.block_size = spec.engine.block_bytes;
        table.block_cache = rocksdb::NewLRUCache(spec.engine.block_cache_bytes);
        table.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10));
        table.verify_compression = spec.engine.verify_output;
        options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table));
        return options;
    }

    Json create_dataset(const Spec &spec, const fs::path &destination)
    {
        validate(spec);
        const auto root = reserve_directory(destination, spec);
        auto options = engine_options(spec);
        options.create_if_missing = true;
        options.error_if_exists = true;
        if (spec.dataset.load_mode != "sync")
        {
            // Creation-only tuning: more background parallelism changes timing, never the logical content.
            const auto hardware = static_cast<int>(std::min(16U, std::max(1U, std::thread::hardware_concurrency())));
            options.max_background_jobs = std::max(options.max_background_jobs, hardware);
            options.max_subcompactions = std::max<std::uint32_t>(options.max_subcompactions, static_cast<std::uint32_t>(std::max(1, hardware / 2)));
            options.max_write_batch_group_size_bytes = 64ULL << 20;
        }
        std::unique_ptr<rocksdb::DB> database;
        check(rocksdb::DB::Open(options, (root / "db").string(), &database));
        auto creation = load_records(*database, spec, root);
        const auto verify_started = SteadyClock::now();
        check(database->VerifyChecksum());
        check(database->Close());
        database.reset();
        sync_directory(root / "db");
        auto manifest = seal(root, spec);
        creation["checksum_verify_seconds"] = std::chrono::duration<double>(SteadyClock::now() - verify_started).count();
        manifest["creation"] = creation;
        return manifest;
    }

    Json verify_dataset(const fs::path &directory)
    {
        const auto manifest = read_manifest(directory);
        OfflineLock lock(directory);
        return verify_locked(directory, manifest);
    }

    Spec dataset_spec(const fs::path &directory)
    {
        return parse_spec(read_manifest(directory).at("spec").dump());
    }

    Json verify_trial(const fs::path &directory, const fs::path &load_output)
    {
        const auto document = [](const fs::path &path)
        {
            require(fs::is_regular_file(fs::symlink_status(path)) && fs::file_size(path) <= 1ULL << 20,
                    "invalid trial metadata file: " + path.string());
            std::ifstream input(path);
            return Json::parse(input);
        };
        const auto manifest = read_manifest(directory);
        const auto run = document(directory / "run.json");
        const auto load = document(load_output / "manifest.json");
        const auto result = document(load_output / "result.json");
        require(run.at("owner") == "lsm-lab" && run.at("state") == "stopped" && load.at("owner") == "lsm-lab" &&
                    load.at("state") == "complete" && result.at("state") == "complete" &&
                    run.at("run_id") == load.at("run_id") && load.at("run_id") == result.at("run_id") &&
                    load.at("dataset_id") == manifest.at("dataset_id") && load.at("spec") == manifest.at("spec"),
                "trial identity mismatch or trial has not stopped cleanly");
        require(result.at("warmup").at("indeterminate_writes") == 0 && result.at("measurement").at("indeterminate_writes") == 0,
                "cannot prove expected state after indeterminate writes");
        const auto spec = parse_spec(manifest.at("spec").dump());
        const auto budget = load.at("memory_budget_bytes").get<std::uint64_t>();
        require(budget <= 64ULL << 30, "invalid verification memory budget");
        // Loads that insert new keys declare the exclusive upper bound of the key indices they may create.
        const auto key_index_limit = load.contains("key_index_limit") ? load.at("key_index_limit").get<std::uint64_t>() : spec.dataset.records;
        require(key_index_limit >= spec.dataset.records && key_index_limit <= maximum_key_index, "invalid trial key index limit");
        std::map<std::uint64_t, std::uint64_t> versions;
        std::ifstream input(load_output / "final-versions.jsonl");
        require(input.good(), "missing expected final versions");
        std::array<char, 256> line{};
        while (input.getline(line.data(), line.size()))
        {
            const auto entry = Json::parse(line.data());
            require(entry.at("key_index").is_number_unsigned() && entry.at("generation").is_number_unsigned(), "invalid version entry");
            const auto index = entry.at("key_index").get<std::uint64_t>();
            const auto generation = entry.at("generation").get<std::uint64_t>();
            require(index < key_index_limit && generation > spec.dataset.overwrite_rounds && versions.size() < budget / 192 &&
                        versions.emplace(index, generation).second,
                    "duplicate, out-of-range, or over-budget version entry");
        }
        require(input.eof() && !input.bad(), "malformed expected-version stream");
        OfflineLock lock(directory);
        auto report = verify_locked(directory, manifest, versions, false);
        report["run_id"] = run.at("run_id");
        report["modified_keys_verified"] = versions.size();
        report["key_index_limit"] = key_index_limit;
        return report;
    }

    Json initialize_shared_root(const fs::path &directory)
    {
        const auto absolute = fs::absolute(directory).lexically_normal();
        const auto parent = fs::canonical(absolute.parent_path());
        require(!absolute.filename().empty(), "shared root must name a new directory");
        disk_budget(parent, 1ULL << 20);
        const auto root = parent / absolute.filename();
        require(fs::create_directory(root), "shared root already exists");
        require(fs::create_directory(root / ".lsm-staging"), "cannot create shared staging directory");
        Json marker{{"schema_version", 1}, {"owner", "lsm-lab"}, {"shared_root_id", rocksdb::Env::Default()->GenerateUniqueId()}};
        publish_manifest(root, marker, ".lsm-shared.json");
        sync_directory(parent);
        return marker;
    }

    Json clone_dataset(const fs::path &source, const fs::path &destination)
    {
        const auto manifest = read_manifest(source);
        OfflineLock lock(source);
        verify_locked(source, manifest);
        const auto spec = parse_spec(manifest.at("spec").dump());
        const auto root = reserve_directory(destination, spec);
        require(fs::create_directory(root / "db"), "cannot create clone db directory");
        auto *env = rocksdb::Env::Default();
        for (const auto &entry : manifest.at("files"))
        {
            const auto name = entry.at("name").get<std::string>();
            const auto bytes = entry.at("bytes").get<std::uint64_t>();
            disk_budget(root, spec.limits.minimum_free_disk_bytes, bytes);
            fs::copy_file(source / "db" / name, root / "db" / name, fs::copy_options::none);
            check(env->SyncFile((root / "db" / name).string(), rocksdb::EnvOptions(), true));
        }
        sync_directory(root / "db");
        require(inventory(root / "db") == manifest.at("files"), "clone file checksum mismatch");
        return seal(root, spec, manifest.at("dataset_id").get<std::string>());
    }
}