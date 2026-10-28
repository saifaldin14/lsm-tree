# Remote compaction and replication over RDMA

The GPU compaction in [gpu-compaction.md](gpu-compaction.md) runs inside the database
process. This document covers two extensions:

- **Remote compaction:** move compaction to a shared compaction server.
- **Replication:** replicate the database so compaction runs only once.

Both use TCP or RDMA. The plan and hypotheses (H13–H17) are in
[publication/PREREGISTRATION.md](../publication/PREREGISTRATION.md).

## Remote compaction

`remote_merge` is a compaction transform
([include/lsm_lab/remote_merge.hpp](../include/lsm_lab/remote_merge.hpp)). It plugs in where
`cuda_merge` does, through the same `MergeCompactionInput` hook, but sends the work over
the network.

1. RocksDB's `CompactionJob` runs on the database node as usual and builds the merge
   request: input blocks, key bounds, snapshots and output format.
2. The database node reads the input blocks straight into a registered buffer and sends
   the request with the blocks to `lsm_compaction_server`.
3. The server runs its backend, `cuda_merge` (GPU) or `cpu_merge` (CPU), on the received
   bytes. A thread-local `PreloadedMergeBlocks` stands in for the input files, so the
   merge engines are unchanged.
4. The server returns the finished data blocks, boundary keys and filter hashes. If the
   job cannot be built, it returns the merged entry stream instead.
5. The database node writes the files, index, filter and footer, and installs the result,
   exactly as for local GPU compaction.

Properties:

- **No shared filesystem:** the input travels with the request. RocksDB's own
  `CompactionService` (wrapped by the gRPC worker in
  [src/engine/remote_compaction.cpp](../src/engine/remote_compaction.cpp)) needs one.
- **Same output checks:** the output passes the same differential checks as local
  compaction (`tests/remote_compaction_test.sh`).
- **Falls back on failure:** if the server fails, disconnects or times out, that job runs
  on the CPU in the database process and is counted in `merge_fallbacks`.
  `lsm_compaction_server --crash-after N` injects failures for testing.

### Transports

| Transport | Mechanism |
|---|---|
| `tcp://host:port` | Length-prefixed messages over one socket per connection. |
| `rdma://host:port` | RC queue pairs set up with librdmacm. Requests and replies move with one-sided RDMA operations between pre-registered buffers that are reused across jobs. |

RDMA protocol for one call:

1. **Request.**
   - *Push (default):* the client asks for a request buffer; the server advertises one;
     the client RDMA-WRITEs the request into it and sends "ready".
   - *Pull (`LSM_RDMA_PUSH=0`):* the client sends the address of its request buffer;
     the server RDMA-READs it.
2. **Reply.** The server RDMA-WRITEs the reply into the reply buffer the client
   advertised. If the reply is larger, it first asks for a bigger buffer.
3. **Done.** The server SENDs "done"; RC ordering makes the reply visible first.

Waits block on a completion channel (`LSM_RDMA_POLL=1` busy-polls instead). A client also
watches its connection events and gives up after `LSM_REMOTE_TIMEOUT_MS` (default 120 s)
without progress.

On ConnectX-6 Dx at 100 Gb/s, push reaches about 90 Gb/s for 128 MiB transfers, against
about 92 Gb/s for `ib_write_bw`. Pull is limited by RDMA READ to about 50 Gb/s
(`ib_read_bw` reaches 51 Gb/s). The client spends about 0.012 CPU-seconds per GiB moved
over RDMA, against about 0.7 over TCP.

### Configuration

| Variable | Effect |
|---|---|
| `LSM_REMOTE_ENDPOINT` | Server address for `remote_merge`. |
| `LSM_REMOTE_CONNECTIONS` | Concurrent remote jobs per database process (default 2). |
| `LSM_RDMA_PUSH` | `1` (default) pushes requests with RDMA WRITE; `0` lets the server pull them with RDMA READ. |
| `LSM_RDMA_POLL` | `1` busy-polls completions instead of blocking. |
| `LSM_REMOTE_TIMEOUT_MS` | Client timeout without progress (default 120000). |

The transform's statistics add `merge_remote_ns`, `merge_server_ns`, `merge_sent_bytes`
and `merge_received_bytes`.

Tools:

- `lsm_rpc_bench` measures the transport alone with echo requests.
- `lsm_compaction_server` prints its backend's statistics on SIGTERM.

## Replication

`lsm_system_bench --replication physical|logical --replicate-to <endpoints>` replicates the
primary to backups (`lsm_replica_server`)
([include/lsm_lab/replication.hpp](../include/lsm_lab/replication.hpp)).

| Mode | Primary | Backups |
|---|---|---|
| `physical` (compact once) | A RocksDB `FileSystem` wrapper mirrors every file write in the database directory: WAL data on every WAL flush (before the write is acknowledged); SST and MANIFEST data in 4 MiB chunks and on sync and close; creates, renames and deletes as they happen. | Hold an exact copy of the primary's files and never compact. |
| `logical` (per-replica compaction) | Sends each write batch to every backup before applying it. | Run their own RocksDB with the primary's options (`lsm_lab/db_options.hpp`) and any compaction backend (`--replica-backend`), and compact independently. |

How a run proceeds:

1. **Bootstrap.** Before the run, the primary resets each backup and sends it the
   primary's files, so backups start identical.
2. **Run.** Writes are replicated as described above.
3. **Check.** After the run, the primary computes a digest of every key and value. Each
   backup computes the same digest:
   - a physical backup opens its copy read-only with stock RocksDB, as a failover would;
   - a logical backup first finishes its compaction.

   Any mismatch fails the run (`tests/replication_test.sh`).

Combined with `--mode remote_merge`, physical replication compacts once, on the
compaction server, for the whole replica set.

## Results so far (CPU compaction server)

These were measured on two machines linked at 100 Gb/s (ConnectX-6 Dx, RoCE), with the
compaction server running the CPU merge (`cpu_merge`). Details, figures and tables are in
the [README](../README.md#5-remote-compaction-and-replication-over-rdma).

- **Transport:** RDMA push reaches 92 Gb/s (the `ib_write_bw` ceiling) and costs the
  client about 0.012 CPU-seconds per GiB. TCP reaches 6–7 Gb/s at about 1 CPU-second
  per GiB.
- **Remote compaction:** the database node's CPU per GiB compacted falls to 1.7–2.0
  seconds over RDMA, against 4.7–15.0 for native RocksDB; TCP needs 3.2–3.6. RDMA adds
  2–12% to compaction time compared with running the same merge locally.
- **Replication with 2 backups:** compacting once and shipping the files cuts cluster CPU
  per write by 3.4× for batched writes (same throughput) and by 1.8× for single writes
  (25% lower throughput, see Limits), for about 3× more replication traffic.

## Limits

- **No failover protocol.** If a backup fails during a run, the primary's write fails and
  RocksDB stops accepting writes. Recovery is out of scope; the read-only open checks
  only that a backup's copy is complete.
- **Sequential replication.** Operations go to backups one after another, so latency grows
  with the number of backups.
- **WAL shipping is serialised.** Physical replication ships each WAL append inside
  RocksDB's write path, where only one write group holds the WAL at a time. With single
  writes, the network round trips therefore serialise, and throughput is 25% below
  logical replication. Shipping the WAL asynchronously with group commit would remove
  this.
- **Slow CPU merge on the server.** The server's CPU backend (`cpu_merge`) is a
  reference implementation about 2× slower than RocksDB's own compaction. CPU-server
  results are therefore a lower bound for remote CPU compaction.
- **Extra hops for SSTs.** With remote compaction plus physical replication, each output
  file crosses the network twice: server to primary, then primary to each backup.
- **GPUDirect RDMA is not used.** On the server, received data passes through host memory
  before the GPU. Data-center GPUs could receive directly with GPUDirect RDMA.
