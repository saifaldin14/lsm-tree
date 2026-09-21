#include "lsm_lab/worker.hpp"
#include "compaction.grpc.pb.h"

#include <iostream>

namespace
{
    namespace wire = lsm_lab::rpc::v1;

    class WorkerService final : public wire::CompactionWorkerService::Service
    {
    public:
        explicit WorkerService(lsm_lab::Worker &worker) : worker_(worker) {}

        grpc::Status Capabilities(grpc::ServerContext *context, const wire::WorkerCapabilitiesRequest *request,
                                  wire::WorkerCapabilitiesResponse *response) override
        {
            return lsm_lab::rpc_call(*context, [&]
                                     {
                lsm_lab::require(request->wire_version() == lsm_lab::wire_version &&
                                     request->engine_fingerprint() == lsm_lab::engine_fingerprint(),
                                 "incompatible wire or engine version");
                *response = worker_.capabilities(); });
        }

        grpc::Status Submit(grpc::ServerContext *context, const wire::SubmitCompactionRequest *request,
                            wire::CompactionAttempt *response) override
        {
            return lsm_lab::rpc_call(*context, [&]
                                     { *response = worker_.submit(*request); });
        }

        grpc::Status Inspect(grpc::ServerContext *context, const wire::AttemptRequest *request,
                             wire::CompactionAttempt *response) override
        {
            return lsm_lab::rpc_call(*context, [&]
                                     { *response = worker_.inspect(*request); });
        }

        grpc::Status Cancel(grpc::ServerContext *context, const wire::AttemptRequest *request,
                            wire::CompactionAttempt *response) override
        {
            return lsm_lab::rpc_call(*context, [&]
                                     { *response = worker_.cancel(*request); });
        }

        grpc::Status Acknowledge(grpc::ServerContext *context, const wire::InstallationAcknowledgement *request,
                                 wire::CompactionAttempt *response) override
        {
            return lsm_lab::rpc_call(*context, [&]
                                     { *response = worker_.acknowledge(*request); });
        }

    private:
        lsm_lab::Worker &worker_;
    };
}

int main(int argc, char **argv)
{
    try
    {
        lsm_lab::Arguments arguments(argc, argv);
        lsm_lab::WorkerOptions options;
        options.shared_root = arguments.required("shared-root");
        options.journal = arguments.required("journal");
        options.threads = arguments.number("threads", 1, 1, 64);
        options.queue_limit = arguments.number("queue-limit", 16, 1, 1024);
        const auto endpoint = arguments.take("listen", "127.0.0.1:50052");
        const auto ready = arguments.required("ready-file");
        const auto tls = lsm_lab::TlsOptions::take(arguments);
        arguments.finish();
        lsm_lab::block_shutdown_signals();
        lsm_lab::Worker worker(std::move(options));
        WorkerService service(worker);
        auto server = lsm_lab::start_server(endpoint, tls, service, ready);
        lsm_lab::wait_for_shutdown();
        server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(5));
        worker.stop();
        server->Wait();
        lsm_lab::durable_json(ready, {{"state", "stopped"}, {"schema_version", 1}});
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << nlohmann::json{{"status", "error"}, {"detail", error.what()}}.dump() << '\n';
        return 1;
    }
}