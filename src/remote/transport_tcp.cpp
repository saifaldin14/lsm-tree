#include "transport.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
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
        constexpr std::size_t kAlignment = 4096;

        struct FreeDeleter
        {
            void operator()(char *p) const { std::free(p); }
        };
        using Buffer = std::unique_ptr<char, FreeDeleter>;

        Buffer allocate(std::size_t bytes)
        {
            bytes = (std::max<std::size_t>(bytes, 1) + kAlignment - 1) / kAlignment * kAlignment;
            auto *p = static_cast<char *>(std::aligned_alloc(kAlignment, bytes));
            if (p == nullptr)
                throw std::bad_alloc();
            return Buffer(p);
        }

        // Grows to at least bytes, keeping no contents.
        char *ensure(Buffer &buffer, std::size_t &capacity, std::size_t bytes)
        {
            if (bytes > capacity)
            {
                buffer.reset();
                capacity = std::max(bytes, capacity + capacity / 2);
                buffer = allocate(capacity);
            }
            return buffer.get();
        }

        void write_all(int fd, const char *data, std::size_t bytes)
        {
            while (bytes > 0)
            {
                const auto written = ::send(fd, data, bytes, MSG_NOSIGNAL);
                if (written < 0 && errno == EINTR)
                    continue;
                if (written <= 0)
                    throw std::runtime_error(std::string("tcp send: ") + std::strerror(errno));
                data += written;
                bytes -= static_cast<std::size_t>(written);
            }
        }

        // false on a clean end of stream before any byte.
        bool read_all(int fd, char *data, std::size_t bytes)
        {
            std::size_t done = 0;
            while (done < bytes)
            {
                const auto got = ::recv(fd, data + done, bytes - done, 0);
                if (got < 0 && errno == EINTR)
                    continue;
                if (got == 0 && done == 0)
                    return false;
                if (got <= 0)
                    throw std::runtime_error(std::string("tcp recv: ") + (got == 0 ? "closed" : std::strerror(errno)));
                done += static_cast<std::size_t>(got);
            }
            return true;
        }

        void tune(int fd)
        {
            const int one = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
            ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
            const int buffer = 8 << 20;
            ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buffer, sizeof(buffer));
            ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer));
        }

        class TcpClient final : public RpcClient
        {
        public:
            explicit TcpClient(const Endpoint &endpoint) : endpoint_(endpoint)
            {
                addrinfo hints{};
                hints.ai_family = AF_UNSPEC;
                hints.ai_socktype = SOCK_STREAM;
                addrinfo *result = nullptr;
                const auto port = std::to_string(endpoint.port);
                if (::getaddrinfo(endpoint.host.c_str(), port.c_str(), &hints, &result) != 0 || result == nullptr)
                    throw std::runtime_error("tcp: cannot resolve " + endpoint.text());
                for (auto *ai = result; ai != nullptr && fd_ < 0; ai = ai->ai_next)
                {
                    fd_ = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
                    if (fd_ >= 0 && ::connect(fd_, ai->ai_addr, ai->ai_addrlen) != 0)
                    {
                        ::close(fd_);
                        fd_ = -1;
                    }
                }
                ::freeaddrinfo(result);
                if (fd_ < 0)
                    throw std::runtime_error("tcp: cannot connect to " + endpoint.text());
                tune(fd_);
            }
            ~TcpClient() override
            {
                if (fd_ >= 0)
                    ::close(fd_);
            }

            char *request_buffer(std::size_t bytes) override { return ensure(request_, capacity_, bytes); }

            rocksdb::Status call(std::size_t request_bytes, std::shared_ptr<const char> *reply,
                                 std::size_t *reply_bytes) override
            {
                const auto status = send(request_bytes);
                return status.ok() ? finish(reply, reply_bytes) : status;
            }

            rocksdb::Status send(std::size_t request_bytes) override
            {
                try
                {
                    const std::uint64_t length = request_bytes;
                    write_all(fd_, reinterpret_cast<const char *>(&length), sizeof(length));
                    write_all(fd_, request_.get(), request_bytes);
                    return rocksdb::Status::OK();
                }
                catch (const std::exception &error)
                {
                    return rocksdb::Status::IOError(error.what());
                }
            }

            rocksdb::Status finish(std::shared_ptr<const char> *reply, std::size_t *reply_bytes) override
            {
                try
                {
                    std::uint64_t answer = 0;
                    if (!read_all(fd_, reinterpret_cast<char *>(&answer), sizeof(answer)))
                        throw std::runtime_error("tcp: server closed the connection");
                    auto buffer = allocate(answer);
                    read_all(fd_, buffer.get(), answer);
                    *reply = std::shared_ptr<const char>(buffer.release(), [](const char *p) {
                        std::free(const_cast<char *>(p));
                    });
                    *reply_bytes = answer;
                    return rocksdb::Status::OK();
                }
                catch (const std::exception &error)
                {
                    return rocksdb::Status::IOError(error.what());
                }
            }

            std::string describe() const override { return endpoint_.text(); }

        private:
            Endpoint endpoint_;
            int fd_ = -1;
            Buffer request_;
            std::size_t capacity_ = 0;
        };

        class GrowingSink final : public ReplySink
        {
        public:
            char *reserve(std::size_t bytes) override { return ensure(buffer_, capacity_, bytes); }
            const char *data() const { return buffer_.get(); }

        private:
            Buffer buffer_;
            std::size_t capacity_ = 0;
        };

        class TcpServer final : public RpcServer
        {
        public:
            TcpServer(const Endpoint &endpoint, RpcHandler handler) : handler_(std::move(handler))
            {
                fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
                const int one = 1;
                ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
                sockaddr_in address{};
                address.sin_family = AF_INET;
                address.sin_port = htons(endpoint.port);
                if (::inet_pton(AF_INET, endpoint.host.c_str(), &address.sin_addr) != 1)
                    throw std::invalid_argument("tcp: listen host must be an IPv4 address: " + endpoint.host);
                if (::bind(fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
                    ::listen(fd_, 64) != 0)
                    throw std::runtime_error(std::string("tcp: cannot listen: ") + std::strerror(errno));
                socklen_t length = sizeof(address);
                ::getsockname(fd_, reinterpret_cast<sockaddr *>(&address), &length);
                port_ = ntohs(address.sin_port);
            }
            ~TcpServer() override
            {
                stop();
                for (auto &thread : threads_)
                    if (thread.joinable())
                        thread.join();
                if (fd_ >= 0)
                    ::close(fd_);
            }

            std::uint16_t port() const override { return port_; }

            void run() override
            {
                // Polls with a timeout: closing a listening socket does not wake accept() everywhere.
                const int listening = fd_;
                while (!stopping_)
                {
                    pollfd ready{listening, POLLIN, 0};
                    if (::poll(&ready, 1, 200) <= 0 || stopping_)
                        continue;
                    const int connection = ::accept(listening, nullptr, nullptr);
                    if (connection < 0)
                        continue;
                    tune(connection);
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
                Buffer request;
                std::size_t capacity = 0;
                GrowingSink sink;
                try
                {
                    while (true)
                    {
                        std::uint64_t length = 0;
                        if (!read_all(connection, reinterpret_cast<char *>(&length), sizeof(length)))
                            break;
                        char *data = ensure(request, capacity, length);
                        read_all(connection, data, length);
                        const std::uint64_t answer = handler_(data, length, sink);
                        write_all(connection, reinterpret_cast<const char *>(&answer), sizeof(answer));
                        write_all(connection, sink.data(), answer);
                    }
                }
                catch (const std::exception &)
                {
                    // Connection lost; the client falls back to local compaction.
                }
                ::close(connection);
            }

            RpcHandler handler_;
            int fd_ = -1;
            std::uint16_t port_ = 0;
            std::atomic<bool> stopping_{false};
            std::mutex mutex_;
            std::vector<int> connections_;
            std::vector<std::thread> threads_;
        };
    }

    std::unique_ptr<RpcClient> connect_tcp(const Endpoint &endpoint) { return std::make_unique<TcpClient>(endpoint); }

    std::unique_ptr<RpcServer> listen_tcp(const Endpoint &endpoint, RpcHandler handler)
    {
        return std::make_unique<TcpServer>(endpoint, std::move(handler));
    }
}
