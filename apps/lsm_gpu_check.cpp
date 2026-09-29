#include "lsm_lab/block_transform.hpp"

#include <algorithm>
#include <iostream>
#include <lz4.h>
#include "util/coding.h"
#include "util/crc32c.h"

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    const std::string backend = argv[1];
    bool required = false;
    std::size_t block_count = 1;
    std::size_t block_bytes = 65536;
    std::size_t repetitions = 1;
    for (int index = 2; index < argc; ++index)
    {
        const std::string argument = argv[index];
        if (argument == "--require")
        {
            required = true;
        }
        else if ((argument == "--blocks" || argument == "--block-bytes" || argument == "--repetitions") &&
                 index + 1 < argc)
        {
            const auto value = std::stoull(argv[++index]);
            if (value == 0)
                return 2;
            if (argument == "--blocks")
                block_count = value;
            else if (argument == "--block-bytes")
                block_bytes = value;
            else
                repetitions = value;
        }
        else
        {
            return 2;
        }
    }
    if (block_count > rocksdb::CompactionBlockTransform::max_blocks ||
        block_bytes > rocksdb::CompactionBlockTransform::max_block_bytes || repetitions > 1000)
        return 2;
    const auto available = lsm_lab::available_transforms();
    if (std::find(available.begin(), available.end(), backend) == available.end())
    {
        std::cout << nlohmann::json{{"status", "UNAVAILABLE"}, {"backend", backend}, {"reason", "backend is not compiled or no physical device is exposed to this process"}}.dump() << '\n';
        return required ? 1 : 77;
    }
    try
    {
        auto transform = lsm_lab::make_transform(backend);
        if (!transform)
            throw std::runtime_error("hardware check requires a transforming backend");
        std::vector<std::string> originals;
        std::vector<std::string> encoded;
        for (std::size_t block = 0; block < block_count; ++block)
        {
            std::string original(block_bytes, '\0');
            for (std::size_t offset = 0; offset < original.size(); ++offset)
                original[offset] = static_cast<char>((offset + block * 17) % 251);
            std::string serialized;
            rocksdb::PutVarint32(&serialized, original.size());
            std::string compressed(LZ4_compressBound(original.size()), '\0');
            const auto size = LZ4_compress_default(original.data(), compressed.data(), original.size(), compressed.size());
            if (size <= 0)
                throw std::runtime_error("test compression failed");
            serialized.append(compressed.data(), size);
            serialized += '\4';
            rocksdb::PutFixed32(&serialized, rocksdb::crc32c::Mask(rocksdb::crc32c::Value(serialized.data(), serialized.size())));
            originals.push_back(std::move(original));
            encoded.push_back(std::move(serialized));
        }
        std::vector<rocksdb::CompactionBlockInput> input;
        for (const auto &serialized : encoded)
            input.push_back({serialized, 0});
        std::vector<std::string> output;
        for (std::size_t repetition = 0; repetition < repetitions; ++repetition)
        {
            const auto status = transform->Transform(input, &output);
            if (!status.ok() || output != originals)
                throw std::runtime_error("GPU decoded bytes failed CPU comparison: " + status.ToString());
        }
        auto report = transform->statistics();
        if (backend != "cpu_batched" && report.at("dispatches").get<std::uint64_t>() == 0 &&
            (backend != "cuda_hybrid" || required))
            throw std::runtime_error("no GPU dispatch executed");
        report["status"] = "PASS";
        report["full_bytes_verified"] = true;
        report["requested_blocks"] = block_count;
        report["requested_block_bytes"] = block_bytes;
        report["repetitions"] = repetitions;
        std::cout << report.dump(2) << '\n';
        return 0;
    }
    catch (const std::exception &failure)
    {
        std::cerr << nlohmann::json{{"status", "FAIL"}, {"backend", backend}, {"detail", failure.what()}}.dump() << '\n';
        return 1;
    }
}