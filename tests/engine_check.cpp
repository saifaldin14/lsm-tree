#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

#include <rocksdb/convenience.h>
#include <rocksdb/db.h>
#include <rocksdb/table.h>

namespace
{

    void require(bool condition, const std::string &message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    void check(const rocksdb::Status &status)
    {
        if (!status.ok())
        {
            throw std::runtime_error(status.ToString());
        }
    }

    class ScratchDirectory
    {
    public:
        ScratchDirectory()
        {
            auto pattern = (std::filesystem::temp_directory_path() / "lsm-engine-XXXXXX").string();
            std::vector<char> buffer(pattern.begin(), pattern.end());
            buffer.push_back('\0');
            auto *created = ::mkdtemp(buffer.data());
            require(created != nullptr, "cannot create test directory");
            path = created;
        }
        ~ScratchDirectory()
        {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
        std::filesystem::path path;
    };

}

int main()
{
    try
    {
        const auto &codecs = rocksdb::GetSupportedCompressions();
        require(std::find(codecs.begin(), codecs.end(), rocksdb::kLZ4Compression) != codecs.end(),
                "RocksDB must have real LZ4 support");
        ScratchDirectory scratch;
        rocksdb::Options options;
        options.create_if_missing = true;
        options.error_if_exists = true;
        options.compression = rocksdb::kLZ4Compression;
        options.disable_auto_compactions = true;
        options.write_buffer_size = 1 << 20;
        rocksdb::BlockBasedTableOptions table;
        table.format_version = 6;
        table.verify_compression = true;
        options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table));
        std::unique_ptr<rocksdb::DB> database;
        check(rocksdb::DB::Open(options, scratch.path.string(), &database));
        rocksdb::WriteOptions writes;
        writes.sync = true;
        const std::string original(257, 'a');
        std::string replacement(257, 'b');
        replacement[200] = '\0';
        check(database->Put(writes, "key", original));
        check(database->Put(writes, "deleted", original));
        check(database->Flush(rocksdb::FlushOptions()));
        const auto *snapshot = database->GetSnapshot();
        check(database->Put(writes, "key", replacement));
        check(database->Delete(writes, "deleted"));
        check(database->Flush(rocksdb::FlushOptions()));
        check(database->CompactRange(rocksdb::CompactRangeOptions(), nullptr, nullptr));
        std::string value;
        rocksdb::ReadOptions historical;
        historical.snapshot = snapshot;
        check(database->Get(historical, "key", &value));
        require(value == original, "snapshot payload changed during compaction");
        check(database->Get(historical, "deleted", &value));
        require(value == original, "snapshot deletion retention failed");
        database->ReleaseSnapshot(snapshot);
        check(database->VerifyChecksum());
        check(database->Close());
        database.reset();
        options.create_if_missing = false;
        options.error_if_exists = false;
        check(rocksdb::DB::Open(options, scratch.path.string(), &database));
        check(database->Get(rocksdb::ReadOptions(), "key", &value));
        require(value == replacement, "full payload did not survive reopen");
        require(database->Get(rocksdb::ReadOptions(), "deleted", &value).IsNotFound(),
                "deleted value reappeared after reopen");
        check(database->VerifyChecksum());
        check(database->Close());
        std::cout << "PASS: full values, LZ4, snapshot retention, compaction, checksums, reopen\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}