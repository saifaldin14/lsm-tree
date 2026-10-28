// RDMA transport: RC queue pairs set up with librdmacm.
//
// Protocol for one call (request and reply buffers on both sides are registered once
// and reused). The request moves by push (default) or pull (LSM_RDMA_PUSH=0):
//   pull: client -> server  SEND  kRequest {request addr/rkey/bytes, reply addr/rkey/capacity}
//         server            RDMA READ the request from client memory
//   push: client -> server  SEND  kRequestPush {request bytes, reply addr/rkey/capacity}
//         server -> client  SEND  kRequestBuffer {server buffer addr/rkey}
//         client            RDMA WRITE the request into server memory
//         client -> server  SEND  kRequestReady
//   eager: requests and replies of at most kInline bytes travel inside the control
//          messages themselves (kRequestInline, kDoneInline), one round trip in total
// then, for push and pull:
//   server -> client  SEND  kNeed {bytes}        only if the reply exceeds the capacity
//   client -> server  SEND  kReplyBuffer {...}   with a larger reply buffer
//   server            RDMA WRITE the reply into client memory
//   server -> client  SEND  kDone {bytes}        (RC ordering: the WRITE is visible first)
// Waits block on a completion channel (LSM_RDMA_POLL=1 busy-polls instead). A client also
// watches its connection events and gives up after LSM_REMOTE_TIMEOUT_MS (default 120 s)
// without progress, so a dead server makes the job fall back instead of hanging.

#include "transport.hpp"

#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <deque>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <netdb.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace lsm_lab::detail
{
    namespace
    {
        constexpr std::size_t kPage = 4096;
        constexpr std::uint64_t kChunk = 256ULL << 20; // below the 1 GiB max message size
        constexpr int kRecvSlots = 16;
        constexpr std::uint64_t kSendId = ~0ULL;
        constexpr std::uint64_t kRdmaIdBase = 1ULL << 32;

        enum : std::uint32_t
        {
            kRequest = 1,
            kNeed = 2,
            kReplyBuffer = 3,
            kDone = 4,
            kRequestPush = 5,
            kRequestBuffer = 6,
            kRequestReady = 7,
            kRequestInline = 8,
            kDoneInline = 9,
        };

        struct Ctrl
        {
            std::uint32_t type = 0;
            std::uint32_t request_rkey = 0;
            std::uint64_t request_addr = 0;
            std::uint64_t request_bytes = 0;
            std::uint64_t reply_addr = 0;
            std::uint64_t reply_capacity = 0;
            std::uint32_t reply_rkey = 0;
            std::uint32_t inline_bytes = 0; // payload bytes following this header in the same message
            std::uint64_t reply_bytes = 0;
        };

        // Control messages carry up to kInline payload bytes after the header.
        constexpr std::size_t kSlot = 16384;
        constexpr std::size_t kInline = kSlot - sizeof(Ctrl);

        struct Message
        {
            Ctrl ctrl;
            std::string payload;
        };

        [[noreturn]] void fail(const std::string &what)
        {
            throw std::runtime_error("rdma: " + what + (errno != 0 ? std::string(": ") + std::strerror(errno) : ""));
        }

        bool busy_poll()
        {
            static const bool value = std::getenv("LSM_RDMA_POLL") != nullptr;
            return value;
        }

        bool push_requests()
        {
            static const bool value = [] {
                // Push (RDMA WRITE) reaches line rate on ConnectX-6; READ peaks near half of it.
                const char *configured = std::getenv("LSM_RDMA_PUSH");
                return configured == nullptr || std::string(configured) != "0";
            }();
            return value;
        }

        int client_timeout_ms()
        {
            static const int value = [] {
                const char *configured = std::getenv("LSM_REMOTE_TIMEOUT_MS");
                const auto parsed = configured == nullptr ? 0 : std::atoi(configured);
                return parsed > 0 ? parsed : 120000;
            }();
            return value;
        }

        void set_nonblocking(int fd)
        {
            const int flags = ::fcntl(fd, F_GETFL);
            if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0)
                fail("fcntl");
        }

        struct Pd
        {
            explicit Pd(ibv_context *context) : pd(ibv_alloc_pd(context))
            {
                if (pd == nullptr)
                    fail("ibv_alloc_pd");
            }
            ~Pd() { ibv_dealloc_pd(pd); }
            ibv_pd *pd;
        };

        struct Region
        {
            char *data = nullptr;
            std::size_t capacity = 0;
            ibv_mr *mr = nullptr;
        };

        // Registered buffers, reused across calls (registration is expensive).
        class Pool : public std::enable_shared_from_this<Pool>
        {
        public:
            explicit Pool(std::shared_ptr<Pd> pd) : pd_(std::move(pd)) {}
            ~Pool()
            {
                for (auto &region : free_)
                    destroy(region);
            }

            Region get(std::size_t bytes)
            {
                {
                    const std::lock_guard guard(mutex_);
                    auto best = free_.end();
                    for (auto it = free_.begin(); it != free_.end(); ++it)
                        if (it->capacity >= bytes && (best == free_.end() || it->capacity < best->capacity))
                            best = it;
                    if (best != free_.end())
                    {
                        auto region = *best;
                        free_.erase(best);
                        return region;
                    }
                }
                std::size_t capacity = 1ULL << 20;
                while (capacity < bytes)
                    capacity <<= 1;
                Region region;
                region.capacity = capacity;
                region.data = static_cast<char *>(std::aligned_alloc(kPage, capacity));
                if (region.data == nullptr)
                    throw std::bad_alloc();
                region.mr = ibv_reg_mr(pd_->pd, region.data, capacity,
                                       IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
                if (region.mr == nullptr)
                {
                    std::free(region.data);
                    fail("ibv_reg_mr");
                }
                return region;
            }

            void put(Region region)
            {
                const std::lock_guard guard(mutex_);
                free_.push_back(region);
            }

        private:
            static void destroy(Region &region)
            {
                ibv_dereg_mr(region.mr);
                std::free(region.data);
            }

            std::shared_ptr<Pd> pd_;
            std::mutex mutex_;
            std::vector<Region> free_;
        };

        // One connected RC queue pair with its completion queue and control messages.
        class Channel
        {
        public:
            Channel(rdma_cm_id *id, std::shared_ptr<Pd> pd) : id_(id), pd_(std::move(pd))
            {
                completion_ = ibv_create_comp_channel(id->verbs);
                if (completion_ == nullptr)
                    fail("ibv_create_comp_channel");
                set_nonblocking(completion_->fd);
                cq_ = ibv_create_cq(id->verbs, 256, nullptr, completion_, 0);
                if (cq_ == nullptr)
                    fail("ibv_create_cq");
                ibv_qp_init_attr attr{};
                attr.send_cq = cq_;
                attr.recv_cq = cq_;
                attr.qp_type = IBV_QPT_RC;
                attr.cap.max_send_wr = 64;
                attr.cap.max_recv_wr = kRecvSlots;
                attr.cap.max_send_sge = 1;
                attr.cap.max_recv_sge = 1;
                if (rdma_create_qp(id, pd_->pd, &attr) != 0)
                    fail("rdma_create_qp");
                ctrl_.resize((kRecvSlots + 1) * kSlot);
                ctrl_mr_ = ibv_reg_mr(pd_->pd, ctrl_.data(), ctrl_.size(), IBV_ACCESS_LOCAL_WRITE);
                if (ctrl_mr_ == nullptr)
                    fail("ibv_reg_mr (control)");
                for (int slot = 0; slot < kRecvSlots; ++slot)
                    post_recv(slot);
                ibv_device_attr device{};
                ibv_query_device(id->verbs, &device);
                read_depth_ = std::max(1, std::min({16, device.max_qp_init_rd_atom, device.max_qp_rd_atom}));
            }

            ~Channel()
            {
                if (id_->qp != nullptr)
                    rdma_destroy_qp(id_);
                if (ctrl_mr_ != nullptr)
                    ibv_dereg_mr(ctrl_mr_);
                if (cq_ != nullptr)
                    ibv_destroy_cq(cq_);
                if (completion_ != nullptr)
                    ibv_destroy_comp_channel(completion_);
            }

            rdma_conn_param connection_parameters() const
            {
                rdma_conn_param parameters{};
                parameters.initiator_depth = static_cast<std::uint8_t>(read_depth_);
                parameters.responder_resources = static_cast<std::uint8_t>(read_depth_);
                parameters.retry_count = 7;
                parameters.rnr_retry_count = 7;
                return parameters;
            }

            // Client, once connected: waits also end when the connection drops or nothing happens
            // for timeout_ms. The server waits indefinitely.
            void watch(rdma_event_channel *cm_events, int timeout_ms)
            {
                cm_events_ = cm_events;
                timeout_ms_ = timeout_ms;
                set_nonblocking(cm_events_->fd);
            }

            const std::shared_ptr<Pd> &pd() const { return pd_; }
            rdma_cm_id *id() const { return id_; }

            void send(Ctrl message, const char *payload = nullptr, std::size_t bytes = 0)
            {
                if (bytes > kInline)
                    throw std::logic_error("inline payload too large");
                message.inline_bytes = static_cast<std::uint32_t>(bytes);
                char *slot = ctrl_.data() + kRecvSlots * kSlot;
                std::memcpy(slot, &message, sizeof(Ctrl));
                if (bytes != 0)
                    std::memcpy(slot + sizeof(Ctrl), payload, bytes);
                ibv_sge sge{reinterpret_cast<std::uint64_t>(slot), static_cast<std::uint32_t>(sizeof(Ctrl) + bytes),
                            ctrl_mr_->lkey};
                ibv_send_wr wr{};
                wr.wr_id = kSendId;
                wr.sg_list = &sge;
                wr.num_sge = 1;
                wr.opcode = IBV_WR_SEND;
                wr.send_flags = IBV_SEND_SIGNALED;
                ibv_send_wr *bad = nullptr;
                if (ibv_post_send(id_->qp, &wr, &bad) != 0)
                    fail("post send");
                wait_sends(1, kSendId);
            }

            Message receive()
            {
                while (received_.empty())
                    poll_once();
                auto message = std::move(received_.front());
                received_.pop_front();
                return message;
            }

            // One-sided READ (remote -> local) or WRITE (local -> remote) of bytes, in chunks.
            void transfer(ibv_wr_opcode opcode, const Region &local, std::uint64_t remote, std::uint32_t rkey,
                          std::uint64_t bytes)
            {
                const int window = opcode == IBV_WR_RDMA_READ ? read_depth_ : 32;
                std::uint64_t done = 0;
                while (done < bytes)
                {
                    int posted = 0;
                    for (; posted < window && done < bytes; ++posted)
                    {
                        const auto length = std::min(kChunk, bytes - done);
                        ibv_sge sge{reinterpret_cast<std::uint64_t>(local.data + done), static_cast<std::uint32_t>(length),
                                    local.mr->lkey};
                        ibv_send_wr wr{};
                        wr.wr_id = kRdmaIdBase + static_cast<std::uint64_t>(posted);
                        wr.sg_list = &sge;
                        wr.num_sge = 1;
                        wr.opcode = opcode;
                        wr.send_flags = IBV_SEND_SIGNALED;
                        wr.wr.rdma.remote_addr = remote + done;
                        wr.wr.rdma.rkey = rkey;
                        ibv_send_wr *bad = nullptr;
                        if (ibv_post_send(id_->qp, &wr, &bad) != 0)
                            fail("post rdma");
                        done += length;
                    }
                    wait_sends(posted, 0);
                }
            }

        private:
            void post_recv(int slot)
            {
                ibv_sge sge{reinterpret_cast<std::uint64_t>(ctrl_.data() + slot * kSlot), static_cast<std::uint32_t>(kSlot),
                            ctrl_mr_->lkey};
                ibv_recv_wr wr{};
                wr.wr_id = static_cast<std::uint64_t>(slot);
                wr.sg_list = &sge;
                wr.num_sge = 1;
                ibv_recv_wr *bad = nullptr;
                if (ibv_post_recv(id_->qp, &wr, &bad) != 0)
                    fail("post recv");
            }

            // Waits for count send-side completions (any id when id is 0, else that id).
            void wait_sends(int count, std::uint64_t id)
            {
                while (completed_sends_ < count || (id != 0 && !send_done_))
                    poll_once();
                completed_sends_ -= count;
                send_done_ = false;
                (void)id;
            }

            // Fails if the connection dropped (client only).
            void check_connection()
            {
                if (cm_events_ == nullptr)
                    return;
                rdma_cm_event *event = nullptr;
                while (rdma_get_cm_event(cm_events_, &event) == 0)
                {
                    const auto type = event->event;
                    rdma_ack_cm_event(event);
                    if (type == RDMA_CM_EVENT_DISCONNECTED || type == RDMA_CM_EVENT_DEVICE_REMOVAL ||
                        type == RDMA_CM_EVENT_TIMEWAIT_EXIT)
                    {
                        errno = 0;
                        fail("server disconnected");
                    }
                }
            }

            // Blocks until the completion channel signals, the connection drops or the timeout passes.
            void wait_event(std::chrono::steady_clock::time_point deadline)
            {
                while (true)
                {
                    pollfd fds[2] = {{completion_->fd, POLLIN, 0}, {cm_events_ ? cm_events_->fd : -1, POLLIN, 0}};
                    int wait = -1;
                    if (timeout_ms_ > 0)
                    {
                        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                                              deadline - std::chrono::steady_clock::now())
                                              .count();
                        if (left <= 0)
                        {
                            errno = 0;
                            fail("timed out waiting for the server");
                        }
                        wait = static_cast<int>(std::min<long long>(left, 1000));
                    }
                    const int ready = ::poll(fds, cm_events_ ? 2 : 1, wait);
                    if (ready < 0 && errno != EINTR)
                        fail("poll");
                    if (ready > 0 && (fds[1].revents & POLLIN) != 0)
                        check_connection();
                    if (ready > 0 && (fds[0].revents & POLLIN) != 0)
                    {
                        ibv_cq *cq = nullptr;
                        void *context = nullptr;
                        if (ibv_get_cq_event(completion_, &cq, &context) == 0)
                        {
                            ibv_ack_cq_events(cq, 1);
                            return;
                        }
                    }
                }
            }

            void poll_once()
            {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(timeout_ms_, 0));
                ibv_wc wc[16];
                int n = 0;
                while ((n = ibv_poll_cq(cq_, 16, wc)) == 0)
                {
                    if (busy_poll())
                    {
                        if (timeout_ms_ > 0 && std::chrono::steady_clock::now() > deadline)
                        {
                            errno = 0;
                            fail("timed out waiting for the server");
                        }
                        check_connection();
                        continue;
                    }
                    if (ibv_req_notify_cq(cq_, 0) != 0)
                        fail("req notify");
                    if ((n = ibv_poll_cq(cq_, 16, wc)) != 0)
                        break;
                    wait_event(deadline);
                }
                if (n < 0)
                    fail("poll cq");
                for (int index = 0; index < n; ++index)
                {
                    if (wc[index].status != IBV_WC_SUCCESS)
                    {
                        errno = 0;
                        fail(std::string("work completion: ") + ibv_wc_status_str(wc[index].status));
                    }
                    if (wc[index].opcode == IBV_WC_RECV)
                    {
                        const auto slot = static_cast<int>(wc[index].wr_id);
                        const char *data = ctrl_.data() + slot * kSlot;
                        Message message;
                        std::memcpy(&message.ctrl, data, sizeof(Ctrl));
                        message.payload.assign(data + sizeof(Ctrl), message.ctrl.inline_bytes);
                        received_.push_back(std::move(message));
                        post_recv(slot);
                    }
                    else
                    {
                        ++completed_sends_;
                        if (wc[index].wr_id == kSendId)
                            send_done_ = true;
                    }
                }
            }

            rdma_cm_id *id_;
            std::shared_ptr<Pd> pd_;
            rdma_event_channel *cm_events_ = nullptr;
            int timeout_ms_ = -1;
            ibv_comp_channel *completion_ = nullptr;
            ibv_cq *cq_ = nullptr;
            ibv_mr *ctrl_mr_ = nullptr;
            std::vector<char> ctrl_; // kRecvSlots receive slots, then one send slot
            std::deque<Message> received_;
            int completed_sends_ = 0;
            bool send_done_ = false;
            int read_depth_ = 1;
        };

        rdma_cm_event_type next_event(rdma_event_channel *events, rdma_cm_id **id = nullptr)
        {
            rdma_cm_event *event = nullptr;
            if (rdma_get_cm_event(events, &event) != 0)
                fail("rdma_get_cm_event");
            const auto type = event->event;
            if (id != nullptr)
                *id = event->id;
            rdma_ack_cm_event(event);
            return type;
        }

        void expect(rdma_event_channel *events, rdma_cm_event_type wanted, const std::string &what)
        {
            const auto got = next_event(events);
            if (got != wanted)
            {
                errno = 0;
                fail(what + " (" + rdma_event_str(got) + ")");
            }
        }

        addrinfo *resolve(const Endpoint &endpoint, bool passive)
        {
            addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_flags = passive ? AI_PASSIVE : 0;
            addrinfo *result = nullptr;
            const auto port = std::to_string(endpoint.port);
            if (::getaddrinfo(endpoint.host.c_str(), port.c_str(), &hints, &result) != 0 || result == nullptr)
            {
                errno = 0;
                fail("cannot resolve " + endpoint.text());
            }
            return result;
        }

        class RdmaClient final : public RpcClient
        {
        public:
            explicit RdmaClient(const Endpoint &endpoint) : endpoint_(endpoint)
            {
                events_ = rdma_create_event_channel();
                if (events_ == nullptr || rdma_create_id(events_, &id_, nullptr, RDMA_PS_TCP) != 0)
                    fail("rdma_create_id");
                auto *address = resolve(endpoint, false);
                const int resolved = rdma_resolve_addr(id_, nullptr, address->ai_addr, 5000);
                ::freeaddrinfo(address);
                if (resolved != 0)
                    fail("rdma_resolve_addr");
                expect(events_, RDMA_CM_EVENT_ADDR_RESOLVED, "address resolution");
                if (rdma_resolve_route(id_, 5000) != 0)
                    fail("rdma_resolve_route");
                expect(events_, RDMA_CM_EVENT_ROUTE_RESOLVED, "route resolution");
                auto pd = std::make_shared<Pd>(id_->verbs);
                channel_ = std::make_unique<Channel>(id_, pd);
                requests_ = std::make_shared<Pool>(pd);
                replies_ = std::make_shared<Pool>(pd);
                auto parameters = channel_->connection_parameters();
                if (rdma_connect(id_, &parameters) != 0)
                    fail("rdma_connect");
                expect(events_, RDMA_CM_EVENT_ESTABLISHED, "connection to " + endpoint.text());
                channel_->watch(events_, client_timeout_ms());
                device_ = ibv_get_device_name(id_->verbs->device);
            }

            ~RdmaClient() override
            {
                if (request_.data != nullptr)
                    requests_->put(request_);
                rdma_disconnect(id_);
                channel_.reset();
                rdma_destroy_id(id_);
                rdma_destroy_event_channel(events_);
            }

            char *request_buffer(std::size_t bytes) override
            {
                if (request_.capacity < bytes)
                {
                    if (request_.data != nullptr)
                        requests_->put(request_);
                    request_ = requests_->get(bytes);
                }
                return request_.data;
            }

            rocksdb::Status call(std::size_t request_bytes, std::shared_ptr<const char> *reply,
                                 std::size_t *reply_bytes) override
            {
                try
                {
                    // Replies are usually no larger than the request; kNeed covers the rest.
                    auto region = replies_->get(std::max<std::size_t>(request_bytes, last_reply_));
                    Ctrl message;
                    message.reply_addr = reinterpret_cast<std::uint64_t>(region.data);
                    message.reply_rkey = region.mr->rkey;
                    message.reply_capacity = region.capacity;
                    if (request_bytes <= kInline)
                    {
                        // Eager: one round trip for small requests (WAL appends, write batches).
                        message.type = kRequestInline;
                        channel_->send(message, request_.data, request_bytes);
                    }
                    else
                    {
                        message.type = push_requests() ? kRequestPush : kRequest;
                        message.request_addr = reinterpret_cast<std::uint64_t>(request_.data);
                        message.request_rkey = request_.mr->rkey;
                        message.request_bytes = request_bytes;
                        channel_->send(message);
                        if (push_requests())
                        {
                            const auto target = channel_->receive().ctrl;
                            if (target.type != kRequestBuffer)
                            {
                                errno = 0;
                                fail("expected a request buffer");
                            }
                            channel_->transfer(IBV_WR_RDMA_WRITE, request_, target.request_addr, target.request_rkey,
                                               request_bytes);
                            Ctrl ready;
                            ready.type = kRequestReady;
                            channel_->send(ready);
                        }
                    }
                    while (true)
                    {
                        const auto received = channel_->receive();
                        const auto &answer = received.ctrl;
                        if (answer.type == kDoneInline)
                        {
                            std::memcpy(region.data, received.payload.data(), received.payload.size());
                            last_reply_ = received.payload.size();
                            *reply = std::shared_ptr<const char>(region.data, [pool = replies_, region](const char *) {
                                pool->put(region);
                            });
                            *reply_bytes = received.payload.size();
                            return rocksdb::Status::OK();
                        }
                        if (answer.type == kNeed)
                        {
                            replies_->put(region);
                            region = replies_->get(answer.reply_bytes);
                            Ctrl buffer;
                            buffer.type = kReplyBuffer;
                            buffer.reply_addr = reinterpret_cast<std::uint64_t>(region.data);
                            buffer.reply_rkey = region.mr->rkey;
                            buffer.reply_capacity = region.capacity;
                            channel_->send(buffer);
                            continue;
                        }
                        if (answer.type != kDone)
                        {
                            errno = 0;
                            fail("unexpected control message");
                        }
                        last_reply_ = answer.reply_bytes;
                        // The reply keeps its pool (and protection domain) alive until it is released.
                        *reply = std::shared_ptr<const char>(region.data, [pool = replies_, region](const char *) {
                            pool->put(region);
                        });
                        *reply_bytes = answer.reply_bytes;
                        return rocksdb::Status::OK();
                    }
                }
                catch (const std::exception &error)
                {
                    return rocksdb::Status::IOError(error.what());
                }
            }

            std::string describe() const override { return endpoint_.text() + " (" + device_ + ")"; }

        private:
            Endpoint endpoint_;
            std::string device_;
            rdma_event_channel *events_ = nullptr;
            rdma_cm_id *id_ = nullptr;
            std::unique_ptr<Channel> channel_;
            std::shared_ptr<Pool> requests_;
            std::shared_ptr<Pool> replies_;
            Region request_;
            std::size_t last_reply_ = 0;
        };

        class RegionSink final : public ReplySink
        {
        public:
            explicit RegionSink(std::shared_ptr<Pool> pool) : pool_(std::move(pool)) {}
            ~RegionSink() override
            {
                if (region_.data != nullptr)
                    pool_->put(region_);
            }
            char *reserve(std::size_t bytes) override
            {
                if (region_.capacity < bytes)
                {
                    if (region_.data != nullptr)
                        pool_->put(region_);
                    region_ = pool_->get(bytes);
                }
                return region_.data;
            }
            const Region &region() const { return region_; }

        private:
            std::shared_ptr<Pool> pool_;
            Region region_;
        };

        class RdmaServer final : public RpcServer
        {
        public:
            RdmaServer(const Endpoint &endpoint, RpcHandler handler) : handler_(std::move(handler))
            {
                events_ = rdma_create_event_channel();
                if (events_ == nullptr || rdma_create_id(events_, &listener_, nullptr, RDMA_PS_TCP) != 0)
                    fail("rdma_create_id");
                auto *address = resolve(endpoint, true);
                const int bound = rdma_bind_addr(listener_, address->ai_addr);
                ::freeaddrinfo(address);
                if (bound != 0 || rdma_listen(listener_, 64) != 0)
                    fail("cannot listen on " + endpoint.text());
                port_ = ntohs(rdma_get_src_port(listener_));
            }

            ~RdmaServer() override
            {
                stop();
                // Disconnecting flushes each queue pair, which wakes and ends its serving thread.
                // The connection ids and the event channel are left to process exit: destroying an
                // id blocks until its pending events (nobody reads them any more) are acknowledged.
                for (auto &[id, connection] : connections_)
                {
                    rdma_disconnect(id);
                    if (connection->thread.joinable())
                        connection->thread.join();
                }
            }

            std::uint16_t port() const override { return port_; }

            void run() override
            {
                pollfd fd{events_->fd, POLLIN, 0};
                while (!stopping_)
                {
                    if (::poll(&fd, 1, 200) <= 0)
                        continue;
                    rdma_cm_event *event = nullptr;
                    if (rdma_get_cm_event(events_, &event) != 0)
                        continue;
                    const auto type = event->event;
                    auto *id = event->id;
                    if (type == RDMA_CM_EVENT_CONNECT_REQUEST)
                    {
                        try
                        {
                            accept(id);
                        }
                        catch (const std::exception &)
                        {
                            rdma_reject(id, nullptr, 0);
                        }
                        rdma_ack_cm_event(event);
                    }
                    else if (type == RDMA_CM_EVENT_DISCONNECTED)
                    {
                        rdma_ack_cm_event(event);
                        close(id);
                    }
                    else
                    {
                        rdma_ack_cm_event(event);
                    }
                }
            }

            void stop() override { stopping_ = true; }

        private:
            struct Connection
            {
                std::unique_ptr<Channel> channel;
                std::shared_ptr<Pool> pool;
                std::thread thread;
            };

            void accept(rdma_cm_id *id)
            {
                auto pd = std::make_shared<Pd>(id->verbs);
                auto connection = std::make_unique<Connection>();
                connection->channel = std::make_unique<Channel>(id, pd);
                connection->pool = std::make_shared<Pool>(pd);
                auto parameters = connection->channel->connection_parameters();
                if (rdma_accept(id, &parameters) != 0)
                    fail("rdma_accept");
                auto *raw = connection.get();
                connection->thread = std::thread([this, raw] { serve(*raw); });
                connections_[id] = std::move(connection);
            }

            void close(rdma_cm_id *id)
            {
                const auto it = connections_.find(id);
                if (it == connections_.end())
                    return;
                rdma_disconnect(id);
                if (it->second->thread.joinable())
                    it->second->thread.join();
                connections_.erase(it);
                rdma_destroy_id(id);
            }

            void serve(Connection &connection)
            {
                auto &channel = *connection.channel;
                Region request;
                RegionSink sink(connection.pool);
                try
                {
                    while (!stopping_)
                    {
                        const auto received = channel.receive();
                        const auto &message = received.ctrl;
                        if (message.type != kRequest && message.type != kRequestPush && message.type != kRequestInline)
                            continue;
                        const auto request_bytes =
                            message.type == kRequestInline ? received.payload.size() : message.request_bytes;
                        if (request.capacity < request_bytes)
                        {
                            if (request.data != nullptr)
                                connection.pool->put(request);
                            request = connection.pool->get(request_bytes);
                        }
                        if (message.type == kRequestInline)
                        {
                            std::memcpy(request.data, received.payload.data(), request_bytes);
                        }
                        else if (message.type == kRequest)
                        {
                            channel.transfer(IBV_WR_RDMA_READ, request, message.request_addr, message.request_rkey,
                                             message.request_bytes);
                        }
                        else
                        {
                            Ctrl target;
                            target.type = kRequestBuffer;
                            target.request_addr = reinterpret_cast<std::uint64_t>(request.data);
                            target.request_rkey = request.mr->rkey;
                            channel.send(target);
                            Ctrl ready;
                            do
                                ready = channel.receive().ctrl;
                            while (ready.type != kRequestReady);
                        }
                        const auto bytes = handler_(request.data, request_bytes, sink);
                        if (bytes <= kInline)
                        {
                            Ctrl done;
                            done.type = kDoneInline;
                            channel.send(done, sink.region().data, bytes);
                            continue;
                        }
                        auto reply_addr = message.reply_addr;
                        auto reply_rkey = message.reply_rkey;
                        if (bytes > message.reply_capacity)
                        {
                            Ctrl need;
                            need.type = kNeed;
                            need.reply_bytes = bytes;
                            channel.send(need);
                            Ctrl buffer;
                            do
                                buffer = channel.receive().ctrl;
                            while (buffer.type != kReplyBuffer);
                            reply_addr = buffer.reply_addr;
                            reply_rkey = buffer.reply_rkey;
                        }
                        channel.transfer(IBV_WR_RDMA_WRITE, sink.region(), reply_addr, reply_rkey, bytes);
                        Ctrl done;
                        done.type = kDone;
                        done.reply_bytes = bytes;
                        channel.send(done);
                    }
                }
                catch (const std::exception &)
                {
                    // Flushed on disconnect, or the client vanished; it falls back to local compaction.
                }
                if (request.data != nullptr)
                    connection.pool->put(request);
            }

            RpcHandler handler_;
            rdma_event_channel *events_ = nullptr;
            rdma_cm_id *listener_ = nullptr;
            std::uint16_t port_ = 0;
            std::atomic<bool> stopping_{false};
            std::map<rdma_cm_id *, std::unique_ptr<Connection>> connections_;
        };
    }

    std::unique_ptr<RpcClient> connect_rdma(const Endpoint &endpoint) { return std::make_unique<RdmaClient>(endpoint); }

    std::unique_ptr<RpcServer> listen_rdma(const Endpoint &endpoint, RpcHandler handler)
    {
        return std::make_unique<RdmaServer>(endpoint, std::move(handler));
    }
}
