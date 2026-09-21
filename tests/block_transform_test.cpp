#include "lsm_lab/block_transform.hpp"

#include <gtest/gtest.h>
#include <lz4.h>
#include "table/format.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace lsm_lab
{
    namespace
    {
        std::string serialize(const std::string &input, bool compressed, std::uint32_t modifier)
        {
            std::string encoded;
            if (compressed)
            {
                std::string buffer(LZ4_compressBound(static_cast<int>(input.size())), '\0');
                const auto size = LZ4_compress_default(input.data(), buffer.data(), static_cast<int>(input.size()), static_cast<int>(buffer.size()));
                EXPECT_GT(size, 0);
                rocksdb::PutVarint32(&encoded, input.size());
                encoded.append(buffer.data(), size);
            }
            else
                encoded = input;
            encoded += compressed ? '\4' : '\0';
            rocksdb::PutFixed32(&encoded, rocksdb::crc32c::Mask(rocksdb::crc32c::Value(encoded.data(), encoded.size())) + modifier);
            return encoded;
        }

        class BlockTransformTest : public ::testing::TestWithParam<std::string>
        {
        };

        TEST_P(BlockTransformTest, FullBytesNativeFramingAndBoundedReuse)
        {
            auto backend = make_transform(GetParam());
            const auto modifier = rocksdb::ChecksumModifierForContext(123, 123456789);
            std::vector<std::string> originals, serialized;
            for (std::size_t index = 0; index < 32; ++index)
            {
                std::string value(index * 1024 + 17, static_cast<char>(index));
                for (std::size_t offset = 0; offset < value.size() / 2; ++offset)
                    value[offset] = static_cast<char>((offset * 131 + index) & 255);
                originals.push_back(value);
                serialized.push_back(serialize(value, index % 2 != 0, modifier));
            }
            std::vector<rocksdb::CompactionBlockInput> input;
            for (const auto &value : serialized)
                input.push_back({value, modifier});
            std::vector<std::string> output;
            for (int repetition = 0; repetition < 3; ++repetition)
            {
                const auto status = backend->Transform(input, &output);
                ASSERT_TRUE(status.ok()) << status.ToString();
                EXPECT_EQ(output, originals);
            }
            EXPECT_EQ(backend->statistics().at("blocks"), 96);
            if (GetParam() == "cpu_batched")
                EXPECT_EQ(backend->statistics().at("dispatches"), 0);
            else
                EXPECT_GE(backend->statistics().at("dispatches").get<std::uint64_t>(), 3U);
        }

        TEST_P(BlockTransformTest, RejectsCorruptionMalformedLz4AndBudgets)
        {
            auto backend = make_transform(GetParam());
            std::vector<std::string> output;
            auto serialized = serialize(std::string(65536, 'a'), true, 0);
            serialized[serialized.size() - 1] ^= 1;
            EXPECT_TRUE(backend->Transform({{serialized, 0}}, &output).IsCorruption());
            EXPECT_TRUE(output.empty());
            EXPECT_TRUE(backend->Transform({{rocksdb::Slice("x", 1), 0}}, &output).IsCorruption());
            std::string invalid;
            rocksdb::PutVarint32(&invalid, 500);
            invalid.append(3, '\0');
            invalid += '\4';
            rocksdb::PutFixed32(&invalid, rocksdb::crc32c::Mask(rocksdb::crc32c::Value(invalid.data(), invalid.size())));
            EXPECT_TRUE(backend->Transform({{invalid, 0}}, &output).IsCorruption());
            auto valid = serialize("payload", false, 0);
            EXPECT_TRUE(backend->Transform(std::vector<rocksdb::CompactionBlockInput>(33, {valid, 0}), &output).IsInvalidArgument());
            EXPECT_TRUE(backend->Transform({{valid, 1}}, &output).IsCorruption());
        }

        std::vector<std::string> tested_backends()
        {
            auto backends = available_transforms();
            backends.erase(backends.begin());
            return backends;
        }
        INSTANTIATE_TEST_SUITE_P(AvailableBackends, BlockTransformTest, ::testing::ValuesIn(tested_backends()));
    }
}