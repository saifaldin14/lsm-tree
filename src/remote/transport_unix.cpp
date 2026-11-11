// Same-machine transport: unix://<socket path>. Requests and replies stay in shared memory
// (memfd on Linux, an unlinked temporary file elsewhere) that the client owns and passes to the
// server once with SCM_RIGHTS; the socket carries only small control messages. So a compaction
// sent to a GPU compaction server on the same machine is not copied through the kernel.
//
//   client -> server  Message {request bytes and region, reply region, region to drop} + new fds
//   server            reads the request in place, runs the handler, writes the reply into the
//                     client's reply region (or, if it is too small, keeps it and asks)
//   server -> client  Answer {reply bytes} or Answer {need bytes}; the client then sends a larger
//                     reply region and the server copies the kept reply into it.
//
// Reply regions come from a per-client pool and return to it when the caller drops the reply,
// so a reply stays valid while a compaction consumes it even if the connection is reused.

#include "transport.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <poll.h>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace lsm_lab::detail
{
    namespace
    {
        constexpr std::size_t kPage = 4096;

        std::size_t round_up(std::size_t bytes) { return (std::max<std::size_t>(bytes, 1) + kPage - 1) / kPage * kPage; }

        [[noreturn]] void fail(const std::string &what)
        {
            throw std::runtime_error("unix: " + what + ": " + std::strerror(errno));
        }

        int shared_memory_fd(std::size_t bytes)
        {
#ifdef __linux__
            const int fd = ::memfd_create("lsm-rpc", MFD_CLOEXEC);
            if (fd < 0)
                fail("memfd_create");
#else
            char path[] = "/tmp/lsm-rpc-XXXXXX";
            const int fd = ::mkstemp(path);
            if (fd < 0)
                fail("mkstemp");
            ::unlink(path);
#endif
            if (::ftruncate(fd, static_cast<off_t>(bytes)) != 0)
            {
                ::close(fd);
                fail("ftruncate");
            }
            return fd;
        }

        // A shared-memory region; the creator owns the fd, a receiver maps the passed copy.
        struct Region
        {
            std::uint64_t id = 0;
            std::size_t bytes = 0;
            int fd = -1;
            char *data = nullptr;

            Region() = default;
            Region(std::uint64_t region_id, std::size_t size, int descriptor) : id(region_id), bytes(size), fd(descriptor)
            {
                void *mapped = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                if (mapped == MAP_FAILED)
                    fail("mmap");
                data = static_cast<char *>(mapped);
            }
            Region(const Region &) = delete;
            Region &operator=(const Region &) = delete;
            ~Region()
            {
                if (data != nullptr)
                    ::munmap(data, bytes);
                if (fd >= 0)
                    ::close(fd);
            }
        };

        struct Message
        {
            std::uint64_t request_bytes = 0;
            std::uint64_t request_region = 0;
            std::uint64_t request_region_bytes = 0;
            std::uint64_t reply_region = 0;
            std::uint64_t reply_region_bytes = 0;
            std::uint64_t drop_region = 0;
            // Bit 0: request region fd attached; bit 1: reply region fd attached; bit 2: this
            // message only supplies a larger reply region for a kept reply.
            std::uint32_t flags = 0;
            std::uint32_t padding = 0;
        };
        struct Answer
        {
            std::uint64_t reply_bytes = 0;
            std::uint64_t need_bytes = 0;
        };

        void send_with_fds(int socket, const void *data, std::size_t bytes, const std::vector<int> &fds)
        {
            iovec vector{const_cast<void *>(data), bytes};
            msghdr header{};
            header.msg_iov = &vector;
            header.msg_iovlen = 1;
            std::vector<char> control(CMSG_SPACE(sizeof(int) * std::max<std::size_t>(fds.size(), 1)));
            if (!fds.empty())
            {
                header.msg_control = control.data();
                header.msg_controllen = CMSG_SPACE(sizeof(int) * fds.size());
                auto *message = CMSG_FIRSTHDR(&header);
                message->cmsg_level = SOL_SOCKET;
                message->cmsg_type = SCM_RIGHTS;
                message->cmsg_len = CMSG_LEN(sizeof(int) * fds.size());
                std::memcpy(CMSG_DATA(message), fds.data(), sizeof(int) * fds.size());
            }
            auto *cursor = static_cast<const char *>(data);
            std::size_t left = bytes;
            bool first = true;
            while (left > 0)
            {
                const auto sent = first ? ::sendmsg(socket, &header, MSG_NOSIGNAL) : ::send(socket, cursor, left, MSG_NOSIGNAL);
                if (sent < 0 && errno == EINTR)
                    continue;
                if (sent <= 0)
                    fail("send");
                first = false;
                cursor += sent;
                left -= static_cast<std::size_t>(sent);
            }
        }

        // false on a clean end of stream before any byte; received fds are appended to fds.
        bool receive_with_fds(int socket, void *data, std::size_t bytes, std::vector<int> *fds)
        {
            auto *cursor = static_cast<char *>(data);
            std::size_t done = 0;
            while (done < bytes)
            {
                iovec vector{cursor + done, bytes - done};
                msghdr header{};
                header.msg_iov = &vector;
                header.msg_iovlen = 1;
                char control[CMSG_SPACE(sizeof(int) * 4)];
                header.msg_control = control;
                header.msg_controllen = sizeof(control);
                const auto got = ::recvmsg(socket, &header, 0);
                if (got < 0 && errno == EINTR)
                    continue;
                if (got == 0 && done == 0)
                    return false;
                if (got <= 0)
                {
                    if (got == 0)
                        errno = ECONNRESET;
                    fail("receive");
                }
                for (auto *message = CMSG_FIRSTHDR(&header); message != nullptr; message = CMSG_NXTHDR(&header, message))
                    if (message->cmsg_level == SOL_SOCKET && message->cmsg_type == SCM_RIGHTS)
                    {
                        const auto count = (message->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                        const auto *received = reinterpret_cast<const int *>(CMSG_DATA(message));
                        fds->insert(fds->end(), received, received + count);
                    }
                done += static_cast<std::size_t>(got);
            }
            return true;
        }

        sockaddr_un socket_address(const std::string &path)
        {
            sockaddr_un address{};
            address.sun_family = AF_UNIX;
            if (path.size() >= sizeof(address.sun_path))
                throw std::invalid_argument("unix: socket path too long: " + path);
            std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
            return address;
        }

        class UnixClient final : public RpcClient
        {
        public:
            explicit UnixClient(const Endpoint &endpoint) : endpoint_(endpoint), pool_(std::make_shared<ReplyPool>())
            {
                socket_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
                if (socket_ < 0)
                    fail("socket");
                const auto address = socket_address(endpoint.host);
                if (::connect(socket_, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0)
                {
                    ::close(socket_);
                    fail("connect " + endpoint.host);
                }
            }
            ~UnixClient() override { ::close(socket_); }

            char *request_buffer(std::size_t bytes) override
            {
                if (!request_ || request_->bytes < bytes)
                {
                    if (request_)
                        retired_ = request_->id;
                    const auto size = round_up(std::max(bytes, request_ ? request_->bytes + request_->bytes / 2 : 0));
                    request_ = std::make_unique<Region>(next_id_++, size, shared_memory_fd(size));
                    request_sent_ = false;
                }
                return request_->data;
            }

            rocksdb::Status call(std::size_t request_bytes, std::shared_ptr<const char> *reply,
                                 std::size_t *reply_bytes) override
            {
                try
                {
                    auto region = pool_->take(next_id_, reply_hint_);
                    Message message;
                    message.request_bytes = request_bytes;
                    message.request_region = request_->id;
                    message.request_region_bytes = request_->bytes;
                    message.reply_region = region->id;
                    message.reply_region_bytes = region->bytes;
                    message.drop_region = retired_;
                    std::vector<int> fds;
                    if (!request_sent_)
                    {
                        message.flags |= 1;
                        fds.push_back(request_->fd);
                    }
                    if (known_.count(region->id) == 0)
                    {
                        message.flags |= 2;
                        fds.push_back(region->fd);
                    }
                    send_with_fds(socket_, &message, sizeof(message), fds);
                    request_sent_ = true;
                    retired_ = 0;
                    known_.insert({region->id, true});
                    Answer answer;
                    std::vector<int> unused;
                    if (!receive_with_fds(socket_, &answer, sizeof(answer), &unused))
                        throw std::runtime_error("unix: server closed the connection");
                    if (answer.need_bytes > 0)
                    {
                        // The reply did not fit: give the server a larger region for it.
                        pool_->put(std::move(region));
                        reply_hint_ = round_up(answer.need_bytes + answer.need_bytes / 4);
                        region = pool_->take(next_id_, reply_hint_);
                        Message supply;
                        supply.flags = 4;
                        supply.reply_region = region->id;
                        supply.reply_region_bytes = region->bytes;
                        std::vector<int> supplied;
                        if (known_.count(region->id) == 0)
                        {
                            supply.flags |= 2;
                            supplied.push_back(region->fd);
                        }
                        send_with_fds(socket_, &supply, sizeof(supply), supplied);
                        known_.insert({region->id, true});
                        if (!receive_with_fds(socket_, &answer, sizeof(answer), &unused))
                            throw std::runtime_error("unix: server closed the connection");
                    }
                    *reply_bytes = answer.reply_bytes;
                    auto *data = region->data;
                    auto shared = std::shared_ptr<Region>(region.release(), [pool = pool_](Region *returned) {
                        pool->put(std::unique_ptr<Region>(returned));
                    });
                    *reply = std::shared_ptr<const char>(shared, data);
                    return rocksdb::Status::OK();
                }
                catch (const std::exception &error)
                {
                    return rocksdb::Status::IOError(error.what());
                }
            }

            std::string describe() const override { return endpoint_.text(); }

        private:
            // Reply regions not in use; a region in use belongs to the reply that holds it.
            class ReplyPool
            {
            public:
                std::unique_ptr<Region> take(std::uint64_t &next_id, std::size_t at_least)
                {
                    {
                        const std::lock_guard lock(mutex_);
                        auto best = free_.end();
                        for (auto it = free_.begin(); it != free_.end(); ++it)
                            if ((*it)->bytes >= at_least && (best == free_.end() || (*it)->bytes < (*best)->bytes))
                                best = it;
                        if (best != free_.end())
                        {
                            auto region = std::move(*best);
                            free_.erase(best);
                            return region;
                        }
                    }
                    const auto size = round_up(std::max<std::size_t>(at_least, 1 << 20));
                    return std::make_unique<Region>(next_id++, size, shared_memory_fd(size));
                }
                void put(std::unique_ptr<Region> region)
                {
                    const std::lock_guard lock(mutex_);
                    free_.push_back(std::move(region));
                }

            private:
                std::mutex mutex_;
                std::vector<std::unique_ptr<Region>> free_;
            };

            Endpoint endpoint_;
            int socket_ = -1;
            std::shared_ptr<ReplyPool> pool_;
            std::unique_ptr<Region> request_;
            bool request_sent_ = false;
            std::uint64_t retired_ = 0;
            std::uint64_t next_id_ = 1;
            std::size_t reply_hint_ = 1 << 20;
            std::map<std::uint64_t, bool> known_;
        };

        // Writes the reply straight into the client's reply region when it fits, otherwise into
        // a local buffer that is copied once the client supplies a larger region.
        class RegionSink final : public ReplySink
        {
        public:
            RegionSink(char *region, std::size_t bytes) : region_(region), region_bytes_(bytes) {}
            char *reserve(std::size_t bytes) override
            {
                if (bytes <= region_bytes_)
                    return region_;
                overflow_.resize(bytes);
                return overflow_.data();
            }
            bool overflowed() const { return !overflow_.empty(); }
            const char *overflow() const { return overflow_.data(); }

        private:
            char *region_;
            std::size_t region_bytes_;
            std::vector<char> overflow_;
        };

        class UnixServer final : public RpcServer
        {
        public:
            UnixServer(const Endpoint &endpoint, RpcHandler handler) : path_(endpoint.host), handler_(std::move(handler))
            {
                fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
                if (fd_ < 0)
                    fail("socket");
                ::unlink(path_.c_str());
                const auto address = socket_address(path_);
                if (::bind(fd_, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0 || ::listen(fd_, 64) != 0)
                    fail("listen " + path_);
            }
            ~UnixServer() override
            {
                stop();
                for (auto &thread : threads_)
                    if (thread.joinable())
                        thread.join();
                ::close(fd_);
                ::unlink(path_.c_str());
            }

            std::uint16_t port() const override { return 0; }

            void run() override
            {
                while (!stopping_)
                {
                    pollfd ready{fd_, POLLIN, 0};
                    if (::poll(&ready, 1, 200) <= 0 || stopping_)
                        continue;
                    const int connection = ::accept(fd_, nullptr, nullptr);
                    if (connection < 0)
                        continue;
                    const std::lock_guard guard(mutex_);
                    connections_.push_back(connection);
                    threads_.emplace_back([this, connection] { serve(connection); });
                }
            }

            void stop() override
            {
                stopping_ = true;
                const std::lock_guard guard(mutex_);
                for (const int connection : connections_)
                    ::shutdown(connection, SHUT_RDWR);
            }

        private:
            void serve(int connection)
            {
                std::map<std::uint64_t, std::unique_ptr<Region>> regions;
                const auto adopt = [&](std::uint64_t id, std::uint64_t bytes, std::vector<int> &fds, std::size_t &next) {
                    if (next >= fds.size())
                        throw std::runtime_error("unix: missing region descriptor");
                    regions[id] = std::make_unique<Region>(id, bytes, fds[next++]);
                };
                const auto region = [&](std::uint64_t id) -> Region & {
                    const auto it = regions.find(id);
                    if (it == regions.end())
                        throw std::runtime_error("unix: unknown region");
                    return *it->second;
                };
                try
                {
                    while (true)
                    {
                        Message message;
                        std::vector<int> fds;
                        if (!receive_with_fds(connection, &message, sizeof(message), &fds))
                            break;
                        std::size_t next = 0;
                        if (message.flags & 1)
                            adopt(message.request_region, message.request_region_bytes, fds, next);
                        if (message.flags & 2)
                            adopt(message.reply_region, message.reply_region_bytes, fds, next);
                        for (; next < fds.size(); ++next)
                            ::close(fds[next]);
                        if (message.drop_region != 0)
                            regions.erase(message.drop_region);
                        auto &request = region(message.request_region);
                        auto &reply = region(message.reply_region);
                        if (message.request_bytes > request.bytes)
                            throw std::runtime_error("unix: request larger than its region");
                        RegionSink sink(reply.data, reply.bytes);
                        Answer answer;
                        answer.reply_bytes = handler_(request.data, message.request_bytes, sink);
                        if (!sink.overflowed())
                        {
                            send_with_fds(connection, &answer, sizeof(answer), {});
                            continue;
                        }
                        Answer need;
                        need.need_bytes = answer.reply_bytes;
                        send_with_fds(connection, &need, sizeof(need), {});
                        Message supply;
                        std::vector<int> supplied;
                        if (!receive_with_fds(connection, &supply, sizeof(supply), &supplied))
                            break;
                        next = 0;
                        if (supply.flags & 2)
                            adopt(supply.reply_region, supply.reply_region_bytes, supplied, next);
                        auto &larger = region(supply.reply_region);
                        if (answer.reply_bytes > larger.bytes)
                            throw std::runtime_error("unix: supplied reply region too small");
                        std::memcpy(larger.data, sink.overflow(), answer.reply_bytes);
                        send_with_fds(connection, &answer, sizeof(answer), {});
                    }
                }
                catch (const std::exception &)
                {
                    // Connection lost; the client falls back to local compaction.
                }
                ::close(connection);
            }

            std::string path_;
            RpcHandler handler_;
            int fd_ = -1;
            std::atomic<bool> stopping_{false};
            std::mutex mutex_;
            std::vector<int> connections_;
            std::vector<std::thread> threads_;
        };
    }

    std::unique_ptr<RpcClient> connect_unix(const Endpoint &endpoint) { return std::make_unique<UnixClient>(endpoint); }

    std::unique_ptr<RpcServer> listen_unix(const Endpoint &endpoint, RpcHandler handler)
    {
        return std::make_unique<UnixServer>(endpoint, std::move(handler));
    }
}
