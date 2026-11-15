// Remote compaction server: performs the merged input of compactions sent by database
// processes (remote_merge transform) over TCP or RDMA, with a local backend.
//
//   lsm_compaction_server --listen rdma://192.168.126.104:7471 --backend cuda_merge
//
// Prints "listening <endpoint>" once ready (port 0 picks a free port) and the backend's
// statistics as JSON on SIGINT/SIGTERM. --crash-after N (fault injection) terminates the
// process abruptly when request N+1 arrives. --backend remote_merge relays each request to
// the server at LSM_REMOTE_ENDPOINT (e.g. from an RDMA network to a GPU host on another one).
// With --backend cuda_merge, one process serves every client from one set of GPU engines:
// --engines K jobs run at once (LSM_GPU_MERGE_STREAMS) and --scheduler fifo|fair|stall decides
// which waiting job runs next (LSM_GPU_SCHEDULER). With --backend cpu_merge, --engines K bounds the
// merges running at once on the CPU (LSM_CPU_MERGE_SLOTS; unbounded without it) and --scheduler
// orders the waiting ones (LSM_CPU_MERGE_SCHEDULER). unix://<path> serves clients on the same
// machine through shared memory. --drain-seconds S (default 0): on SIGTERM, new merge requests are
// refused with Busy (clients retry them on another connection) while running merges finish, for at
// most S seconds; then the server stops and prints its statistics, with "drain_refused" and
// "drain_complete" added.

#include "lsm_lab/remote_merge.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <pthread.h>
#include <string>
#include <thread>

int main(int argc, char **argv)
{
    try
    {
        std::string listen = "tcp://0.0.0.0:7471";
        std::string backend = "cpu_merge";
        long long crash_after = -1;
        std::string scheduler, engines;
        double drain_seconds = 0;
        for (int index = 1; index < argc; ++index)
        {
            const std::string name = argv[index];
            if (index + 1 >= argc)
                throw std::invalid_argument("missing value for " + name);
            const std::string value = argv[++index];
            if (name == "--listen")
                listen = value;
            else if (name == "--backend")
                backend = value;
            else if (name == "--crash-after")
                crash_after = std::stoll(value);
            else if (name == "--scheduler")
                scheduler = value;
            else if (name == "--engines")
                engines = value;
            else if (name == "--drain-seconds")
                drain_seconds = std::stod(value);
            else
                throw std::invalid_argument("unknown argument: " + name);
        }
        if (backend != "cpu_merge" && backend != "cuda_merge" && backend != "remote_merge")
            throw std::invalid_argument("--backend must be cpu_merge, cuda_merge or remote_merge");
        // Signals are taken by one thread (sigwait), never inside a handler.
        sigset_t signals;
        sigemptyset(&signals);
        sigaddset(&signals, SIGINT);
        sigaddset(&signals, SIGTERM);
        pthread_sigmask(SIG_BLOCK, &signals, nullptr);
        if (!scheduler.empty())
            setenv(backend == "cpu_merge" ? "LSM_CPU_MERGE_SCHEDULER" : "LSM_GPU_SCHEDULER", scheduler.c_str(), 1);
        if (!engines.empty())
            setenv(backend == "cpu_merge" ? "LSM_CPU_MERGE_SLOTS" : "LSM_GPU_MERGE_STREAMS", engines.c_str(), 1);
        auto transform = lsm_lab::make_transform(backend);
        auto endpoint = lsm_lab::parse_endpoint(listen);
        lsm_lab::Drain drain;
        auto handler = drain.wrap(lsm_lab::make_merge_handler(transform));
        if (crash_after >= 0)
            handler = [handler, crash_after, served = std::make_shared<std::atomic<long long>>(0)](
                          const char *data, std::size_t bytes, lsm_lab::ReplySink &sink) {
                if (served->fetch_add(1) >= crash_after)
                    std::_Exit(3);
                return handler(data, bytes, sink);
            };
        auto server = lsm_lab::listen_rpc(endpoint, std::move(handler));
        std::atomic<bool> drained{true};
        std::thread waiter([&] {
            int signal = 0;
            sigwait(&signals, &signal);
            if (signal == SIGTERM && drain_seconds > 0)
            {
                drain.begin();
                drained = drain.wait(std::chrono::milliseconds(static_cast<long long>(drain_seconds * 1000)));
            }
            server->stop();
        });
        waiter.detach();
        endpoint.port = server->port();
        std::cout << "listening " << endpoint.text() << " backend " << backend << " device " << transform->device()
                  << std::endl;
        server->run();
        auto statistics = transform->statistics();
        statistics["drain_refused"] = drain.refused();
        statistics["drain_complete"] = drained.load();
        std::cout << statistics.dump() << std::endl;
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "lsm_compaction_server: " << error.what() << '\n';
        return 1;
    }
}
