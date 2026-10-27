// Replication backup: receives mirrored files (physical replication) or write batches
// (logical replication) from a primary over TCP or RDMA. See lsm_lab/replication.hpp.
//
//   lsm_replica_server --listen rdma://192.168.126.102:7481 --dir /dev/shm/$USER/replica
//
// The directory should start as a copy of the primary's initial database. Prints
// "listening <endpoint>" once ready.

#include "lsm_lab/replication.hpp"

#include <csignal>
#include <iostream>
#include <pthread.h>
#include <string>
#include <thread>

int main(int argc, char **argv)
{
    try
    {
        std::string listen = "tcp://0.0.0.0:7481";
        std::string directory;
        for (int index = 1; index + 1 < argc; index += 2)
        {
            const std::string name = argv[index];
            const std::string value = argv[index + 1];
            if (name == "--listen")
                listen = value;
            else if (name == "--dir")
                directory = value;
            else
                throw std::invalid_argument("unknown argument: " + name);
        }
        if (directory.empty())
            throw std::invalid_argument("--dir is required");
        sigset_t signals;
        sigemptyset(&signals);
        sigaddset(&signals, SIGINT);
        sigaddset(&signals, SIGTERM);
        pthread_sigmask(SIG_BLOCK, &signals, nullptr);
        auto endpoint = lsm_lab::parse_endpoint(listen);
        auto server = lsm_lab::listen_rpc(endpoint, lsm_lab::make_replica_handler(directory));
        std::thread waiter([&] {
            int signal = 0;
            sigwait(&signals, &signal);
            server->stop();
        });
        waiter.detach();
        endpoint.port = server->port();
        std::cout << "listening " << endpoint.text() << " dir " << directory << std::endl;
        server->run();
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "lsm_replica_server: " << error.what() << '\n';
        return 1;
    }
}
