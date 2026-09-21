#include "lsm_lab/dataset.hpp"

#include <fstream>
#include <set>
#include <unistd.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <rocksdb/db.h>

namespace lsm_lab
{
    namespace
    {
        class DatasetTest : public ::testing::Test
        {
        protected:
            void SetUp() override
            {
                auto pattern = (std::filesystem::temp_directory_path() / "lsm-dataset-XXXXXX").string();
                std::vector<char> buffer(pattern.begin(), pattern.end());
                buffer.push_back('\0');
                const auto *created = ::mkdtemp(buffer.data());
                ASSERT_NE(created, nullptr);
                root = created;
                spec.dataset.records = 513;
                spec.dataset.value_bytes = 257;
                spec.dataset.overwrite_rounds = 2;
                spec.engine.write_buffer_bytes = 65536;
                spec.engine.target_file_bytes = 65536;
                spec.limits.minimum_free_disk_bytes = 1;
            }

            void TearDown() override
            {
                if (!root.empty())
                {
                    std::error_code error;
                    std::filesystem::remove_all(root, error);
                }
            }

            std::filesystem::path root;
            Spec spec;
        };

        TEST_F(DatasetTest, GeneratorIsStableUniqueAndFullWidth)
        {
            const auto key = record_key(spec.dataset, 0);
            const std::string golden("\x91\x0a\x2d\xec\x89\x02\x5c\xc1\0\0\0\0\0\0\0\0", 16);
            EXPECT_EQ(key, golden);
            std::set<std::string> keys;
            for (std::uint64_t index = 0; index < spec.dataset.records; ++index)
            {
                keys.insert(record_key(spec.dataset, index));
            }
            EXPECT_EQ(keys.size(), spec.dataset.records);
            const auto first = record_value(spec.dataset, 1, 0);
            const auto second = record_value(spec.dataset, 1, 1);
            EXPECT_EQ(first.size(), 257U);
            EXPECT_EQ(first, record_value(spec.dataset, 1, 0));
            EXPECT_NE(first.substr(128), second.substr(128));
            EXPECT_EQ(first.substr(128, 16), first.substr(144, 16));
        }

        TEST_F(DatasetTest, CreatesAndVerifiesRealSsts)
        {
            const auto destination = root / "baseline";
            const auto manifest = create_dataset(spec, destination);
            EXPECT_EQ(manifest.at("state"), "ready");
            EXPECT_EQ(dataset_spec(destination), spec);
            EXPECT_FALSE(manifest.at("files").empty());
            const auto report = verify_dataset(destination);
            EXPECT_EQ(report.at("records_verified"), spec.dataset.records);
            EXPECT_EQ(report.at("full_payloads_verified"), true);
            bool has_sst = false;
            for (const auto &file : manifest.at("files"))
            {
                has_sst |= std::filesystem::path(file.at("name").get<std::string>()).extension() == ".sst";
            }
            EXPECT_TRUE(has_sst);
        }

        TEST_F(DatasetTest, CloneIsIndependentAndRefusesOverwrite)
        {
            const auto source = root / "baseline";
            const auto destination = root / "trial";
            const auto manifest = create_dataset(spec, source);
            const auto clone = clone_dataset(source, destination);
            EXPECT_EQ(clone.at("parent_id"), manifest.at("dataset_id"));
            EXPECT_EQ(clone.at("files"), manifest.at("files"));
            EXPECT_NO_THROW(verify_dataset(destination));
            EXPECT_THROW(clone_dataset(source, destination), std::runtime_error);
            auto options = engine_options(spec);
            std::unique_ptr<rocksdb::DB> database;
            ASSERT_TRUE(rocksdb::DB::Open(options, (destination / "db").string(), &database).ok());
            rocksdb::WriteOptions writes;
            writes.sync = true;
            ASSERT_TRUE(database->Put(writes, record_key(spec.dataset, 0), "changed").ok());
            ASSERT_TRUE(database->Close().ok());
            database.reset();
            EXPECT_THROW(verify_dataset(destination), std::runtime_error);
            EXPECT_NO_THROW(verify_dataset(source));
        }

        TEST_F(DatasetTest, RefusesExistingDirectoryAndActiveDatabase)
        {
            std::ofstream(root / "unrelated") << "preserve";
            EXPECT_THROW(create_dataset(spec, root), std::runtime_error);
            EXPECT_TRUE(std::filesystem::exists(root / "unrelated"));
            const auto destination = root / "baseline";
            create_dataset(spec, destination);
            std::unique_ptr<rocksdb::DB> database;
            ASSERT_TRUE(rocksdb::DB::Open(engine_options(spec), (destination / "db").string(), &database).ok());
            EXPECT_THROW(verify_dataset(destination), std::runtime_error);
            EXPECT_THROW(clone_dataset(destination, root / "copy"), std::runtime_error);
            ASSERT_TRUE(database->Close().ok());
        }

        TEST_F(DatasetTest, DetectsCorruptedFileAndInsufficientBudget)
        {
            const auto destination = root / "baseline";
            const auto manifest = create_dataset(spec, destination);
            const auto file = destination / "db" / manifest.at("files").front().at("name").get<std::string>();
            std::ofstream(file, std::ios::binary | std::ios::app).put('x');
            EXPECT_THROW(verify_dataset(destination), std::runtime_error);
            spec.limits.logical_write_bytes = 1;
            EXPECT_THROW(create_dataset(spec, root / "rejected"), std::invalid_argument);
            EXPECT_FALSE(std::filesystem::exists(root / "rejected"));
        }

        TEST_F(DatasetTest, SupportsUncompressedAndOddSizedValues)
        {
            spec.engine.compression = "none";
            spec.dataset.value_bytes = 13;
            spec.dataset.overwrite_rounds = 0;
            create_dataset(spec, root / "plain");
            EXPECT_NO_THROW(verify_dataset(root / "plain"));
        }
    }
}