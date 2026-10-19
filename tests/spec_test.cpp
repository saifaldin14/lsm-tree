#include "lsm_lab/spec.hpp"

#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace lsm_lab
{
    namespace
    {

        TEST(Spec, RoundTripAndByteBudget)
        {
            Spec spec;
            EXPECT_EQ(logical_write_bytes(spec.dataset), 544000U);
            EXPECT_EQ(parse_spec(spec_json(spec).dump()), spec);
            spec.dataset.seed = std::numeric_limits<std::uint64_t>::max();
            EXPECT_EQ(parse_spec(spec_json(spec).dump()), spec);
        }

        TEST(Spec, RejectsUnknownAndMissingFields)
        {
            auto document = spec_json(Spec{});
            document["dataset"]["recordz"] = 3;
            EXPECT_THROW(parse_spec(document.dump()), std::invalid_argument);
            document["dataset"].erase("recordz");
            document["engine"].erase("compression");
            EXPECT_THROW(parse_spec(document.dump()), std::invalid_argument);
        }

        TEST(Spec, RejectsInvalidNumericTypesAndLimits)
        {
            auto document = spec_json(Spec{});
            for (const nlohmann::json &value : {nlohmann::json(-1), nlohmann::json(1.5),
                                                nlohmann::json(true), nlohmann::json("100"),
                                                nlohmann::json(nullptr), nlohmann::json(0)})
            {
                document["dataset"]["records"] = value;
                EXPECT_THROW(parse_spec(document.dump()), std::invalid_argument);
            }
            document = spec_json(Spec{});
            document["dataset"]["key_bytes"] = 1ULL << 32;
            EXPECT_THROW(parse_spec(document.dump()), std::invalid_argument);
            document = spec_json(Spec{});
            document["limits"]["logical_write_bytes"] = 543999;
            EXPECT_THROW(parse_spec(document.dump()), std::invalid_argument);
        }

        TEST(Spec, RejectsDuplicateKeys)
        {
            EXPECT_THROW(parse_spec(R"({"schema_version":1,"schema_version":1})"), std::invalid_argument);
            EXPECT_THROW(parse_spec(R"({"dataset":{"records":1,"records":2}})"), std::invalid_argument);
        }

        TEST(Spec, RejectsUnknownVersionAndCompression)
        {
            auto document = spec_json(Spec{});
            document["schema_version"] = 2;
            EXPECT_THROW(parse_spec(document.dump()), std::invalid_argument);
            document = spec_json(Spec{});
            document["engine"]["compression"] = "gpu";
            EXPECT_THROW(parse_spec(document.dump()), std::invalid_argument);
            document["engine"]["compression"] = "none";
            EXPECT_EQ(parse_spec(document.dump()).engine.compression, "none");
        }

        TEST(Spec, RejectsOversizedAndMalformedDocuments)
        {
            EXPECT_THROW(parse_spec(std::string(65537, ' ')), std::invalid_argument);
            EXPECT_THROW(parse_spec("{"), nlohmann::json::parse_error);
            EXPECT_THROW(parse_spec("[]"), std::invalid_argument);
            EXPECT_THROW(parse_spec(std::string(20, '[') + "0" + std::string(20, ']')),
                         std::invalid_argument);
        }

        TEST(Spec, DetectsIntegerOverflow)
        {
            DatasetSpec dataset;
            dataset.records = std::numeric_limits<std::uint64_t>::max();
            EXPECT_THROW(logical_write_bytes(dataset), std::invalid_argument);
            dataset.records = std::numeric_limits<std::uint64_t>::max() / 272;
            EXPECT_THROW(logical_write_bytes(dataset), std::invalid_argument);
        }


        TEST(Spec, OptionalFieldsDefaultToCurrentBehaviorAndAreOmitted)
        {
            const auto document = spec_json(Spec{});
            EXPECT_EQ(document.at("dataset").size(), 5U);
            EXPECT_EQ(document.at("engine").size(), 7U);
            auto explicit_defaults = document;
            explicit_defaults["dataset"]["load_mode"] = "sync";
            explicit_defaults["engine"]["max_subcompactions"] = 1;
            explicit_defaults["engine"]["max_write_buffer_number"] = 4;
            explicit_defaults["engine"]["use_direct_io"] = false;
            explicit_defaults["engine"]["verify_output"] = true;
            const auto parsed = parse_spec(explicit_defaults.dump());
            EXPECT_EQ(parsed, Spec{});
            EXPECT_EQ(spec_json(parsed), document);
        }

        TEST(Spec, OptionalFieldsRoundTrip)
        {
            Spec spec;
            auto ingest = Spec{};
            ingest.dataset.load_mode = "ingest";
            EXPECT_EQ(parse_spec(spec_json(ingest).dump()), ingest);
            spec.dataset.load_mode = "bulk";
            spec.engine.compression = "zstd";
            spec.engine.max_subcompactions = 4;
            spec.engine.max_bytes_for_level_base = 256ULL << 20;
            spec.engine.max_write_buffer_number = 6;
            spec.engine.level0_file_num_compaction_trigger = 8;
            spec.engine.level0_slowdown_writes_trigger = 40;
            spec.engine.level0_stop_writes_trigger = 64;
            spec.engine.use_direct_io = true;
            spec.engine.verify_output = false;
            const auto document = spec_json(spec);
            EXPECT_EQ(document.at("engine").at("verify_output"), false);
            EXPECT_EQ(document.at("dataset").at("load_mode"), "bulk");
            EXPECT_EQ(document.at("engine").at("max_bytes_for_level_base"), 256ULL << 20);
            EXPECT_EQ(document.at("engine").at("use_direct_io"), true);
            EXPECT_EQ(parse_spec(document.dump()), spec);
        }

        TEST(Spec, RejectsInvalidOptionalFields)
        {
            const auto rejects = [](const char *section, const char *field, const nlohmann::json &value)
            {
                auto document = spec_json(Spec{});
                document[section][field] = value;
                EXPECT_THROW(parse_spec(document.dump()), std::invalid_argument) << section << "." << field << "=" << value.dump();
            };
            rejects("dataset", "load_mode", "fast");
            rejects("dataset", "load_mode", 1);
            rejects("engine", "max_subcompactions", 0);
            rejects("engine", "max_subcompactions", 257);
            rejects("engine", "max_bytes_for_level_base", 1024);
            rejects("engine", "max_bytes_for_level_base", -1);
            rejects("engine", "max_write_buffer_number", 1);
            rejects("engine", "max_write_buffer_number", 65);
            rejects("engine", "level0_file_num_compaction_trigger", 0);
            rejects("engine", "level0_file_num_compaction_trigger", 21);
            rejects("engine", "level0_slowdown_writes_trigger", 37);
            rejects("engine", "level0_stop_writes_trigger", 19);
            rejects("engine", "level0_stop_writes_trigger", 1025);
            rejects("engine", "use_direct_io", 1);
            rejects("engine", "use_direct_io", "true");
            rejects("engine", "verify_output", 0);
            rejects("engine", "max_subcompaction", 2);
            auto document = spec_json(Spec{});
            document["engine"]["level0_file_num_compaction_trigger"] = 30;
            document["engine"]["level0_slowdown_writes_trigger"] = 30;
            EXPECT_NO_THROW(parse_spec(document.dump()));
        }

        TEST(Spec, AcceptsZstdCompression)
        {
            auto document = spec_json(Spec{});
            document["engine"]["compression"] = "zstd";
            EXPECT_EQ(parse_spec(document.dump()).engine.compression, "zstd");
            document["engine"]["compression"] = "ZSTD";
            EXPECT_THROW(parse_spec(document.dump()), std::invalid_argument);
        }

        TEST(Spec, CheckedInDatasetConfigsKeepTheirCanonicalEncoding)
        {
            std::size_t checked = 0;
            for (const auto &entry : std::filesystem::directory_iterator(LSM_SOURCE_DIR "/configs"))
            {
                if (entry.path().extension() != ".json")
                    continue;
                std::ifstream input(entry.path());
                std::stringstream text;
                text << input.rdbuf();
                const auto document = nlohmann::json::parse(text.str());
                if (!document.contains("engine"))
                    continue;
                EXPECT_EQ(spec_json(parse_spec(text.str())), document) << entry.path();
                ++checked;
            }
            EXPECT_GT(checked, 0U);
        }
    }
}