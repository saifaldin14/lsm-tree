#include "lsm_lab/load.hpp"

#include <iostream>
#include <limits>

int main(int argc, char **argv)
{
    try
    {
        lsm_lab::Arguments arguments(argc, argv);
        lsm_lab::LoadOptions options;
        options.endpoint = arguments.required("endpoint");
        options.run_id = arguments.required("run-id");
        options.output = arguments.required("output");
        options.required_mode = arguments.take("require-mode");
        options.closed_loop_clients = static_cast<std::uint32_t>(arguments.number("closed-loop", 0, 0, 1024));
        if (options.closed_loop_clients == 0)
        {
            options.requests = arguments.number("requests", 1000, 1, 1'000'000'000);
            options.warmup_requests = arguments.number("warmup-requests", 0, 0, 1'000'000'000);
            options.rate = arguments.number("rate", 1000, 1, 10'000'000);
            options.threads = arguments.number("threads", 4, 1, 64);
            options.queue_capacity = arguments.number("queue-capacity", 256, 1, 65536);
        }
        else
        {
            options.duration_seconds = arguments.number("duration-seconds", 10, 1, 86400);
            options.warmup_duration_seconds = arguments.number("warmup-duration-seconds", 0, 0, 86400);
        }
        options.seed = arguments.number("seed", 1, 0, std::numeric_limits<std::uint64_t>::max());
        options.read_percent = arguments.number("read-percent", 50, 0, 100);
        options.insert_percent = arguments.number("insert-percent", 0, 0, 100);
        options.deadline_ms = arguments.number("deadline-ms", 5000, 1, 60000);
        options.write_budget_bytes = arguments.number("write-budget-bytes", 64ULL << 20, 1, 1ULL << 50);
        options.memory_budget_bytes = arguments.number("memory-budget-bytes", 256ULL << 20, 16ULL << 20, 64ULL << 30);
        options.artifact_budget_bytes = arguments.number("artifact-budget-bytes", 2ULL << 30, 1, 1ULL << 50);
        options.tls = lsm_lab::TlsOptions::take(arguments);
        arguments.finish();
        const auto result = options.closed_loop_clients == 0 ? lsm_lab::run_load(options) : lsm_lab::run_closed_loop(options);
        std::cout << result.dump(2) << '\n';
        return result.at("valid") == true ? 0 : 3;
    }
    catch (const std::exception &error)
    {
        std::cerr << nlohmann::json{{"status", "error"}, {"detail", error.what()}}.dump() << '\n';
        return 1;
    }
}