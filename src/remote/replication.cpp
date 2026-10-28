#include "lsm_lab/replication.hpp"

#include <rocksdb/convenience.h>
#include <rocksdb/options.h>

#include "util/hash.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <sys/resource.h>
#include <unistd.h>

namespace lsm_lab
{
    namespace
    {
        struct ReplicaHeader
        {
            std::uint32_t magic = 0x4c534d50; // "LSMP"
            ReplicaOp op = ReplicaOp::append;
            std::uint64_t path_bytes = 0;
            std::uint64_t offset = 0;
            std::uint64_t data_bytes = 0;
        };

        struct ReplicaReply
        {
            std::uint32_t magic = 0x4c534d51;
            std::uint32_t ok = 1;
            std::uint64_t message_bytes = 0;
        };

        double cpu_seconds()
        {
            rusage usage{};
            getrusage(RUSAGE_SELF, &usage);
            return usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6 + usage.ru_stime.tv_sec +
                   usage.ru_stime.tv_usec / 1e6;
        }

        rocksdb::IOStatus io_status(const rocksdb::Status &status)
        {
            return status.ok() ? rocksdb::IOStatus::OK() : rocksdb::IOStatus::IOError(status.ToString());
        }

        // Info logs are local diagnostics, not database state.
        bool replicated_name(const std::string &relative)
        {
            return relative != "LOG" && relative.rfind("LOG.old", 0) != 0 && relative != "LOCK";
        }

        class ReplicatingWritableFile final : public rocksdb::FSWritableFileOwnerWrapper
        {
        public:
            ReplicatingWritableFile(std::unique_ptr<rocksdb::FSWritableFile> target, std::string relative, bool wal,
                                    std::shared_ptr<ReplicaSet> replicas, std::uint64_t offset)
                : FSWritableFileOwnerWrapper(std::move(target)), relative_(std::move(relative)), wal_(wal),
                  replicas_(std::move(replicas)), offset_(offset)
            {
            }

            rocksdb::IOStatus Append(const rocksdb::Slice &data, const rocksdb::IOOptions &options,
                                     rocksdb::IODebugContext *dbg) override
            {
                auto status = FSWritableFileOwnerWrapper::Append(data, options, dbg);
                return status.ok() ? buffer(data) : status;
            }

            rocksdb::IOStatus Append(const rocksdb::Slice &data, const rocksdb::IOOptions &options,
                                     const rocksdb::DataVerificationInfo &verification,
                                     rocksdb::IODebugContext *dbg) override
            {
                auto status = FSWritableFileOwnerWrapper::Append(data, options, verification, dbg);
                return status.ok() ? buffer(data) : status;
            }

            rocksdb::IOStatus PositionedAppend(const rocksdb::Slice &data, std::uint64_t offset,
                                               const rocksdb::IOOptions &options,
                                               rocksdb::IODebugContext *dbg) override
            {
                auto status = FSWritableFileOwnerWrapper::PositionedAppend(data, offset, options, dbg);
                return status.ok() ? positioned(data, offset) : status;
            }

            rocksdb::IOStatus PositionedAppend(const rocksdb::Slice &data, std::uint64_t offset,
                                               const rocksdb::IOOptions &options,
                                               const rocksdb::DataVerificationInfo &verification,
                                               rocksdb::IODebugContext *dbg) override
            {
                auto status = FSWritableFileOwnerWrapper::PositionedAppend(data, offset, options, verification, dbg);
                return status.ok() ? positioned(data, offset) : status;
            }

            rocksdb::IOStatus Flush(const rocksdb::IOOptions &options, rocksdb::IODebugContext *dbg) override
            {
                auto status = FSWritableFileOwnerWrapper::Flush(options, dbg);
                // WAL data reaches every backup before the write is acknowledged.
                return status.ok() && wal_ ? ship() : status;
            }

            rocksdb::IOStatus Sync(const rocksdb::IOOptions &options, rocksdb::IODebugContext *dbg) override
            {
                auto status = FSWritableFileOwnerWrapper::Sync(options, dbg);
                return status.ok() ? ship() : status;
            }

            rocksdb::IOStatus Fsync(const rocksdb::IOOptions &options, rocksdb::IODebugContext *dbg) override
            {
                auto status = FSWritableFileOwnerWrapper::Fsync(options, dbg);
                return status.ok() ? ship() : status;
            }

            rocksdb::IOStatus Truncate(std::uint64_t size, const rocksdb::IOOptions &options,
                                       rocksdb::IODebugContext *dbg) override
            {
                auto status = FSWritableFileOwnerWrapper::Truncate(size, options, dbg);
                if (!status.ok())
                    return status;
                status = ship();
                if (!status.ok())
                    return status;
                offset_ = size;
                return io_status(replicas_->broadcast(ReplicaOp::truncate, relative_, size, nullptr, 0));
            }

            rocksdb::IOStatus Close(const rocksdb::IOOptions &options, rocksdb::IODebugContext *dbg) override
            {
                auto status = FSWritableFileOwnerWrapper::Close(options, dbg);
                if (!status.ok() || closed_)
                    return status;
                closed_ = true;
                status = ship();
                if (!status.ok())
                    return status;
                return io_status(replicas_->broadcast(ReplicaOp::close, relative_, 0, nullptr, 0));
            }

        private:
            static constexpr std::size_t kChunk = 4ULL << 20;

            rocksdb::IOStatus buffer(const rocksdb::Slice &data)
            {
                if (pending_.empty())
                    pending_offset_ = offset_;
                pending_.append(data.data(), data.size());
                offset_ += data.size();
                return pending_.size() >= kChunk ? ship() : rocksdb::IOStatus::OK();
            }

            rocksdb::IOStatus positioned(const rocksdb::Slice &data, std::uint64_t offset)
            {
                auto status = ship();
                if (!status.ok())
                    return status;
                offset_ = std::max(offset_, offset + data.size());
                return io_status(replicas_->broadcast(ReplicaOp::append, relative_, offset, data.data(), data.size()));
            }

            rocksdb::IOStatus ship()
            {
                if (pending_.empty())
                    return rocksdb::IOStatus::OK();
                const auto status = replicas_->broadcast(ReplicaOp::append, relative_, pending_offset_, pending_.data(),
                                                         pending_.size());
                pending_.clear();
                return io_status(status);
            }

            std::string relative_;
            bool wal_;
            std::shared_ptr<ReplicaSet> replicas_;
            std::uint64_t offset_;
            std::uint64_t pending_offset_ = 0;
            std::string pending_;
            bool closed_ = false;
        };

        class ReplicatingFileSystem final : public rocksdb::FileSystemWrapper
        {
        public:
            ReplicatingFileSystem(std::shared_ptr<rocksdb::FileSystem> base, std::string root,
                                  std::shared_ptr<ReplicaSet> replicas)
                : FileSystemWrapper(std::move(base)), root_(std::move(root)), replicas_(std::move(replicas))
            {
                while (root_.size() > 1 && root_.back() == '/')
                    root_.pop_back();
            }

            const char *Name() const override { return "ReplicatingFileSystem"; }

            rocksdb::IOStatus NewWritableFile(const std::string &name, const rocksdb::FileOptions &options,
                                              std::unique_ptr<rocksdb::FSWritableFile> *result,
                                              rocksdb::IODebugContext *dbg) override
            {
                auto status = target()->NewWritableFile(name, options, result, dbg);
                std::string relative;
                if (!status.ok() || !inside(name, &relative))
                    return status;
                status = io_status(replicas_->broadcast(ReplicaOp::create, relative, 0, nullptr, 0));
                if (status.ok())
                    *result = wrap(std::move(*result), relative, 0);
                return status;
            }

            rocksdb::IOStatus ReopenWritableFile(const std::string &name, const rocksdb::FileOptions &options,
                                                 std::unique_ptr<rocksdb::FSWritableFile> *result,
                                                 rocksdb::IODebugContext *dbg) override
            {
                auto status = target()->ReopenWritableFile(name, options, result, dbg);
                std::string relative;
                if (status.ok() && inside(name, &relative))
                {
                    const auto size = (*result)->GetFileSize(rocksdb::IOOptions(), dbg);
                    *result = wrap(std::move(*result), relative, size);
                }
                return status;
            }

            rocksdb::IOStatus RenameFile(const std::string &source, const std::string &target_name,
                                         const rocksdb::IOOptions &options, rocksdb::IODebugContext *dbg) override
            {
                auto status = target()->RenameFile(source, target_name, options, dbg);
                std::string from, to;
                if (!status.ok() || !inside(source, &from) || !inside(target_name, &to))
                    return status;
                return io_status(replicas_->broadcast(ReplicaOp::rename, from, 0, to.data(), to.size()));
            }

            rocksdb::IOStatus LinkFile(const std::string &source, const std::string &target_name,
                                       const rocksdb::IOOptions &options, rocksdb::IODebugContext *dbg) override
            {
                auto status = target()->LinkFile(source, target_name, options, dbg);
                std::string from, to;
                if (!status.ok() || !inside(source, &from) || !inside(target_name, &to))
                    return status;
                return io_status(replicas_->broadcast(ReplicaOp::link, from, 0, to.data(), to.size()));
            }

            rocksdb::IOStatus DeleteFile(const std::string &name, const rocksdb::IOOptions &options,
                                         rocksdb::IODebugContext *dbg) override
            {
                auto status = target()->DeleteFile(name, options, dbg);
                std::string relative;
                if (!status.ok() || !inside(name, &relative))
                    return status;
                return io_status(replicas_->broadcast(ReplicaOp::remove, relative, 0, nullptr, 0));
            }

            rocksdb::IOStatus Truncate(const std::string &name, std::size_t size, const rocksdb::IOOptions &options,
                                       rocksdb::IODebugContext *dbg) override
            {
                auto status = target()->Truncate(name, size, options, dbg);
                std::string relative;
                if (!status.ok() || !inside(name, &relative))
                    return status;
                return io_status(replicas_->broadcast(ReplicaOp::truncate, relative, size, nullptr, 0));
            }

        private:
            bool inside(const std::string &name, std::string *relative) const
            {
                if (name.size() <= root_.size() + 1 || name.compare(0, root_.size(), root_) != 0 ||
                    name[root_.size()] != '/')
                    return false;
                *relative = name.substr(root_.size() + 1);
                return relative->find('/') == std::string::npos && replicated_name(*relative);
            }

            std::unique_ptr<rocksdb::FSWritableFile> wrap(std::unique_ptr<rocksdb::FSWritableFile> file,
                                                          const std::string &relative, std::uint64_t offset)
            {
                const bool wal = relative.size() > 4 && relative.compare(relative.size() - 4, 4, ".log") == 0;
                return std::make_unique<ReplicatingWritableFile>(std::move(file), relative, wal, replicas_, offset);
            }

            std::string root_;
            std::shared_ptr<ReplicaSet> replicas_;
        };

        // ---- backup side

        class Replica
        {
        public:
            explicit Replica(std::string directory) : directory_(std::move(directory))
            {
                std::filesystem::create_directories(directory_);
                started_cpu_ = cpu_seconds();
            }

            ~Replica()
            {
                for (auto &[name, fd] : files_)
                    ::close(fd);
            }

            // Returns the reply text; throws on failure.
            std::string apply(const ReplicaHeader &header, const std::string &path, const char *data)
            {
                ops_.fetch_add(1, std::memory_order_relaxed);
                bytes_.fetch_add(header.data_bytes, std::memory_order_relaxed);
                switch (header.op)
                {
                case ReplicaOp::create:
                    replace(path, open_file(path, O_CREAT | O_TRUNC | O_WRONLY));
                    return {};
                case ReplicaOp::append:
                    write_at(file(path), header.offset, data, header.data_bytes);
                    return {};
                case ReplicaOp::close:
                    replace(path, -1);
                    return {};
                case ReplicaOp::truncate:
                    if (::ftruncate(file(path), static_cast<off_t>(header.offset)) != 0)
                        fail("ftruncate " + path);
                    return {};
                case ReplicaOp::rename:
                {
                    const std::string target(data, header.data_bytes);
                    check_name(target);
                    replace(path, -1);
                    if (::rename(full(path).c_str(), full(target).c_str()) != 0)
                        fail("rename " + path);
                    return {};
                }
                case ReplicaOp::link:
                {
                    const std::string target(data, header.data_bytes);
                    check_name(target);
                    if (::link(full(path).c_str(), full(target).c_str()) != 0)
                        fail("link " + path);
                    return {};
                }
                case ReplicaOp::remove:
                    replace(path, -1);
                    if (::unlink(full(path).c_str()) != 0 && errno != ENOENT)
                        fail("unlink " + path);
                    return {};
                case ReplicaOp::open_database:
                    open_database(nlohmann::json::parse(std::string(data, header.data_bytes)));
                    return {};
                case ReplicaOp::write_batch:
                {
                    if (!db_)
                        throw std::runtime_error("write batch before open_database");
                    rocksdb::WriteBatch batch(std::string(data, header.data_bytes));
                    const auto status = db_->Write(write_options_, &batch);
                    if (!status.ok())
                        throw std::runtime_error("replica write: " + status.ToString());
                    return {};
                }
                case ReplicaOp::digest:
                    return digest();
                case ReplicaOp::statistics:
                    return statistics().dump();
                case ReplicaOp::wait:
                    wait_for_compaction();
                    return {};
                case ReplicaOp::reset:
                    reset();
                    return {};
                }
                throw std::runtime_error("unknown replica operation");
            }

        private:
            [[noreturn]] static void fail(const std::string &what)
            {
                throw std::runtime_error("replica: " + what + ": " + std::strerror(errno));
            }

            static void check_name(const std::string &name)
            {
                if (name.empty() || name.find('/') != std::string::npos || name == "." || name == "..")
                    throw std::runtime_error("replica: invalid file name " + name);
            }

            std::string full(const std::string &name) const { return directory_ + "/" + name; }

            int open_file(const std::string &name, int flags)
            {
                check_name(name);
                const int fd = ::open(full(name).c_str(), flags | O_CLOEXEC, 0644);
                if (fd < 0)
                    fail("open " + name);
                return fd;
            }

            // Installs fd (or forgets the file when fd is -1), closing any previous descriptor.
            void replace(const std::string &name, int fd)
            {
                const std::lock_guard guard(mutex_);
                const auto it = files_.find(name);
                if (it != files_.end())
                {
                    ::close(it->second);
                    files_.erase(it);
                }
                if (fd >= 0)
                    files_[name] = fd;
            }

            int file(const std::string &name)
            {
                {
                    const std::lock_guard guard(mutex_);
                    const auto it = files_.find(name);
                    if (it != files_.end())
                        return it->second;
                }
                const int fd = open_file(name, O_CREAT | O_WRONLY);
                const std::lock_guard guard(mutex_);
                const auto [it, inserted] = files_.emplace(name, fd);
                if (!inserted)
                    ::close(fd);
                return it->second;
            }

            static void write_at(int fd, std::uint64_t offset, const char *data, std::uint64_t bytes)
            {
                while (bytes > 0)
                {
                    const auto written = ::pwrite(fd, data, bytes, static_cast<off_t>(offset));
                    if (written < 0 && errno == EINTR)
                        continue;
                    if (written <= 0)
                        fail("pwrite");
                    data += written;
                    offset += static_cast<std::uint64_t>(written);
                    bytes -= static_cast<std::uint64_t>(written);
                }
            }

            void open_database(const nlohmann::json &request)
            {
                const std::lock_guard guard(db_mutex_);
                if (db_)
                    throw std::runtime_error("replica database already open");
                const auto shape = request.at("shape").get<DbShape>();
                const auto backend = request.value("backend", std::string("native_cpu"));
                transform_ = backend == "native_cpu" ? nullptr : make_transform(backend);
                auto options = make_db_options(shape, transform_);
                write_options_.disableWAL = request.value("disable_wal", false);
                const auto status = rocksdb::DB::Open(options, directory_, &db_);
                if (!status.ok())
                    throw std::runtime_error("replica open: " + status.ToString());
                backend_ = backend;
            }

            void reset()
            {
                const std::lock_guard guard(db_mutex_);
                db_.reset();
                transform_.reset();
                backend_ = "physical";
                {
                    const std::lock_guard files_guard(mutex_);
                    for (auto &[name, fd] : files_)
                        ::close(fd);
                    files_.clear();
                }
                for (const auto &entry : std::filesystem::directory_iterator(directory_))
                    std::filesystem::remove_all(entry.path());
            }

            void wait_for_compaction()
            {
                const std::lock_guard guard(db_mutex_);
                if (!db_)
                    return;
                rocksdb::WaitForCompactOptions wait;
                wait.flush = true;
                const auto status = db_->WaitForCompact(wait);
                if (!status.ok())
                    throw std::runtime_error("replica wait for compaction: " + status.ToString());
            }

            std::string digest()
            {
                wait_for_compaction();
                const std::lock_guard guard(db_mutex_);
                if (db_)
                    return database_digest(db_.get());
                // Physical replica: open the mirrored files read-only, as a failover would.
                {
                    const std::lock_guard files_guard(mutex_);
                    for (auto &[name, fd] : files_)
                        ::close(fd);
                    files_.clear();
                }
                rocksdb::Options options;
                std::unique_ptr<rocksdb::DB> copy;
                const auto status = rocksdb::DB::OpenForReadOnly(options, directory_, &copy);
                if (!status.ok())
                    throw std::runtime_error("replica read-only open: " + status.ToString());
                return database_digest(copy.get());
            }

            nlohmann::json statistics()
            {
                nlohmann::json result{{"cpu_seconds", cpu_seconds() - started_cpu_},
                                      {"operations", ops_.load()},
                                      {"bytes", bytes_.load()},
                                      {"logical", static_cast<bool>(db_)},
                                      {"backend", backend_}};
                if (transform_)
                    result["transform"] = transform_->statistics();
                return result;
            }

            std::string directory_;
            std::mutex mutex_;
            std::map<std::string, int> files_;
            std::mutex db_mutex_;
            std::unique_ptr<rocksdb::DB> db_;
            std::shared_ptr<BatchTransform> transform_;
            std::string backend_ = "physical";
            rocksdb::WriteOptions write_options_;
            std::atomic<std::uint64_t> ops_{0};
            std::atomic<std::uint64_t> bytes_{0};
            double started_cpu_ = 0;
        };

        std::size_t reply(ReplySink &sink, bool ok, const std::string &message)
        {
            ReplicaReply header;
            header.ok = ok ? 1 : 0;
            header.message_bytes = message.size();
            char *out = sink.reserve(sizeof(header) + message.size());
            std::memcpy(out, &header, sizeof(header));
            std::memcpy(out + sizeof(header), message.data(), message.size());
            return sizeof(header) + message.size();
        }
    }

    // ---- client side

    struct ReplicaSet::Impl
    {
        struct Backup
        {
            Endpoint endpoint;
            std::mutex mutex;
            std::vector<std::unique_ptr<RpcClient>> idle;
        };

        std::vector<std::string> endpoints;
        std::vector<std::unique_ptr<Backup>> backups;
        std::atomic<std::uint64_t> operations{0};
        std::atomic<std::uint64_t> bytes{0};
        std::atomic<std::uint64_t> nanoseconds{0};

        std::unique_ptr<RpcClient> acquire(Backup &backup)
        {
            {
                const std::lock_guard guard(backup.mutex);
                if (!backup.idle.empty())
                {
                    auto client = std::move(backup.idle.back());
                    backup.idle.pop_back();
                    return client;
                }
            }
            return connect_rpc(backup.endpoint);
        }

        void release(Backup &backup, std::unique_ptr<RpcClient> client)
        {
            const std::lock_guard guard(backup.mutex);
            backup.idle.push_back(std::move(client));
        }
    };

    ReplicaSet::ReplicaSet(std::vector<std::string> endpoints) : impl_(std::make_unique<Impl>())
    {
        impl_->endpoints = std::move(endpoints);
        for (const auto &text : impl_->endpoints)
        {
            auto backup = std::make_unique<Impl::Backup>();
            backup->endpoint = parse_endpoint(text);
            backup->idle.push_back(connect_rpc(backup->endpoint));
            impl_->backups.push_back(std::move(backup));
        }
    }

    ReplicaSet::~ReplicaSet() = default;

    const std::vector<std::string> &ReplicaSet::endpoints() const { return impl_->endpoints; }

    rocksdb::Status ReplicaSet::broadcast(ReplicaOp op, const std::string &path, std::uint64_t offset, const char *data,
                                          std::size_t bytes, std::vector<std::string> *replies)
    {
        const auto started = std::chrono::steady_clock::now();
        if (replies != nullptr)
            replies->clear();
        ReplicaHeader header;
        header.op = op;
        header.path_bytes = path.size();
        header.offset = offset;
        header.data_bytes = bytes;
        const auto total = sizeof(header) + path.size() + bytes;
        for (std::size_t index = 0; index < impl_->backups.size(); ++index)
        {
            auto &backup = *impl_->backups[index];
            std::unique_ptr<RpcClient> client;
            try
            {
                client = impl_->acquire(backup);
            }
            catch (const std::exception &error)
            {
                return rocksdb::Status::IOError(std::string("replica connect: ") + error.what());
            }
            char *buffer = client->request_buffer(total);
            std::memcpy(buffer, &header, sizeof(header));
            std::memcpy(buffer + sizeof(header), path.data(), path.size());
            if (bytes != 0)
                std::memcpy(buffer + sizeof(header) + path.size(), data, bytes);
            std::shared_ptr<const char> answer;
            std::size_t answer_bytes = 0;
            const auto status = client->call(total, &answer, &answer_bytes);
            if (!status.ok())
                return status; // the connection is dropped
            ReplicaReply result;
            if (answer_bytes < sizeof(result))
                return rocksdb::Status::Corruption("short replica reply");
            std::memcpy(&result, answer.get(), sizeof(result));
            const std::string message(answer.get() + sizeof(result), result.message_bytes);
            impl_->release(backup, std::move(client));
            if (result.ok == 0)
                return rocksdb::Status::IOError("replica " + impl_->endpoints[index] + ": " + message);
            if (replies != nullptr)
                replies->push_back(message);
        }
        impl_->operations.fetch_add(1, std::memory_order_relaxed);
        impl_->bytes.fetch_add(bytes * impl_->backups.size(), std::memory_order_relaxed);
        impl_->nanoseconds.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                                    std::chrono::steady_clock::now() - started)
                                                                    .count()),
                                     std::memory_order_relaxed);
        return rocksdb::Status::OK();
    }

    nlohmann::json ReplicaSet::statistics() const
    {
        return {{"backups", impl_->endpoints},
                {"operations", impl_->operations.load()},
                {"bytes_sent", impl_->bytes.load()},
                {"seconds", static_cast<double>(impl_->nanoseconds.load()) / 1e9}};
    }

    std::shared_ptr<rocksdb::FileSystem> make_replicating_file_system(std::shared_ptr<rocksdb::FileSystem> base,
                                                                      const std::string &root,
                                                                      std::shared_ptr<ReplicaSet> replicas)
    {
        return std::make_shared<ReplicatingFileSystem>(std::move(base), root, std::move(replicas));
    }

    rocksdb::Status bootstrap_replicas(const std::string &root, ReplicaSet &replicas)
    {
        auto status = replicas.broadcast(ReplicaOp::reset, "", 0, nullptr, 0);
        if (!status.ok())
            return status;
        constexpr std::size_t kChunk = 64ULL << 20;
        std::string buffer(kChunk, '\0');
        for (const auto &entry : std::filesystem::directory_iterator(root))
        {
            const auto name = entry.path().filename().string();
            if (!entry.is_regular_file() || !replicated_name(name))
                continue;
            status = replicas.broadcast(ReplicaOp::create, name, 0, nullptr, 0);
            const int fd = ::open(entry.path().c_str(), O_RDONLY | O_CLOEXEC);
            if (fd < 0)
                return rocksdb::Status::IOError("bootstrap open " + name, std::strerror(errno));
            std::uint64_t offset = 0;
            while (status.ok())
            {
                const auto got = ::pread(fd, buffer.data(), kChunk, static_cast<off_t>(offset));
                if (got < 0 && errno == EINTR)
                    continue;
                if (got < 0)
                {
                    status = rocksdb::Status::IOError("bootstrap read " + name, std::strerror(errno));
                    break;
                }
                if (got == 0)
                    break;
                status = replicas.broadcast(ReplicaOp::append, name, offset, buffer.data(), static_cast<std::size_t>(got));
                offset += static_cast<std::uint64_t>(got);
            }
            ::close(fd);
            if (status.ok())
                status = replicas.broadcast(ReplicaOp::close, name, 0, nullptr, 0);
            if (!status.ok())
                return status;
        }
        return rocksdb::Status::OK();
    }

    std::string database_digest(rocksdb::DB *db)
    {
        rocksdb::ReadOptions read;
        read.verify_checksums = true;
        std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(read));
        std::uint64_t count = 0;
        std::uint64_t hash = 0;
        for (it->SeekToFirst(); it->Valid(); it->Next())
        {
            hash = rocksdb::Hash64(it->key().data(), it->key().size(), hash);
            hash = rocksdb::Hash64(it->value().data(), it->value().size(), hash);
            ++count;
        }
        if (!it->status().ok())
            throw std::runtime_error("digest: " + it->status().ToString());
        std::ostringstream out;
        out << count << ':' << std::hex << hash;
        return out.str();
    }

    RpcHandler make_replica_handler(const std::string &directory)
    {
        auto replica = std::make_shared<Replica>(directory);
        return [replica](const char *data, std::size_t bytes, ReplySink &sink) -> std::size_t {
            try
            {
                ReplicaHeader header;
                if (bytes < sizeof(header))
                    throw std::runtime_error("short replica request");
                std::memcpy(&header, data, sizeof(header));
                if (header.magic != ReplicaHeader{}.magic ||
                    sizeof(header) + header.path_bytes + header.data_bytes > bytes)
                    throw std::runtime_error("malformed replica request");
                const std::string path(data + sizeof(header), header.path_bytes);
                return reply(sink, true, replica->apply(header, path, data + sizeof(header) + header.path_bytes));
            }
            catch (const std::exception &error)
            {
                return reply(sink, false, error.what());
            }
        };
    }
}
