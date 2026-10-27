// Remote compaction server: performs the merged input of compactions sent by database
// processes (remote_merge transform) over TCP or RDMA, with a local backend.
//
//   lsm_compaction_server --listen rdma://192.168.126.104:7471 --backend cuda_merge
//
// Prints "listening <endpoint>" once ready (port 0 picks a free port) and the backend's
// statistics as JSON on SIGINT/SIGTERM. --crash-after N (fault injection) terminates the
// process abruptly when request N+1 arrives.

#include "lsm_lab/remote_merge.hpp"

#include <atomic>
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
            else
                throw std::invalid_argument("unknown argument: " + name);
        }
        if (backend != "cpu_merge" && backend != "cuda_merge")
            throw std::invalid_argument("--backend must be cpu_merge or cuda_merge");
        // Signals are taken by one thread (sigwait), never inside a handler.
        sigset_t signals;
        sigemptyset(&signals);
        sigaddset(&signals, SIGINT);
        sigaddset(&signals, SIGTERM);
        pthread_sigmask(SIG_BLOCK, &signals, nullptr);
        auto transform = lsm_lab::make_transform(backend);
        auto endpoint = lsm_lab::parse_endpoint(listen);
        auto handler = lsm_lab::make_merge_handler(transform);
        if (crash_after >= 0)
            handler = [handler, crash_after, served = std::make_shared<std::atomic<long long>>(0)](
                          const char *data, std::size_t bytes, lsm_lab::ReplySink &sink) {
                if (served->fetch_add(1) >= crash_after)
                    std::_Exit(3);
                return handler(data, bytes, sink);
            };
        auto server = lsm_lab::listen_rpc(endpoint, std::move(handler));
        std::thread waiter([&] {
            int signal = 0;
            sigwait(&signals, &signal);
            server->stop();
        });
        waiter.detach();
        endpoint.port = server->port();
        std::cout << "listening " << endpoint.text() << " backend " << backend << " device " << transform->device()
                  << std::endl;
        server->run();
        std::cout << transform->statistics().dump() << std::endl;
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "lsm_compaction_server: " << error.what() << '\n';
        return 1;
    }
}
