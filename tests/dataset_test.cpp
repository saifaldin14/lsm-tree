#include "lsm_lab/dataset.hpp"

#include <fstream>
#include <set>
#include <unistd.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <rocksdb/convenience.h>
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

        std::vector<std::pair<std::string, std::string>> contents(const std::filesystem::path &dataset, const Spec &spec)
        {
            std::unique_ptr<rocksdb::DB> database;
            EXPECT_TRUE(rocksdb::DB::OpenForReadOnly(engine_options(spec), (dataset / "db").string(), &database).ok());
            std::vector<std::pair<std::string, std::string>> result;
            std::unique_ptr<rocksdb::Iterator> iterator(database->NewIterator(rocksdb::ReadOptions()));
            for (iterator->SeekToFirst(); iterator->Valid(); iterator->Next())
                result.emplace_back(iterator->key().ToString(), iterator->value().ToString());
            EXPECT_TRUE(iterator->status().ok());
            iterator.reset();
            EXPECT_TRUE(database->Close().ok());
            return result;
        }

        TEST_F(DatasetTest, DefaultEngineOptionsAreUnchanged)
        {
            const rocksdb::Options defaults;
            const auto options = engine_options(Spec{});
            EXPECT_EQ(options.compression, rocksdb::kLZ4Compression);
            EXPECT_EQ(options.write_buffer_size, 1ULL << 20);
            EXPECT_EQ(options.target_file_size_base, 1ULL << 20);
            EXPECT_EQ(options.max_bytes_for_level_base, 4ULL << 20);
            EXPECT_EQ(options.max_write_buffer_number, 4);
            EXPECT_EQ(options.max_subcompactions, defaults.max_subcompactions);
            EXPECT_EQ(options.max_subcompactions, 1U);
            EXPECT_EQ(options.level0_file_num_compaction_trigger, defaults.level0_file_num_compaction_trigger);
            EXPECT_EQ(options.level0_slowdown_writes_trigger, defaults.level0_slowdown_writes_trigger);
            EXPECT_EQ(options.level0_stop_writes_trigger, defaults.level0_stop_writes_trigger);
            EXPECT_FALSE(options.use_direct_reads);
            EXPECT_FALSE(options.use_direct_io_for_flush_and_compaction);
            EXPECT_EQ(options.max_background_jobs, 2);
            EXPECT_EQ(options.num_levels, 4);
            // The spec's unset trigger defaults used for validation must match this RocksDB build.
            EXPECT_EQ(defaults.level0_file_num_compaction_trigger, 4);
            EXPECT_EQ(defaults.level0_slowdown_writes_trigger, 20);
            EXPECT_EQ(defaults.level0_stop_writes_trigger, 36);
        }

        TEST_F(DatasetTest, ConfiguredEngineOptionsAreApplied)
        {
            spec.engine.compression = "zstd";
            spec.engine.max_subcompactions = 4;
            spec.engine.max_bytes_for_level_base = 8ULL << 20;
            spec.engine.max_write_buffer_number = 6;
            spec.engine.level0_file_num_compaction_trigger = 2;
            spec.engine.level0_slowdown_writes_trigger = 30;
            spec.engine.level0_stop_writes_trigger = 50;
            spec.engine.use_direct_io = true;
            const auto options = engine_options(spec);
            EXPECT_EQ(options.compression, rocksdb::kZSTD);
            EXPECT_EQ(options.max_subcompactions, 4U);
            EXPECT_EQ(options.max_bytes_for_level_base, 8ULL << 20);
            EXPECT_EQ(options.max_write_buffer_number, 6);
            EXPECT_EQ(options.level0_file_num_compaction_trigger, 2);
            EXPECT_EQ(options.level0_slowdown_writes_trigger, 30);
            EXPECT_EQ(options.level0_stop_writes_trigger, 50);
            EXPECT_TRUE(options.use_direct_reads);
            EXPECT_TRUE(options.use_direct_io_for_flush_and_compaction);
            spec.engine.use_direct_io = false;
            spec.engine.max_bytes_for_level_base.reset();
            EXPECT_EQ(engine_options(spec).max_bytes_for_level_base, spec.engine.target_file_bytes * 4);
        }

        TEST_F(DatasetTest, ZstdDatasetCreatesVerifiesAndReopens)
        {
            const auto &supported = rocksdb::GetSupportedCompressions();
            ASSERT_NE(std::find(supported.begin(), supported.end(), rocksdb::kZSTD), supported.end());
            spec.engine.compression = "zstd";
            const auto destination = root / "zstd";
            const auto manifest = create_dataset(spec, destination);
            EXPECT_EQ(manifest.at("spec").at("engine").at("compression"), "zstd");
            EXPECT_EQ(verify_dataset(destination).at("records_verified"), spec.dataset.records);
            const auto clone = root / "zstd-clone";
            EXPECT_NO_THROW(clone_dataset(destination, clone));
            std::unique_ptr<rocksdb::DB> database;
            ASSERT_TRUE(rocksdb::DB::Open(engine_options(spec), (clone / "db").string(), &database).ok());
            std::string value;
            ASSERT_TRUE(database->Get(rocksdb::ReadOptions(), record_key(spec.dataset, 7), &value).ok());
            EXPECT_EQ(value, record_value(spec.dataset, 7, spec.dataset.overwrite_rounds));
            rocksdb::TablePropertiesCollection tables;
            ASSERT_TRUE(database->GetPropertiesOfAllTables(&tables).ok());
            ASSERT_FALSE(tables.empty());
            for (const auto &[name, properties] : tables)
                EXPECT_EQ(properties->compression_name, "ZSTD") << name;
            rocksdb::WriteOptions writes;
            writes.sync = true;
            ASSERT_TRUE(database->Put(writes, record_key(spec.dataset, 7), record_value(spec.dataset, 7, 0)).ok());
            ASSERT_TRUE(database->Flush(rocksdb::FlushOptions()).ok());
            ASSERT_TRUE(database->Close().ok());
            database.reset();
            ASSERT_TRUE(rocksdb::DB::OpenForReadOnly(engine_options(spec), (clone / "db").string(), &database).ok());
            ASSERT_TRUE(database->Get(rocksdb::ReadOptions(), record_key(spec.dataset, 7), &value).ok());
            EXPECT_EQ(value, record_value(spec.dataset, 7, 0));
            ASSERT_TRUE(database->VerifyChecksum().ok());
            ASSERT_TRUE(database->Close().ok());
            EXPECT_NO_THROW(verify_dataset(destination));
        }

        TEST_F(DatasetTest, BulkLoadMatchesSynchronousContent)
        {
            create_dataset(spec, root / "sync");
            auto bulk = spec;
            bulk.dataset.load_mode = "bulk";
            const auto manifest = create_dataset(bulk, root / "bulk");
            EXPECT_EQ(manifest.at("spec").at("dataset").at("load_mode"), "bulk");
            const auto &creation = manifest.at("creation");
            EXPECT_EQ(creation.at("load_mode"), "bulk");
            EXPECT_EQ(creation.at("wal"), false);
            EXPECT_EQ(creation.at("records_written"), spec.dataset.records * (spec.dataset.overwrite_rounds + 1));
            EXPECT_GT(creation.at("records_per_second").get<double>(), 0.0);
            // The published manifest omits the creation report so it stays identical in shape to synchronous datasets.
            const auto published = nlohmann::json::parse(std::ifstream(root / "bulk" / "dataset.json"));
            EXPECT_FALSE(published.contains("creation"));
            const auto report = verify_dataset(root / "bulk");
            EXPECT_EQ(report.at("records_verified"), spec.dataset.records);
            EXPECT_EQ(report.at("full_payloads_verified"), true);
            EXPECT_EQ(contents(root / "bulk", bulk), contents(root / "sync", spec));
            bool has_wal = false;
            for (const auto &file : published.at("files"))
                has_wal |= std::filesystem::path(file.at("name").get<std::string>()).extension() == ".log" &&
                           file.at("bytes").get<std::uint64_t>() > 0;
            EXPECT_FALSE(has_wal);
        }

        TEST_F(DatasetTest, BulkLoadUsesParallelProducersForLargerDatasets)
        {
            spec.dataset.records = 200'000;
            spec.dataset.value_bytes = 32;
            spec.dataset.overwrite_rounds = 1;
            spec.engine.write_buffer_bytes = 4ULL << 20;
            spec.engine.target_file_bytes = 4ULL << 20;
            spec.limits.logical_write_bytes = 1ULL << 30;
            spec.dataset.load_mode = "bulk";
            const auto manifest = create_dataset(spec, root / "bulk");
            EXPECT_GE(manifest.at("creation").at("producer_threads").get<std::uint64_t>(), 1U);
            EXPECT_EQ(verify_dataset(root / "bulk").at("records_verified"), spec.dataset.records);
        }

        TEST_F(DatasetTest, IngestLoadMatchesSynchronousContentInSortedFiles)
        {
            spec.dataset.records = 5000;
            spec.limits.logical_write_bytes = 64ULL << 20;
            create_dataset(spec, root / "sync");
            auto ingest = spec;
            ingest.dataset.load_mode = "ingest";
            const auto manifest = create_dataset(ingest, root / "ingest");
            const auto &creation = manifest.at("creation");
            EXPECT_EQ(creation.at("load_mode"), "ingest");
            EXPECT_EQ(creation.at("records_written"), spec.dataset.records);
            EXPECT_EQ(creation.at("final_generation"), spec.dataset.overwrite_rounds);
            EXPECT_GE(creation.at("files_ingested").get<std::uint64_t>(), 1U);
            EXPECT_FALSE(std::filesystem::exists(root / "ingest" / "ingest-staging"));
            EXPECT_EQ(verify_dataset(root / "ingest").at("records_verified"), spec.dataset.records);
            EXPECT_EQ(contents(root / "ingest", ingest), contents(root / "sync", spec));
            std::unique_ptr<rocksdb::DB> database;
            ASSERT_TRUE(rocksdb::DB::OpenForReadOnly(engine_options(ingest), (root / "ingest" / "db").string(), &database).ok());
            std::vector<rocksdb::LiveFileMetaData> files;
            database->GetLiveFilesMetaData(&files);
            ASSERT_FALSE(files.empty());
            for (const auto &file : files)
                EXPECT_EQ(file.level, files.front().level);
            ASSERT_TRUE(database->Close().ok());
            EXPECT_NO_THROW(clone_dataset(root / "ingest", root / "ingest-clone"));
        }

        TEST_F(DatasetTest, VerifyTrialChecksInsertedKeys)
        {
            const auto source = root / "baseline";
            const auto trial = root / "trial";
            create_dataset(spec, source);
            const auto clone = clone_dataset(source, trial);
            const auto inserted = spec.dataset.records + 40;
            const auto generation = spec.dataset.overwrite_rounds + 41;
            {
                std::unique_ptr<rocksdb::DB> database;
                ASSERT_TRUE(rocksdb::DB::Open(engine_options(spec), (trial / "db").string(), &database).ok());
                rocksdb::WriteOptions writes;
                writes.sync = true;
                ASSERT_TRUE(database->Put(writes, generated_key(spec.dataset, inserted), generated_value(spec.dataset, inserted, generation)).ok());
                ASSERT_TRUE(database->Put(writes, record_key(spec.dataset, 3), record_value(spec.dataset, 3, generation + 1)).ok());
                ASSERT_TRUE(database->Close().ok());
            }
            const auto write = [](const std::filesystem::path &path, const nlohmann::json &document)
            { std::ofstream(path) << document.dump() << '\n'; };
            write(trial / "run.json", {{"owner", "lsm-lab"}, {"state", "stopped"}, {"run_id", "insert-run"}});
            const auto load = root / "load";
            std::filesystem::create_directory(load);
            nlohmann::json load_manifest{{"owner", "lsm-lab"}, {"state", "complete"}, {"run_id", "insert-run"},
                                         {"dataset_id", clone.at("dataset_id")}, {"spec", clone.at("spec")},
                                         {"memory_budget_bytes", 1ULL << 20}, {"key_index_limit", spec.dataset.records + 100}};
            write(load / "manifest.json", load_manifest);
            write(load / "result.json", {{"state", "complete"}, {"run_id", "insert-run"},
                                         {"warmup", {{"indeterminate_writes", 0}}}, {"measurement", {{"indeterminate_writes", 0}}}});
            std::ofstream(load / "final-versions.jsonl") << nlohmann::json{{"key_index", 3}, {"generation", generation + 1}}.dump() << '\n'
                                                         << nlohmann::json{{"key_index", inserted}, {"generation", generation}}.dump() << '\n';
            const auto report = verify_trial(trial, load);
            EXPECT_EQ(report.at("records_verified"), spec.dataset.records + 1);
            EXPECT_EQ(report.at("inserted_keys_verified"), 1U);
            EXPECT_EQ(report.at("modified_keys_verified"), 2U);
            // An inserted key that the load did not report is rejected.
            std::ofstream(load / "final-versions.jsonl") << nlohmann::json{{"key_index", 3}, {"generation", generation + 1}}.dump() << '\n';
            EXPECT_THROW(verify_trial(trial, load), std::runtime_error);
            // Without a declared key index limit, indices beyond the dataset are out of range.
            std::ofstream(load / "final-versions.jsonl") << nlohmann::json{{"key_index", inserted}, {"generation", generation}}.dump() << '\n'
                                                         << nlohmann::json{{"key_index", 3}, {"generation", generation + 1}}.dump() << '\n';
            EXPECT_NO_THROW(verify_trial(trial, load));
            load_manifest.erase("key_index_limit");
            write(load / "manifest.json", load_manifest);
            EXPECT_THROW(verify_trial(trial, load), std::runtime_error);
            // A reported insert that is missing from the database fails the record count.
            load_manifest["key_index_limit"] = spec.dataset.records + 100;
            write(load / "manifest.json", load_manifest);
            std::ofstream(load / "final-versions.jsonl") << nlohmann::json{{"key_index", inserted}, {"generation", generation}}.dump() << '\n'
                                                         << nlohmann::json{{"key_index", inserted + 1}, {"generation", generation}}.dump() << '\n'
                                                         << nlohmann::json{{"key_index", 3}, {"generation", generation + 1}}.dump() << '\n';
            EXPECT_THROW(verify_trial(trial, load), std::runtime_error);
        }
    }
}