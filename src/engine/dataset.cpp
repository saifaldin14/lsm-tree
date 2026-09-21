#include "lsm_lab/dataset.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <rocksdb/cache.h>
#include <rocksdb/convenience.h>
#include <rocksdb/db.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/table.h>
#include <rocksdb/version.h>
#include <rocksdb/write_batch.h>

namespace lsm_lab
{
    namespace
    {
        namespace fs = std::filesystem;
        using Json = nlohmann::json;

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

        void publish_manifest(const fs::path &root, const Json &manifest)
        {
            auto *env = rocksdb::Env::Default();
            const auto temporary = root / (".dataset-" + env->GenerateUniqueId());
            std::unique_ptr<rocksdb::WritableFile> file;
            check(env->NewWritableFile(temporary.string(), &file, rocksdb::EnvOptions()));
            check(file->Append(manifest.dump(2) + "\n"));
            check(file->Fsync());
            check(file->Close());
            file.reset();
            check(env->RenameFile(temporary.string(), (root / "dataset.json").string()));
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

        Json verify_locked(const fs::path &root, const Json &manifest)
        {
            require(manifest.at("files") == inventory(root / "db"), "dataset file inventory or SHA-256 mismatch");
            const auto spec = parse_spec(manifest.at("spec").dump());
            auto options = engine_options(spec);
            std::unique_ptr<rocksdb::DB> database;
            check(rocksdb::DB::OpenForReadOnly(options, (root / "db").string(), &database, true));
            check(database->VerifyChecksum());
            std::uint64_t count = 0;
            {
                std::unique_ptr<rocksdb::Iterator> iterator(database->NewIterator(rocksdb::ReadOptions()));
                for (iterator->SeekToFirst(); iterator->Valid(); iterator->Next())
                {
                    const auto index = decode_index(iterator->key());
                    require(index < spec.dataset.records && iterator->key() == record_key(spec.dataset, index),
                            "dataset key does not match generator");
                    require(iterator->value() == record_value(spec.dataset, index, spec.dataset.overwrite_rounds),
                            "dataset full value does not match generator");
                    ++count;
                }
                check(iterator->status());
            }
            require(count == spec.dataset.records, "dataset record count mismatch");
            check(database->Close());
            return {{"status", "ok"}, {"dataset_id", manifest.at("dataset_id")}, {"records_verified", count}, {"files_verified", manifest.at("files").size()}, {"full_payloads_verified", true}, {"file_hashes_verified", true}};
        }
    }

    std::string record_key(const DatasetSpec &spec, std::uint64_t index)
    {
        require(spec.key_bytes >= 16 && spec.key_bytes <= 1024 && index < spec.records,
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

    std::string record_value(const DatasetSpec &spec, std::uint64_t index, std::uint64_t generation)
    {
        require(spec.value_bytes >= 1 && spec.value_bytes <= 1ULL << 20 && index < spec.records,
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

    rocksdb::Options engine_options(const Spec &spec)
    {
        validate(spec);
        if (spec.engine.compression == "lz4")
        {
            const auto &supported = rocksdb::GetSupportedCompressions();
            require(std::find(supported.begin(), supported.end(), rocksdb::kLZ4Compression) != supported.end(),
                    "this RocksDB build does not support LZ4");
        }
        rocksdb::Options options;
        options.compression = spec.engine.compression == "lz4" ? rocksdb::kLZ4Compression : rocksdb::kNoCompression;
        options.write_buffer_size = spec.engine.write_buffer_bytes;
        options.target_file_size_base = spec.engine.target_file_bytes;
        options.max_bytes_for_level_base = spec.engine.target_file_bytes * 4;
        options.max_write_buffer_number = 4;
        options.max_background_jobs = static_cast<int>(spec.engine.background_jobs);
        options.num_levels = static_cast<int>(spec.engine.levels);
        options.paranoid_checks = true;
        options.paranoid_file_checks = true;
        rocksdb::BlockBasedTableOptions table;
        table.format_version = 6;
        table.block_size = spec.engine.block_bytes;
        table.block_cache = rocksdb::NewLRUCache(spec.engine.block_cache_bytes);
        table.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10));
        table.verify_compression = true;
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
        std::unique_ptr<rocksdb::DB> database;
        check(rocksdb::DB::Open(options, (root / "db").string(), &database));
        rocksdb::WriteOptions writes;
        writes.sync = true;
        for (std::uint64_t generation = 0; generation <= spec.dataset.overwrite_rounds; ++generation)
        {
            rocksdb::WriteBatch batch;
            for (std::uint64_t index = 0; index < spec.dataset.records; ++index)
            {
                check(batch.Put(record_key(spec.dataset, index), record_value(spec.dataset, index, generation)));
                if (batch.GetDataSize() >= 256 * 1024 || index + 1 == spec.dataset.records)
                {
                    disk_budget(root, spec.limits.minimum_free_disk_bytes, batch.GetDataSize());
                    check(database->Write(writes, &batch));
                    batch.Clear();
                }
            }
            check(database->Flush(rocksdb::FlushOptions()));
        }
        rocksdb::WaitForCompactOptions drain;
        drain.wait_for_purge = true;
        drain.timeout = std::chrono::seconds(60);
        check(database->WaitForCompact(drain));
        check(database->VerifyChecksum());
        check(database->Close());
        database.reset();
        sync_directory(root / "db");
        return seal(root, spec);
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