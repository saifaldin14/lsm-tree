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
            else if (GetParam() == "cuda_hybrid")
            {
                EXPECT_EQ(backend->statistics().at("dispatches"), 0);
                EXPECT_EQ(backend->statistics().at("cpu_fallback_batches"), 3);
                EXPECT_EQ(backend->statistics().at("cpu_fallback_blocks"), 96);
                EXPECT_GT(backend->statistics().at("cpu_fallback_bytes").get<std::uint64_t>(), 0U);
            }
            else
                EXPECT_GE(backend->statistics().at("dispatches").get<std::uint64_t>(), 3U);
        }

        TEST_P(BlockTransformTest, ReusesBackendAcrossChangingBatchSizes)
        {
            auto backend = make_transform(GetParam());
            const auto modifier = rocksdb::ChecksumModifierForContext(456, 987654321);
            std::vector<std::string> originals;
            for (std::size_t index = 0; index < 256; ++index)
                originals.emplace_back((index % 32 + 1) * 2048, static_cast<char>(index + 1));

            std::uint64_t expected_blocks = 0;
            for (const auto count : {std::size_t{1}, std::size_t{256}, std::size_t{7}, std::size_t{256}})
            {
                std::vector<std::string> serialized;
                std::vector<rocksdb::CompactionBlockInput> input;
                for (std::size_t index = 0; index < count; ++index)
                    serialized.push_back(serialize(originals[index], index % 3 != 0, modifier));
                for (const auto &value : serialized)
                    input.push_back({value, modifier});

                std::vector<std::string> output;
                const auto status = backend->Transform(input, &output);
                ASSERT_TRUE(status.ok()) << status.ToString();
                EXPECT_EQ(output, std::vector<std::string>(originals.begin(), originals.begin() + count));
                expected_blocks += count;
            }

            const auto statistics = backend->statistics();
            EXPECT_EQ(statistics.at("batches"), 4);
            EXPECT_EQ(statistics.at("blocks"), expected_blocks);
            if (GetParam() != "cpu_batched")
            {
                EXPECT_GE(statistics.at("dispatches").get<std::uint64_t>(), 4U);
            }
        }

        TEST_P(BlockTransformTest, AggregatesFilesIntoJobWideSession)
        {
            auto backend = make_transform(GetParam());
            auto session = backend->CreateSession();
            std::vector<std::string> originals;
            std::vector<std::string> serialized;
            for (std::size_t index = 0; index < 300; ++index)
            {
                originals.emplace_back(4096, static_cast<char>(index + 1));
                serialized.push_back(serialize(originals.back(), true, 0));
            }
            for (std::size_t file = 0; file < 3; ++file)
            {
                std::vector<rocksdb::CompactionBlockKey> keys;
                std::vector<rocksdb::CompactionBlockInput> input;
                for (std::size_t block = 0; block < 100; ++block)
                {
                    const auto index = file * 100 + block;
                    keys.push_back({file + 100, block * 4096});
                    input.push_back({serialized[index], 0});
                }
                ASSERT_TRUE(session->Add(keys, input).ok());
            }
            ASSERT_TRUE(session->Finish().ok());
            for (std::size_t file = 0; file < 3; ++file)
            {
                for (std::size_t block = 0; block < 100; ++block)
                {
                    std::string output;
                    EXPECT_TRUE(session->Take({file + 100, block * 4096}, &output));
                    EXPECT_EQ(output, originals[file * 100 + block]);
                    EXPECT_FALSE(session->Take({file + 100, block * 4096}, &output));
                }
            }
            EXPECT_EQ(backend->statistics().at("batches"), 2);
            EXPECT_EQ(backend->statistics().at("blocks"), 300);
        }

        TEST_P(BlockTransformTest, BuildsCompressedOutputWithValidChecksum)
        {
            auto backend = make_transform(GetParam());
            std::vector<std::string> originals(32);
            std::vector<rocksdb::Slice> input;
            for (std::size_t index = 0; index < originals.size(); ++index)
            {
                originals[index].resize(65536);
                for (std::size_t offset = 0; offset < originals[index].size(); ++offset)
                    originals[index][offset] = static_cast<char>((offset + index * 11) % 251);
                input.emplace_back(originals[index]);
            }

            std::vector<rocksdb::CompactionOutputBlock> output;
            const auto status = backend->BuildOutput(input, &output);
            ASSERT_TRUE(status.ok()) << status.ToString();
            ASSERT_EQ(output.size(), originals.size());
            for (std::size_t index = 0; index < output.size(); ++index)
            {
                std::string checksummed = output[index].contents;
                checksummed.push_back(output[index].compressed ? '\4' : '\0');
                EXPECT_EQ(output[index].checksum,
                          rocksdb::crc32c::Mask(
                              rocksdb::crc32c::Value(checksummed.data(), checksummed.size())));
                if (output[index].compressed)
                {
                    rocksdb::Slice compressed(output[index].contents);
                    std::uint32_t decoded_bytes = 0;
                    ASSERT_TRUE(rocksdb::GetVarint32(&compressed, &decoded_bytes));
                    ASSERT_EQ(decoded_bytes, originals[index].size());
                    std::string decoded(decoded_bytes, '\0');
                    ASSERT_EQ(LZ4_decompress_safe(compressed.data(), decoded.data(),
                                                  static_cast<int>(compressed.size()),
                                                  static_cast<int>(decoded.size())),
                              static_cast<int>(decoded.size()));
                    EXPECT_EQ(decoded, originals[index]);
                }
                else
                {
                    EXPECT_EQ(output[index].contents, originals[index]);
                }
            }
        }

        TEST_P(BlockTransformTest, SubmitsOwnedOutputAndFinishesOnce)
        {
            auto backend = make_transform(GetParam());
            std::vector<std::string> input(64);
            for (std::size_t index = 0; index < input.size(); ++index)
                input[index].assign(131072, static_cast<char>(index + 1));
            std::unique_ptr<rocksdb::CompactionOutputSubmission> submission;
            ASSERT_TRUE(backend->SubmitOutput(std::move(input), &submission).ok());
            ASSERT_NE(submission, nullptr);
            std::vector<rocksdb::CompactionOutputBlock> output;
            ASSERT_TRUE(submission->Finish(&output).ok());
            ASSERT_EQ(output.size(), 64);
            EXPECT_TRUE(submission->Finish(&output).IsInvalidArgument());
        }

        TEST_P(BlockTransformTest, FinishesTwoOwnedSubmissionsInOrder)
        {
            auto backend = make_transform(GetParam());
            std::vector<std::unique_ptr<rocksdb::CompactionOutputSubmission>>
                submissions(2);
            for (std::size_t batch = 0; batch < submissions.size(); ++batch)
            {
                std::vector<std::string> input(64);
                for (std::size_t index = 0; index < input.size(); ++index)
                    input[index].assign(
                        131072, static_cast<char>(batch * input.size() + index + 1));
                ASSERT_TRUE(backend->SubmitOutput(
                    std::move(input), &submissions[batch]).ok());
            }
            for (auto &submission : submissions)
            {
                std::vector<rocksdb::CompactionOutputBlock> output;
                ASSERT_TRUE(submission->Finish(&output).ok());
                EXPECT_EQ(output.size(), 64);
            }
            const auto statistics = backend->statistics();
            EXPECT_EQ(statistics.at("output_batches"), 2);
            EXPECT_EQ(statistics.at("output_blocks"), 128);
            if (GetParam() == "cuda_batched" || GetParam() == "cuda_hybrid")
            {
                EXPECT_EQ(statistics.at("output_gpu_batches"), 2);
            }
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
            EXPECT_TRUE(backend->Transform(std::vector<rocksdb::CompactionBlockInput>(257, {valid, 0}), &output).IsInvalidArgument());
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