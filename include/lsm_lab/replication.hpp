#pragma once

// Primary-backup replication over the remote-compaction transports (TCP or RDMA).
//
// physical: the primary mirrors every file write in its database directory (WAL, SST,
//   MANIFEST, CURRENT, OPTIONS) to each backup before RocksDB continues, so backups hold
//   an exact copy of the primary's files and never run compaction themselves
//   ("compact once"). WAL data is shipped on every WAL flush, i.e. before a write is
//   acknowledged; other files are shipped in large chunks and on sync/close.
// logical: the primary sends each write batch to every backup before applying it; each
//   backup runs its own RocksDB (with any compaction backend) and compacts itself.
//
// A backup is an lsm_replica_server process. digest() opens a backup's copy (or its
// database) and returns a digest of every key and value, which the primary compares with
// its own.

#include "lsm_lab/db_options.hpp"
#include "lsm_lab/remote_merge.hpp"

#include <nlohmann/json.hpp>
#include <rocksdb/db.h>
#include <rocksdb/file_system.h>
#include <rocksdb/write_batch.h>

#include <memory>
#include <string>
#include <vector>

namespace lsm_lab
{
    enum class ReplicaOp : std::uint32_t
    {
        create = 1,
        append = 2,
        close = 3,
        truncate = 4,
        rename = 5,
        remove = 6,
        link = 7,
        write_batch = 10,
        open_database = 11,
        digest = 12,
        statistics = 13,
        // Logical replica: wait until its compaction is idle (catch-up), as the primary does.
        wait = 14,
        // Empties the backup's directory (before bootstrap).
        reset = 15,
    };

    // Connections to every backup.
    class ReplicaSet
    {
    public:
        explicit ReplicaSet(std::vector<std::string> endpoints);
        ~ReplicaSet();
        ReplicaSet(const ReplicaSet &) = delete;
        ReplicaSet &operator=(const ReplicaSet &) = delete;

        // Sends one operation to every backup and waits for all of them. Returns the first error
        // (with the backup's message); *replies receives each backup's reply text when given.
        rocksdb::Status broadcast(ReplicaOp op, const std::string &path, std::uint64_t offset, const char *data,
                                  std::size_t bytes, std::vector<std::string> *replies = nullptr);
        const std::vector<std::string> &endpoints() const;
        // Operations, bytes and time spent replicating, as JSON.
        nlohmann::json statistics() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    // File system that mirrors writes under root (a directory) to the replica set.
    std::shared_ptr<rocksdb::FileSystem> make_replicating_file_system(std::shared_ptr<rocksdb::FileSystem> base,
                                                                      const std::string &root,
                                                                      std::shared_ptr<ReplicaSet> replicas);

    // Copies every database file under root to each backup (after a reset), so backups start
    // from the primary's exact files. Run before the primary opens its database.
    rocksdb::Status bootstrap_replicas(const std::string &root, ReplicaSet &replicas);

    // Digest of every key and value ("<count>:<hex>"), read with checksum verification.
    std::string database_digest(rocksdb::DB *db);

    // Handler for lsm_replica_server: stores mirrored files (physical) or applies write batches
    // to its own database (logical) under directory.
    RpcHandler make_replica_handler(const std::string &directory);
}
