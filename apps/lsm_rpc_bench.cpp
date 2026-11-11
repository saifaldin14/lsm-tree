// Transport microbenchmark for remote compaction: round trips of a request of S bytes and a
// reply of S bytes against lsm_compaction_server, over TCP or RDMA.
//
//   lsm_rpc_bench --endpoint rdma://192.168.126.104:7471 --sizes 1M,16M,256M --iterations 20
//
// Prints one JSON line per size: median round trip, goodput in Gbit/s counting both
// directions, and client CPU seconds per GiB moved.
//
//   lsm_rpc_bench --endpoint unix:///tmp/s.sock --replay DIR --clients 8 --duration 60
//
// Replay: C clients, each on its own connection, send the compaction requests recorded in
// DIR (LSM_REMOTE_RECORD_DIR, *.req) back to back for the duration, each starting at a
// different request. Prints jobs/s, input GiB/s and latency percentiles.

#include "lsm_lab/remote_merge.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sys/resource.h>
#include <thread>
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

    struct Frame
    {
        std::string bytes;
        std::uint64_t input_bytes = 0;
    };

    std::vector<Frame> load_frames(const std::string &directory)
    {
        std::vector<std::filesystem::path> paths;
        for (const auto &entry : std::filesystem::directory_iterator(directory))
            if (entry.path().extension() == ".req")
                paths.push_back(entry.path());
        std::sort(paths.begin(), paths.end());
        std::vector<Frame> frames;
        for (const auto &path : paths)
        {
            std::ifstream file(path, std::ios::binary);
            Frame frame;
            frame.bytes.assign(std::istreambuf_iterator<char>(file), {});
            lsm_lab::RequestHeader header;
            if (frame.bytes.size() < sizeof(header))
                continue;
            std::memcpy(&header, frame.bytes.data(), sizeof(header));
            frame.input_bytes = header.blocks_bytes;
            frames.push_back(std::move(frame));
        }
        if (frames.empty())
            throw std::runtime_error("no .req files in " + directory);
        return frames;
    }

    int replay(const std::string &endpoint, const std::string &directory, int clients, double duration)
    {
        const auto frames = load_frames(directory);
        std::mutex mutex;
        std::vector<double> latencies;
        std::uint64_t jobs = 0, input = 0, failures = 0;
        std::atomic<bool> stop{false};
        const auto started = std::chrono::steady_clock::now();
        std::vector<std::thread> threads;
        for (int index = 0; index < clients; ++index)
            threads.emplace_back([&, index] {
                auto client = lsm_lab::connect_rpc(lsm_lab::parse_endpoint(endpoint));
                std::vector<double> mine;
                std::uint64_t my_jobs = 0, my_input = 0, my_failures = 0;
                for (std::size_t next = static_cast<std::size_t>(index) * frames.size() / clients; !stop; ++next)
                {
                    const auto &frame = frames[next % frames.size()];
                    std::memcpy(client->request_buffer(frame.bytes.size()), frame.bytes.data(), frame.bytes.size());
                    std::shared_ptr<const char> reply;
                    std::size_t reply_bytes = 0;
                    const auto call_started = std::chrono::steady_clock::now();
                    const auto status = client->call(frame.bytes.size(), &reply, &reply_bytes);
                    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - call_started;
                    lsm_lab::ReplyHeader answer;
                    if (!status.ok() || reply_bytes < sizeof(answer))
                    {
                        ++my_failures;
                        continue;
                    }
                    std::memcpy(&answer, reply.get(), sizeof(answer));
                    if (answer.status != 0)
                    {
                        ++my_failures;
                        continue;
                    }
                    mine.push_back(elapsed.count());
                    ++my_jobs;
                    my_input += frame.input_bytes;
                }
                const std::lock_guard lock(mutex);
                latencies.insert(latencies.end(), mine.begin(), mine.end());
                jobs += my_jobs;
                input += my_input;
                failures += my_failures;
            });
        std::this_thread::sleep_for(std::chrono::duration<double>(duration));
        stop = true;
        for (auto &thread : threads)
            thread.join();
        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - started;
        std::sort(latencies.begin(), latencies.end());
        const auto percentile = [&](double p) {
            return latencies.empty() ? 0.0 : latencies[std::min(latencies.size() - 1, static_cast<std::size_t>(p * latencies.size()))];
        };
        std::uint64_t frame_bytes = 0;
        for (const auto &frame : frames)
            frame_bytes += frame.input_bytes;
        nlohmann::json line{{"endpoint", endpoint},
                            {"clients", clients},
                            {"seconds", elapsed.count()},
                            {"frames", frames.size()},
                            {"mean_frame_input_bytes", frame_bytes / frames.size()},
                            {"jobs", jobs},
                            {"failures", failures},
                            {"jobs_per_second", jobs / elapsed.count()},
                            {"input_gib_per_second", input / elapsed.count() / (1ULL << 30)},
                            {"latency_p50_s", percentile(0.5)},
                            {"latency_p99_s", percentile(0.99)},
                            {"latency_max_s", latencies.empty() ? 0.0 : latencies.back()}};
        std::cout << line.dump() << std::endl;
        return failures == 0 ? 0 : 2;
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
        std::string replay_dir;
        int clients = 1;
        double duration = 30;
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
            else if (name == "--replay")
                replay_dir = value;
            else if (name == "--clients")
                clients = std::stoi(value);
            else if (name == "--duration")
                duration = std::stod(value);
            else
                throw std::invalid_argument("unknown argument: " + name);
        }
        if (!replay_dir.empty())
            return replay(endpoint, replay_dir, clients, duration);
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
