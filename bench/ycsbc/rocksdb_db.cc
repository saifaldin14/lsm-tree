// RocksDB (native or with GPU compaction offload) behind gParaKV's YCSB driver (ycsbc), so the
// head-to-head comparison uses one workload generator and one measurement for every system.
// Settings come from the environment because ycsbc only passes its own flags:
//   LSM_YCSB_MODE             native_cpu (default), gpu_output, cuda_merge or cpu_merge
//   LSM_YCSB_COMPRESSION      none (default, as gParaKV's driver), lz4 or zstd
//   LSM_YCSB_BACKGROUND_JOBS  default 4
//   LSM_YCSB_BLOB_MIN_BYTES   RocksDB BlobDB key/value separation threshold (0 = off)
// Other options match lsm_system_bench.

#include "db/db_factory.h"

#include <lsm_lab/block_transform.hpp>

#include <rocksdb/cache.h>
#include <rocksdb/db.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/table.h>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace
{
    std::string setting(const char *name, const char *fallback)
    {
        const char *value = std::getenv(name);
        return value != nullptr && *value != '\0' ? value : fallback;
    }

    class RocksDb final : public ycsbc::DB
    {
    public:
        explicit RocksDb(const std::string &path)
        {
            const auto mode = setting("LSM_YCSB_MODE", "native_cpu");
            if (mode != "native_cpu")
                transform_ = lsm_lab::make_transform(mode == "gpu_output" ? "cuda_hybrid" : mode);
            rocksdb::Options options;
            options.create_if_missing = true;
            options.write_buffer_size = 64ULL << 20;
            options.max_write_buffer_number = 4;
            options.target_file_size_base = 64ULL << 20;
            options.max_bytes_for_level_base = 256ULL << 20;
            options.max_background_jobs = std::stoi(setting("LSM_YCSB_BACKGROUND_JOBS", "4"));
            options.paranoid_checks = true;
            const auto compression = setting("LSM_YCSB_COMPRESSION", "none");
            options.compression = compression == "lz4"    ? rocksdb::kLZ4Compression
                                  : compression == "zstd" ? rocksdb::kZSTD
                                                          : rocksdb::kNoCompression;
            const auto blob = std::stoul(setting("LSM_YCSB_BLOB_MIN_BYTES", "0"));
            if (blob > 0)
            {
                options.enable_blob_files = true;
                options.min_blob_size = blob;
                options.enable_blob_garbage_collection = true;
            }
            rocksdb::BlockBasedTableOptions table;
            table.format_version = 6;
            table.checksum = rocksdb::kCRC32c;
            table.block_size = 4096;
            table.block_cache = rocksdb::NewLRUCache(256ULL << 20);
            table.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10));
            table.compaction_block_transform = transform_;
            options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table));
            const auto status = rocksdb::DB::Open(options, path, &db_);
            if (!status.ok())
            {
                std::cerr << "can't open rocksdb: " << status.ToString() << std::endl;
                std::exit(1);
            }
            std::cerr << "lsm_ycsbc: mode=" << mode << " compression=" << compression << " background_jobs="
                      << options.max_background_jobs << " blob_min_bytes=" << blob << std::endl;
        }

        int Read(const std::string &, const std::string &key, const std::vector<std::string> *,
                 std::vector<KVPair> &) override
        {
            std::string value;
            const auto status = db_->Get(rocksdb::ReadOptions(), key, &value);
            if (status.IsNotFound())
                ++not_found_;
            else
                check(status, "read");
            return kOK;
        }

        int Scan(const std::string &, const std::string &key, int count, const std::vector<std::string> *,
                 std::vector<std::vector<KVPair>> &) override
        {
            std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(rocksdb::ReadOptions()));
            it->Seek(key);
            for (int index = 0; index < count && it->Valid(); ++index)
                it->Next();
            return kOK;
        }

        int Update(const std::string &table, const std::string &key, std::vector<KVPair> &values) override
        {
            return Insert(table, key, values);
        }

        int Insert(const std::string &, const std::string &key, std::vector<KVPair> &values) override
        {
            for (const auto &value : values)
                check(db_->Put(rocksdb::WriteOptions(), key, value.second), "insert");
            return kOK;
        }

        int Delete(const std::string &, const std::string &key) override
        {
            check(db_->Delete(rocksdb::WriteOptions(), key), "delete");
            return kOK;
        }

        void PrintMyStats() override
        {
            if (not_found_ != 0)
                std::cerr << "read not found: " << not_found_ << std::endl;
            std::string stats;
            if (db_->GetProperty("rocksdb.stats", &stats))
                std::cerr << stats << std::endl;
            if (transform_)
                std::cerr << "lsm_transform: " << transform_->statistics().dump() << std::endl;
        }

    private:
        static void check(const rocksdb::Status &status, const char *what)
        {
            if (!status.ok())
            {
                std::cerr << what << " error: " << status.ToString() << std::endl;
                std::exit(1);
            }
        }

        std::shared_ptr<lsm_lab::BatchTransform> transform_;
        std::unique_ptr<rocksdb::DB> db_;
        std::uint64_t not_found_ = 0;
    };
}

ycsbc::DB *ycsbc::DBFactory::CreateDB(utils::Properties &props)
{
    return new RocksDb(props["dbfilename"]);
}
