// Background CPU load: one busy/sleep duty-cycle thread pinned to each listed CPU until SIGTERM or SIGINT.
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <pthread.h>
#include <sched.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <nlohmann/json.hpp>

namespace
{
    using Clock = std::chrono::steady_clock;

    std::uint64_t parse_number(const std::string &text, std::uint64_t minimum, std::uint64_t maximum, const std::string &name)
    {
        std::uint64_t value = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value < minimum || value > maximum)
            throw std::invalid_argument("invalid --" + name);
        return value;
    }

    std::vector<int> parse_cpus(const std::string &text)
    {
        std::vector<int> cpus;
        std::size_t start = 0;
        while (start <= text.size())
        {
            const auto end = std::min(text.find(',', start), text.size());
            cpus.push_back(static_cast<int>(parse_number(text.substr(start, end - start), 0, 65535, "cpus")));
            start = end + 1;
        }
        if (cpus.empty() || cpus.size() > 1024)
            throw std::invalid_argument("invalid --cpus");
        return cpus;
    }

    bool pin(int cpu)
    {
#if defined(__linux__)
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu, &set);
        return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#else
        (void)cpu;
        return false;
#endif
    }

    void write_ready(const std::filesystem::path &path, const nlohmann::json &document)
    {
        std::string name(1, '.');
        name.append(path.filename().string()).append(".tmp");
        const auto temporary = path.parent_path() / name;
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output << document.dump() << '\n';
            output.flush();
            if (!output)
                throw std::runtime_error("cannot write ready file");
        }
        std::filesystem::rename(temporary, path);
    }
}

int main(int argc, char **argv)
{
    try
    {
        std::map<std::string, std::string> arguments;
        if ((argc - 1) % 2 != 0)
            throw std::invalid_argument("arguments must be --name value pairs");
        for (int index = 1; index < argc; index += 2)
        {
            const std::string name = argv[index];
            if (name.size() < 3 || name.rfind("--", 0) != 0 || !arguments.emplace(name.substr(2), argv[index + 1]).second)
                throw std::invalid_argument("invalid or duplicate argument: " + name);
        }
        const auto take = [&](const std::string &name, const std::string &fallback)
        {
            const auto entry = arguments.find(name);
            if (entry == arguments.end())
                return fallback;
            auto value = entry->second;
            arguments.erase(entry);
            return value;
        };
        const auto cpus = parse_cpus(take("cpus", ""));
        const auto percent = parse_number(take("percent", ""), 1, 100, "percent");
        const auto period_ms = parse_number(take("period-ms", "10"), 1, 1000, "period-ms");
        const std::filesystem::path ready = take("ready-file", "");
        if (!arguments.empty())
            throw std::invalid_argument("unknown argument: --" + arguments.begin()->first);

        sigset_t signals;
        sigemptyset(&signals);
        sigaddset(&signals, SIGTERM);
        sigaddset(&signals, SIGINT);
        if (pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0)
            throw std::runtime_error("cannot block shutdown signals");

        const auto period = std::chrono::milliseconds(period_ms);
        const auto busy = std::chrono::duration_cast<Clock::duration>(period * static_cast<double>(percent) / 100.0);
        std::atomic<bool> stop{false};
        std::atomic<int> pinned{0};
        std::atomic<std::uint64_t> busy_ns{0};
        std::atomic<std::uint64_t> spins{0};
        std::vector<std::thread> threads;
        const auto started = Clock::now();
        for (const auto cpu : cpus)
        {
            threads.emplace_back([&, cpu]
                                 {
                pinned += pin(cpu);
                std::uint64_t spent = 0;
                std::uint64_t iterations = 0;
                auto next = Clock::now();
                while (!stop.load(std::memory_order_relaxed))
                {
                    const auto busy_until = next + busy;
                    next += period;
                    while (Clock::now() < busy_until)
                        ++iterations;
                    spent += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(busy).count());
                    if (percent < 100)
                        std::this_thread::sleep_until(next);
                    else
                        next = Clock::now();
                }
                busy_ns += spent;
                spins += iterations; });
        }
        // Give each thread a moment to apply its affinity before reporting readiness.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const bool all_pinned = pinned.load() == static_cast<int>(cpus.size());
        nlohmann::json report{{"schema_version", 1}, {"state", "ready"}, {"pid", static_cast<long>(getpid())}, {"cpus", cpus},
                              {"percent", percent}, {"period_ms", period_ms},
                              {"affinity", all_pinned ? "pinned" : "unsupported_or_failed"}};
        if (!ready.empty())
            write_ready(ready, report);
        int received = 0;
        while (sigwait(&signals, &received) != 0)
        {
        }
        stop.store(true);
        for (auto &thread : threads)
            thread.join();
        const auto wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count();
        report["state"] = "stopped";
        report["signal"] = received;
        report["wall_ns"] = wall_ns;
        report["busy_ns_total"] = busy_ns.load();
        report["spin_iterations"] = spins.load();
        std::cout << report.dump() << std::endl;
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << nlohmann::json{{"status", "error"}, {"detail", error.what()}}.dump() << '\n';
        return 1;
    }
}
