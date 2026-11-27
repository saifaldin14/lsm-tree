# GPU compaction over RDMA (r7)

Medians of 3 warm repetitions; every run matched native RocksDB.

| Workload | Design | Compaction time (s) | Database-node CPU (s/GiB) | Hops (s) |
|---|---|---:|---:|---|
| 128 B values, LZ4 | Native RocksDB | 5.28 | 8.17 | native 5.28 |
| 128 B values, LZ4 | Remote CPU server, RDMA | 10.39 | 1.27 | db 0.68, rdma 0.25, cpu_server 9.46 |
| 128 B values, LZ4 | Remote GPU, TCP (1 GbE) | 11.62 | 4.46 | db 0.72, gbe 10.56, gpu 0.35 |
| 128 B values, LZ4 | Remote GPU, RDMA + relay | 11.88 | 1.28 | db 0.67, rdma 0.24, relay 0.05, gbe 10.57, gpu 0.34 |
| 128 B values, LZ4 | Native RocksDB, direct testbed | 3.66 | 5.65 | native 3.66 |
| 128 B values, LZ4 | Remote GPU, direct RDMA | 0.91 | 1.00 | db 0.50, rdma 0.20, gpu 0.21 |
| 128 B values, LZ4 | Native RocksDB on the GPU host | 6.69 | 10.35 |  |
| 128 B values, LZ4 | GPU in the database process (GPU host) | 0.86 | 1.21 |  |
| 128 B values, LZ4 | Remote GPU over loopback TCP (GPU host) | 2.08 | 2.77 | db 0.67, loopback 1.06, gpu 0.34 |
| 32 B values, LZ4 | Native RocksDB | 9.26 | 13.22 | native 9.26 |
| 32 B values, LZ4 | Remote CPU server, RDMA | 19.92 | 1.37 | db 0.81, rdma 0.29, cpu_server 18.82 |
| 32 B values, LZ4 | Remote GPU, TCP (1 GbE) | 13.31 | 4.71 | db 0.87, gbe 12.00, gpu 0.44 |
| 32 B values, LZ4 | Remote GPU, RDMA + relay | 13.61 | 1.37 | db 0.81, rdma 0.29, relay 0.05, gbe 12.02, gpu 0.43 |
| 32 B values, LZ4 | Native RocksDB, direct testbed | 6.38 | 9.11 | native 6.38 |
| 32 B values, LZ4 | Remote GPU, direct RDMA | 1.10 | 1.10 | db 0.63, rdma 0.23, gpu 0.25 |
| 32 B values, LZ4 | Native RocksDB on the GPU host | 11.49 | 16.42 |  |
| 32 B values, LZ4 | GPU in the database process (GPU host) | 1.01 | 1.17 |  |
| 32 B values, LZ4 | Remote GPU over loopback TCP (GPU host) | 2.53 | 2.99 | db 0.88, loopback 1.22, gpu 0.43 |
| 128 B values, ZSTD | Native RocksDB | 8.35 | 14.32 | native 8.35 |
| 128 B values, ZSTD | Remote CPU server, RDMA | 14.50 | 1.35 | db 0.66, rdma 0.23, cpu_server 13.60 |
| 128 B values, ZSTD | Remote GPU, TCP (1 GbE) | 11.31 | 4.61 | db 0.72, gbe 10.02, gpu 0.56 |
| 128 B values, ZSTD | Remote GPU, RDMA + relay | 11.53 | 1.35 | db 0.67, rdma 0.23, relay 0.05, gbe 10.01, gpu 0.56 |
| 128 B values, ZSTD | Native RocksDB, direct testbed | 6.77 | 11.61 | native 6.77 |
| 128 B values, ZSTD | Remote GPU, direct RDMA | 1.05 | 1.05 | db 0.50, rdma 0.19, gpu 0.35 |
| 128 B values, ZSTD | Native RocksDB on the GPU host | 11.77 | 20.22 |  |
| 128 B values, ZSTD | GPU in the database process (GPU host) | 1.09 | 1.28 |  |
| 128 B values, ZSTD | Remote GPU over loopback TCP (GPU host) | 2.25 | 2.86 | db 0.68, loopback 1.00, gpu 0.56 |

## Whole database (r8)

Batched writes (100 per batch, WAL off), 4 client threads and 4 background jobs on the database node; medians over repetitions; every run checked all keys and values.

| Design | Writes/s | Writes stalled | Database-node cores used | Database-node CPU per write (µs) | Remote jobs | Fallbacks | Runs |
|---|---:|---:|---:|---:|---:|---:|---:|
| Native RocksDB | 545k | 1% | 3.49 | 6.40 | 0 | 0 | 3 |
| Remote CPU server, RDMA | 363k | 14% | 1.35 | 3.82 | 105 | 0 | 3 |
| Remote GPU, RDMA + relay | 210k | 18% | 0.85 | 3.99 | 87 | 0 | 3 |
