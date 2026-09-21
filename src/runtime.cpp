#include "lsm_lab/runtime.hpp"

#include <array>
#include <charconv>
#include <csignal>
#include <fstream>
#include <pthread.h>
#include <stdexcept>
#include <sys/resource.h>
#include <unistd.h>

#include <openssl/evp.h>

namespace lsm_lab
{
    namespace fs = std::filesystem;

    void require(bool condition, const std::string &message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    void check(const rocksdb::Status &status)
    {
        require(status.ok(), status.ToString());
    }

    std::string read_file(const fs::path &path, std::size_t maximum_bytes)
    {
        require(fs::is_regular_file(path), "not a regular file: " + path.string());
        require(fs::file_size(path) <= maximum_bytes, "file exceeds size limit: " + path.string());
        std::ifstream input(path, std::ios::binary);
        require(input.good(), "cannot read file: " + path.string());
        std::string result(maximum_bytes + 1, '\0');
        input.read(result.data(), static_cast<std::streamsize>(result.size()));
        require(!input.bad(), "file read failed: " + path.string());
        result.resize(static_cast<std::size_t>(input.gcount()));
        require(result.size() <= maximum_bytes, "file grew beyond size limit: " + path.string());
        return result;
    }

    std::string digest_bytes(std::string_view bytes)
    {
        std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
        unsigned int size = 0;
        require(EVP_Digest(bytes.data(), bytes.size(), digest.data(), &size, EVP_sha256(), nullptr) == 1 && size == 32,
                "SHA-256 failed");
        return {reinterpret_cast<const char *>(digest.data()), size};
    }

    std::string digest_hex(std::string_view bytes)
    {
        const auto digest = digest_bytes(bytes);
        constexpr char digits[] = "0123456789abcdef";
        std::string result;
        result.reserve(digest.size() * 2);
        for (unsigned char byte : digest)
        {
            result += digits[byte >> 4];
            result += digits[byte & 15];
        }
        return result;
    }

    std::string engine_fingerprint()
    {
        return LSM_ENGINE_FINGERPRINT;
    }

    std::string unique_id()
    {
        return rocksdb::Env::Default()->GenerateUniqueId();
    }

    bool valid_id(std::string_view value)
    {
        if (value.empty() || value.size() > 128 || value == "." || value == "..")
        {
            return false;
        }
        for (const auto byte : value)
        {
            if (!((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                  (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.'))
            {
                return false;
            }
        }
        return true;
    }

    void sync_directory(const fs::path &path)
    {
        std::unique_ptr<rocksdb::Directory> directory;
        check(rocksdb::Env::Default()->NewDirectory(path.string(), &directory));
        check(directory->Fsync());
    }

    void durable_bytes(const fs::path &path, std::string_view bytes)
    {
        auto *env = rocksdb::Env::Default();
        const auto temporary = path.parent_path() / (".pending-" + unique_id());
        std::unique_ptr<rocksdb::WritableFile> file;
        check(env->NewWritableFile(temporary.string(), &file, rocksdb::EnvOptions()));
        check(file->Append(rocksdb::Slice(bytes.data(), bytes.size())));
        check(file->Fsync());
        check(file->Close());
        check(env->RenameFile(temporary.string(), path.string()));
        sync_directory(path.parent_path());
    }

    void durable_json(const fs::path &path, const nlohmann::json &document)
    {
        durable_bytes(path, document.dump() + "\n");
    }

    fs::path contained_path(const fs::path &root, const fs::path &relative)
    {
        require(!relative.empty() && !relative.is_absolute(), "path must be relative to shared root");
        for (const auto &component : relative)
        {
            require(component != ".." && component != ".", "path traversal is forbidden");
        }
        const auto base = fs::canonical(root);
        const auto path = fs::canonical(base / relative);
        const auto suffix = path.lexically_relative(base);
        require(!suffix.empty() && suffix != "." && *suffix.begin() != "..", "path escapes shared root");
        return path;
    }

    nlohmann::json process_usage()
    {
        rusage usage{};
        require(getrusage(RUSAGE_SELF, &usage) == 0, "getrusage failed");
        const auto micros = [](const timeval &value)
        {
            return static_cast<std::uint64_t>(value.tv_sec) * 1000000 + value.tv_usec;
        };
#ifdef __APPLE__
        const auto rss_bytes = static_cast<std::uint64_t>(usage.ru_maxrss);
#else
        const auto rss_bytes = static_cast<std::uint64_t>(usage.ru_maxrss) * 1024;
#endif
        return {{"user_cpu_us", micros(usage.ru_utime)}, {"system_cpu_us", micros(usage.ru_stime)}, {"peak_rss_bytes", rss_bytes}, {"block_input_operations", usage.ru_inblock}, {"block_output_operations", usage.ru_oublock}};
    }

    Arguments::Arguments(int argc, char **argv)
    {
        require((argc - 1) % 2 == 0, "arguments must be --name value pairs");
        for (int index = 1; index < argc; index += 2)
        {
            const std::string name = argv[index];
            require(name.starts_with("--") && name.size() > 2, "expected --name argument");
            require(values_.emplace(name.substr(2), argv[index + 1]).second, "duplicate argument: " + name);
        }
    }

    std::string Arguments::take(const std::string &name, const std::string &default_value)
    {
        const auto entry = values_.find(name);
        if (entry == values_.end())
        {
            return default_value;
        }
        auto value = entry->second;
        values_.erase(entry);
        return value;
    }

    std::string Arguments::required(const std::string &name)
    {
        auto result = take(name);
        require(!result.empty(), "missing --" + name);
        return result;
    }

    std::uint64_t Arguments::number(const std::string &name, std::uint64_t default_value,
                                    std::uint64_t minimum, std::uint64_t maximum)
    {
        const auto text = take(name, std::to_string(default_value));
        std::uint64_t result = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
        require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
                    result >= minimum && result <= maximum,
                "invalid numeric --" + name);
        return result;
    }

    void Arguments::finish() const
    {
        require(values_.empty(), values_.empty() ? "" : "unknown argument: --" + values_.begin()->first);
    }

    TlsOptions TlsOptions::take(Arguments &arguments, const std::string &prefix)
    {
        return {arguments.take(prefix + "tls-ca"), arguments.take(prefix + "tls-cert"),
                arguments.take(prefix + "tls-key")};
    }

    bool loopback_endpoint(std::string_view endpoint)
    {
        const auto colon = endpoint.rfind(':');
        if (colon == std::string_view::npos)
        {
            return false;
        }
        const auto host = endpoint.substr(0, colon);
        const auto port = endpoint.substr(colon + 1);
        unsigned int number = 0;
        const auto parsed = std::from_chars(port.data(), port.data() + port.size(), number);
        return (host == "127.0.0.1" || host == "[::1]") && !port.empty() &&
               parsed.ec == std::errc{} && parsed.ptr == port.data() + port.size() && number <= 65535;
    }

    std::shared_ptr<grpc::ServerCredentials> server_credentials(const std::string &endpoint, const TlsOptions &tls)
    {
        if (tls.ca.empty() && tls.certificate.empty() && tls.key.empty())
        {
            require(loopback_endpoint(endpoint), "non-loopback servers require mutual TLS");
            return grpc::InsecureServerCredentials();
        }
        require(!tls.ca.empty() && !tls.certificate.empty() && !tls.key.empty(), "TLS requires CA, certificate, and key");
        grpc::SslServerCredentialsOptions options;
        options.pem_root_certs = read_file(tls.ca, 1 << 20);
        options.pem_key_cert_pairs.push_back({read_file(tls.key, 1 << 20), read_file(tls.certificate, 1 << 20)});
        options.client_certificate_request = GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY;
        return grpc::SslServerCredentials(options);
    }

    std::shared_ptr<grpc::Channel> connect(const std::string &endpoint, const TlsOptions &tls)
    {
        std::shared_ptr<grpc::ChannelCredentials> credentials;
        if (tls.ca.empty() && tls.certificate.empty() && tls.key.empty())
        {
            require(loopback_endpoint(endpoint), "non-loopback clients require mutual TLS");
            credentials = grpc::InsecureChannelCredentials();
        }
        else
        {
            require(!tls.ca.empty() && !tls.certificate.empty() && !tls.key.empty(), "TLS requires CA, certificate, and key");
            grpc::SslCredentialsOptions options;
            options.pem_root_certs = read_file(tls.ca, 1 << 20);
            options.pem_cert_chain = read_file(tls.certificate, 1 << 20);
            options.pem_private_key = read_file(tls.key, 1 << 20);
            credentials = grpc::SslCredentials(options);
        }
        grpc::ChannelArguments arguments;
        arguments.SetMaxReceiveMessageSize(maximum_rpc_bytes);
        arguments.SetMaxSendMessageSize(maximum_rpc_bytes);
        arguments.SetInt(GRPC_ARG_ENABLE_RETRIES, 0);
        return grpc::CreateCustomChannel(endpoint, credentials, arguments);
    }

    void block_shutdown_signals()
    {
        sigset_t signals;
        sigemptyset(&signals);
        sigaddset(&signals, SIGINT);
        sigaddset(&signals, SIGTERM);
        require(pthread_sigmask(SIG_BLOCK, &signals, nullptr) == 0, "cannot block shutdown signals");
    }

    void wait_for_shutdown()
    {
        sigset_t signals;
        sigemptyset(&signals);
        sigaddset(&signals, SIGINT);
        sigaddset(&signals, SIGTERM);
        int received = 0;
        require(sigwait(&signals, &received) == 0, "cannot wait for shutdown signal");
    }

    std::unique_ptr<grpc::Server> start_server(const std::string &endpoint, const TlsOptions &tls,
                                               grpc::Service &service, const fs::path &ready_file)
    {
        require(!fs::exists(ready_file), "readiness file already exists");
        grpc::ServerBuilder builder;
        builder.SetMaxReceiveMessageSize(maximum_rpc_bytes);
        builder.SetMaxSendMessageSize(maximum_rpc_bytes);
        int selected_port = 0;
        builder.AddListeningPort(endpoint, server_credentials(endpoint, tls), &selected_port);
        builder.RegisterService(&service);
        auto server = builder.BuildAndStart();
        require(server != nullptr && selected_port != 0, "cannot start RPC listener: " + endpoint);
        const auto actual_endpoint = endpoint.substr(0, endpoint.rfind(':') + 1) + std::to_string(selected_port);
        durable_json(ready_file, {{"schema_version", 1}, {"state", "ready"}, {"pid", getpid()}, {"endpoint", actual_endpoint}, {"engine_fingerprint", engine_fingerprint()}});
        return server;
    }

    EventLog::EventLog(const fs::path &path)
    {
        require(!fs::exists(path), "event log already exists: " + path.string());
        check(rocksdb::Env::Default()->NewWritableFile(path.string(), &file_, rocksdb::EnvOptions()));
        sync_directory(path.parent_path());
    }

    void EventLog::append(nlohmann::json event)
    {
        const std::lock_guard guard(mutex_);
        event["schema_version"] = 1;
        event["event_sequence"] = sequence_++;
        event["elapsed_ns"] = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start_).count();
        check(file_->Append(event.dump() + "\n"));
        check(file_->Fsync());
    }
}