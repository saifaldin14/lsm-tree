#include "lsm_lab/spec.hpp"

#include <limits>
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

    }
}