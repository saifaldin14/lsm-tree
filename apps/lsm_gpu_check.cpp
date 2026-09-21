#include "lsm_lab/block_transform.hpp"

#include <algorithm>
#include <iostream>
#include <lz4.h>
#include "util/coding.h"
#include "util/crc32c.h"

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3)
        return 2;
    const std::string backend = argv[1];
    const bool required = argc == 3 && std::string(argv[2]) == "--require";
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
        std::string original(65536, '\0');
        for (std::size_t offset = 0; offset < original.size(); ++offset)
            original[offset] = static_cast<char>(offset % 251);
        std::string encoded;
        rocksdb::PutVarint32(&encoded, original.size());
        std::string compressed(LZ4_compressBound(original.size()), '\0');
        const auto size = LZ4_compress_default(original.data(), compressed.data(), original.size(), compressed.size());
        if (size <= 0)
            throw std::runtime_error("test compression failed");
        encoded.append(compressed.data(), size);
        encoded += '\4';
        rocksdb::PutFixed32(&encoded, rocksdb::crc32c::Mask(rocksdb::crc32c::Value(encoded.data(), encoded.size())));
        std::vector<std::string> output;
        const auto status = transform->Transform({{encoded, 0}}, &output);
        if (!status.ok() || output.size() != 1 || output.front() != original)
            throw std::runtime_error("GPU decoded bytes failed CPU comparison: " + status.ToString());
        auto report = transform->statistics();
        if (report.at("dispatches").get<std::uint64_t>() == 0)
            throw std::runtime_error("no GPU dispatch executed");
        report["status"] = "PASS";
        report["full_bytes_verified"] = true;
        std::cout << report.dump(2) << '\n';
        return 0;
    }
    catch (const std::exception &failure)
    {
        std::cerr << nlohmann::json{{"status", "FAIL"}, {"backend", backend}, {"detail", failure.what()}}.dump() << '\n';
        return 1;
    }
}