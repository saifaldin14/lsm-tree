// Transport microbenchmark for remote compaction: round trips of a request of S bytes and a
// reply of S bytes against lsm_compaction_server, over TCP or RDMA.
//
//   lsm_rpc_bench --endpoint rdma://192.168.126.104:7471 --sizes 1M,16M,256M --iterations 20
//
// Prints one JSON line per size: median round trip, goodput in Gbit/s counting both
// directions, and client CPU seconds per GiB moved.

#include "lsm_lab/remote_merge.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <sys/resource.h>
#include <vector>

namespace
{
    std::size_t parse_size(const std::string &text)
    {
        std::size_t multiplier = 1;
        auto digits = text;
        switch (text.back())
        {
        case 'K':
            multiplier = 1ULL << 10;
            break;
        case 'M':
            multiplier = 1ULL << 20;
            break;
        case 'G':
            multiplier = 1ULL << 30;
            break;
        default:
            break;
        }
        if (multiplier != 1)
            digits.pop_back();
        return std::stoull(digits) * multiplier;
    }

    double cpu_seconds()
    {
        rusage usage{};
        getrusage(RUSAGE_SELF, &usage);
        return usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6 + usage.ru_stime.tv_sec +
               usage.ru_stime.tv_usec / 1e6;
    }
}

int main(int argc, char **argv)
{
    try
    {
        std::string endpoint;
        std::string sizes = "64K,1M,16M,128M,512M";
        int iterations = 20;
        for (int index = 1; index + 1 < argc; index += 2)
        {
            const std::string name = argv[index];
            const std::string value = argv[index + 1];
            if (name == "--endpoint")
                endpoint = value;
            else if (name == "--sizes")
                sizes = value;
            else if (name == "--iterations")
                iterations = std::stoi(value);
            else
                throw std::invalid_argument("unknown argument: " + name);
        }
        auto client = lsm_lab::connect_rpc(lsm_lab::parse_endpoint(endpoint));
        std::size_t start = 0;
        while (start < sizes.size())
        {
            const auto end = std::min(sizes.find(',', start), sizes.size());
            const auto size = parse_size(sizes.substr(start, end - start));
            start = end + 1;
            lsm_lab::RequestHeader header;
            header.kind = lsm_lab::RpcKind::echo;
            header.meta_bytes = size;
            header.blocks_offset = 64;
            header.blocks_bytes = size;
            const auto request_bytes = std::max<std::size_t>(size, 64);
            char *buffer = client->request_buffer(request_bytes);
            std::memset(buffer, 0x5a, request_bytes);
            std::memcpy(buffer, &header, sizeof(header));
            std::vector<double> seconds;
            const auto cpu_before = cpu_seconds();
            for (int iteration = 0; iteration < iterations + 2; ++iteration)
            {
                std::shared_ptr<const char> reply;
                std::size_t reply_bytes = 0;
                const auto started = std::chrono::steady_clock::now();
                const auto status = client->call(request_bytes, &reply, &reply_bytes);
                const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - started;
                if (!status.ok())
                    throw std::runtime_error(status.ToString());
                if (iteration >= 2) // the first calls register and size buffers
                    seconds.push_back(elapsed.count());
            }
            const auto cpu = cpu_seconds() - cpu_before;
            std::sort(seconds.begin(), seconds.end());
            const auto median = seconds[seconds.size() / 2];
            const double moved = 2.0 * static_cast<double>(size);
            nlohmann::json line{{"endpoint", endpoint},
                                {"transport", client->describe()},
                                {"bytes_each_way", size},
                                {"iterations", iterations},
                                {"median_round_trip_us", median * 1e6},
                                {"min_round_trip_us", seconds.front() * 1e6},
                                {"gbit_per_s", moved * 8 / median / 1e9},
                                {"client_cpu_s_per_gib", cpu / (moved * (iterations + 2) / (1ULL << 30))}};
            std::cout << line.dump() << std::endl;
        }
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "lsm_rpc_bench: " << error.what() << '\n';
        return 1;
    }
}
