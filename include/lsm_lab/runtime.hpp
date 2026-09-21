#pragma once

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include <grpcpp/grpcpp.h>
#include <nlohmann/json.hpp>
#include <rocksdb/env.h>

namespace lsm_lab
{
    constexpr std::uint32_t wire_version = 1;
    constexpr int maximum_rpc_bytes = 16 * 1024 * 1024;
    using Clock = std::chrono::steady_clock;

    void require(bool condition, const std::string &message);
    void check(const rocksdb::Status &status);
    std::string read_file(const std::filesystem::path &path, std::size_t maximum_bytes = maximum_rpc_bytes);
    std::string digest_bytes(std::string_view bytes);
    std::string digest_hex(std::string_view bytes);
    std::string engine_fingerprint();
    std::string unique_id();
    bool valid_id(std::string_view value);
    void sync_directory(const std::filesystem::path &path);
    void durable_bytes(const std::filesystem::path &path, std::string_view bytes);
    void durable_json(const std::filesystem::path &path, const nlohmann::json &document);
    std::filesystem::path contained_path(const std::filesystem::path &root, const std::filesystem::path &relative);
    nlohmann::json process_usage();

    class Arguments
    {
    public:
        Arguments(int argc, char **argv);
        std::string take(const std::string &name, const std::string &default_value = {});
        std::string required(const std::string &name);
        std::uint64_t number(const std::string &name, std::uint64_t default_value,
                             std::uint64_t minimum, std::uint64_t maximum);
        void finish() const;

    private:
        std::map<std::string, std::string> values_;
    };

    struct TlsOptions
    {
        std::filesystem::path ca;
        std::filesystem::path certificate;
        std::filesystem::path key;
        static TlsOptions take(Arguments &arguments, const std::string &prefix = {});
    };

    bool loopback_endpoint(std::string_view endpoint);
    std::shared_ptr<grpc::ServerCredentials> server_credentials(const std::string &endpoint, const TlsOptions &tls);
    std::shared_ptr<grpc::Channel> connect(const std::string &endpoint, const TlsOptions &tls);
    void block_shutdown_signals();
    void wait_for_shutdown();
    std::unique_ptr<grpc::Server> start_server(const std::string &endpoint, const TlsOptions &tls,
                                               grpc::Service &service, const std::filesystem::path &ready_file);

    template <typename Operation>
    grpc::Status rpc_call(grpc::ServerContext &context, Operation operation)
    {
        if (context.IsCancelled())
        {
            return {grpc::StatusCode::CANCELLED, "request cancelled before execution"};
        }
        try
        {
            operation();
            return grpc::Status::OK;
        }
        catch (const std::exception &error)
        {
            return {grpc::StatusCode::FAILED_PRECONDITION, error.what()};
        }
        catch (...)
        {
            return {grpc::StatusCode::INTERNAL, "unhandled service failure"};
        }
    }

    class EventLog
    {
    public:
        explicit EventLog(const std::filesystem::path &path);
        void append(nlohmann::json event);

    private:
        std::mutex mutex_;
        std::unique_ptr<rocksdb::WritableFile> file_;
        Clock::time_point start_ = Clock::now();
        std::uint64_t sequence_ = 0;
    };
}