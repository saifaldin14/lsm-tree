#include "lsm_lab/telemetry.hpp"

#include <charconv>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <iostream>
#include <map>
#include <pthread.h>
#include <sstream>
#include <thread>
#include <unistd.h>

int main(int argc, char **argv)
{
    try
    {
        std::map<std::string, std::string> arguments;
        for (int index = 1; index < argc; index += 2)
        {
            if (index + 1 == argc || !std::string(argv[index]).starts_with("--") ||
                !arguments.emplace(std::string(argv[index]).substr(2), argv[index + 1]).second)
                throw std::invalid_argument("expected unique --name value arguments");
        }
        const auto take = [&](const std::string &key, const std::string &fallback)
        {
            const auto found = arguments.find(key);
            if (found == arguments.end())
                return fallback;
            auto value = found->second;
            arguments.erase(found);
            return value;
        };
        const auto number = [&](const std::string &key, std::uint64_t fallback)
        {
            const auto value = take(key, std::to_string(fallback));
            std::uint64_t parsed = 0;
            const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (result.ec != std::errc{} || result.ptr != value.data() + value.size())
                throw std::invalid_argument("invalid --" + key);
            return parsed;
        };
        lsm_lab::telemetry::SamplerOptions options;
        options.output = take("output", "");
        options.role = take("role", "storage");
        options.pid = number("pid", getpid());
        options.include_host = true;
        options.interval_ms = number("interval-ms", 1000);
        options.max_output_bytes = number("max-output-bytes", 256ULL << 20);
        const auto duration = number("duration-seconds", 0);
        const auto ready = take("ready-file", "");
        const auto required = take("require-metrics", "");
        if (!required.empty())
        {
            std::istringstream list(required);
            for (std::string name; std::getline(list, name, ',');)
            {
                if (name.empty() || name.find(':') == std::string::npos)
                    throw std::invalid_argument("required metrics must use scope:name");
                options.required_metrics.push_back(name);
            }
        }
        if (!arguments.empty() || duration > 604800)
            throw std::invalid_argument("unknown argument or duration exceeds seven days");
        sigset_t signals;
        sigemptyset(&signals);
        sigaddset(&signals, SIGINT);
        sigaddset(&signals, SIGTERM);
        if (pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0)
            throw std::runtime_error("cannot block shutdown signals");
        lsm_lab::telemetry::Sampler sampler(options);
        if (!ready.empty())
        {
            const auto text = nlohmann::json{{"state", "ready"}, {"schema_version", 1}, {"pid", getpid()}, {"host", lsm_lab::telemetry::host_identity()}}.dump();
            const auto temporary = ready + ".partial-" + std::to_string(getpid());
            const int descriptor = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            if (descriptor < 0)
                throw std::runtime_error("cannot create telemetry readiness file");
            std::size_t offset = 0;
            while (offset < text.size())
            {
                const auto written = write(descriptor, text.data() + offset, text.size() - offset);
                if (written < 0 && errno == EINTR)
                    continue;
                if (written <= 0)
                    break;
                offset += static_cast<std::size_t>(written);
            }
            const int sync_result = fsync(descriptor);
            close(descriptor);
            const bool installed = offset == text.size() && sync_result == 0 && link(temporary.c_str(), ready.c_str()) == 0;
            unlink(temporary.c_str());
            if (!installed)
                throw std::runtime_error("cannot persist telemetry readiness; destination may already exist");
        }
        if (duration)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(duration);
            while (std::chrono::steady_clock::now() < deadline)
            {
                sigset_t pending;
                if (sigpending(&pending) == 0 && (sigismember(&pending, SIGINT) || sigismember(&pending, SIGTERM)))
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        else
        {
            int signal = 0;
            if (sigwait(&signals, &signal) != 0)
                throw std::runtime_error("sigwait failed");
        }
        const auto summary = sampler.stop();
        std::cout << summary.dump() << '\n';
        return summary.at("status") == "complete" ? 0 : 3;
    }
    catch (const std::exception &error)
    {
        std::cerr << nlohmann::json{{"status", "error"}, {"detail", error.what()}}.dump() << '\n';
        return 1;
    }
}