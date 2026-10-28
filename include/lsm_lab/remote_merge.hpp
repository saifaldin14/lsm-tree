#pragma once

// Remote merged input: a database process sends a compaction's merge request and its
// input blocks to a compaction server (lsm_compaction_server) over TCP or RDMA, and
// receives the finished data blocks (or the merged entry stream). RocksDB's
// CompactionJob, file cuts and installation stay in the database process.

#include "lsm_lab/block_transform.hpp"
#include "lsm_lab/merged_input.hpp"

#include <rocksdb/compaction_block_transform.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace lsm_lab
{
    // tcp://host:port or rdma://host:port (host is an address on the RDMA-capable interface).
    struct Endpoint
    {
        std::string scheme;
        std::string host;
        std::uint16_t port = 0;
        std::string text() const { return scheme + "://" + host + ":" + std::to_string(port); }
    };
    Endpoint parse_endpoint(const std::string &text);
    bool rdma_supported();

    // ---- wire format (same build on both sides; x86-64 little-endian)

    enum class RpcKind : std::uint32_t
    {
        merge = 1,
        statistics = 2,
        // Transport benchmark: the request carries blocks_bytes of payload and asks for a
        // reply of meta_bytes bytes.
        echo = 3,
    };

    // Request frame: RequestHeader, metadata (encode_merge_request), padding to 64 bytes,
    // then the serialized input blocks exactly as read_merge_blocks produces them.
    struct RequestHeader
    {
        std::uint32_t magic = 0x4c534d52; // "LSMR"
        RpcKind kind = RpcKind::merge;
        std::uint64_t meta_bytes = 0;
        std::uint64_t blocks_offset = 0;
        std::uint64_t blocks_bytes = 0;
    };

    // Reply frame: ReplyHeader, message (status text or statistics JSON), padding to 64
    // bytes, then the payload: for built output the CompactionBuiltBlock array, contents,
    // keys and filter hashes (each 64-byte aligned); for an entry stream the packed data
    // and the CompactionMergedEntry array.
    struct ReplyHeader
    {
        std::uint32_t magic = 0x4c534d41; // "LSMA"
        std::uint32_t status = 0;         // rocksdb::Status::Code
        std::uint32_t subcode = 0;
        std::uint32_t kind = 0;           // 0 status only, 1 built output, 2 entry stream
        std::uint64_t message_bytes = 0;
        std::uint64_t payload_offset = 0;
        std::uint64_t block_count = 0;
        std::uint64_t contents_bytes = 0;
        std::uint64_t keys_bytes = 0;
        std::uint64_t hash_count = 0;
        std::uint64_t entries = 0;
        std::uint64_t packed_bytes = 0;
        rocksdb::CompactionMergeDropped dropped;
        MergeRunStats stats;
        std::uint64_t server_ns = 0;
    };

    std::string encode_merge_request(const rocksdb::CompactionMergeRequest &request);
    rocksdb::Status decode_merge_request(const char *data, std::size_t bytes, rocksdb::CompactionMergeRequest *request);

    // Serves read_merge_blocks from memory on the calling thread: while an instance is alive,
    // read_merge_blocks copies these bytes instead of reading the listed files.
    class PreloadedMergeBlocks
    {
    public:
        PreloadedMergeBlocks(const char *data, std::size_t bytes);
        ~PreloadedMergeBlocks();
        PreloadedMergeBlocks(const PreloadedMergeBlocks &) = delete;
        PreloadedMergeBlocks &operator=(const PreloadedMergeBlocks &) = delete;
        static const PreloadedMergeBlocks *current();
        const char *data() const { return data_; }
        std::size_t bytes() const { return bytes_; }

    private:
        const char *data_;
        std::size_t bytes_;
        const PreloadedMergeBlocks *previous_;
    };

    // ---- transports

    // One request in flight at a time. Buffers are registered once and reused (RDMA).
    class RpcClient
    {
    public:
        virtual ~RpcClient() = default;
        // At least bytes of writable memory for the next request; valid until call returns.
        virtual char *request_buffer(std::size_t bytes) = 0;
        // Sends the first request_bytes of request_buffer. The reply stays valid while *reply lives.
        virtual rocksdb::Status call(std::size_t request_bytes, std::shared_ptr<const char> *reply,
                                     std::size_t *reply_bytes) = 0;
        // call() in two halves, so one thread can have requests outstanding on several
        // connections (e.g. one per backup): send() transmits the request and finish() waits for
        // its reply. The defaults simply run call() in finish().
        virtual rocksdb::Status send(std::size_t request_bytes)
        {
            pending_bytes_ = request_bytes;
            return rocksdb::Status::OK();
        }
        virtual rocksdb::Status finish(std::shared_ptr<const char> *reply, std::size_t *reply_bytes)
        {
            return call(pending_bytes_, reply, reply_bytes);
        }
        virtual std::string describe() const = 0;

    protected:
        std::size_t pending_bytes_ = 0;
    };
    std::unique_ptr<RpcClient> connect_rpc(const Endpoint &endpoint);

    class ReplySink
    {
    public:
        virtual ~ReplySink() = default;
        // At least bytes of writable memory for the reply; the handler returns how many it used.
        virtual char *reserve(std::size_t bytes) = 0;
    };
    using RpcHandler = std::function<std::size_t(const char *request, std::size_t bytes, ReplySink &sink)>;

    class RpcServer
    {
    public:
        virtual ~RpcServer() = default;
        virtual std::uint16_t port() const = 0;
        // Serves until stop(); each connection runs on its own thread.
        virtual void run() = 0;
        virtual void stop() = 0;
    };
    std::unique_ptr<RpcServer> listen_rpc(const Endpoint &endpoint, RpcHandler handler);

    // ---- client transform and server handler

    // Transform that offloads merged input to a compaction server. endpoint defaults to
    // LSM_REMOTE_ENDPOINT; LSM_REMOTE_CONNECTIONS (default 2) bounds concurrent jobs.
    std::shared_ptr<BatchTransform> make_remote_transform(const std::string &endpoint = {});
    // Server statistics (JSON) fetched over a new connection.
    std::string fetch_remote_statistics(const std::string &endpoint);

    // Handles merge and statistics requests with the given local transform.
    RpcHandler make_merge_handler(std::shared_ptr<BatchTransform> transform);
}
