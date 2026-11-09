# Related work

Every earlier system discussed here is either rebuilt inside our RocksDB and measured on
the same harness, or run unmodified from its published code. Numbers in this section come
from [data/summary.md](data/summary.md) and [data/rdma/r7/summary.md](data/rdma/r7/summary.md),
not from the original papers. Published speedups are not comparable with ours because the
earlier GPU and FPGA systems run on different stores (LevelDB, X-Engine, key-value-separated
stores) and different hardware.

## 1. Offloading compaction to a GPU or FPGA

Earlier accelerator work moves part of a compaction off the CPU. The designs differ in how
much of the job they take. A RocksDB compaction has five steps: read and decompress the
input, sort and drop outdated versions, pack survivors into blocks, compress and checksum
each block, and write the file with its index, filter and metadata.

| Design | Stands in for | On the accelerator | Left on the CPU |
| --- | --- | --- | --- |
| Sort/merge offload | gLSM, gParaKV | unpack, sort, merge, drop hidden versions | input decompression, `CompactionIterator`, block building, compression, checksums |
| Pipeline offload without compression | LUDA (MSST'24), Alibaba's FPGA design (FAST'20) | unpack, sort, merge, deletions, block building, filter hashes | input decompression, output compression and checksums |
| Ours | — | steps 1 to 4 and the Bloom filter | file cuts, index, metadata, file writing |

### How the rebuilds work

Each rebuild uses our GPU code for the steps that design offloads and RocksDB's own CPU
code for every step it does not. Every rebuild passes the same exact differential check
against native RocksDB as our own design. Construction and fairness rules are preregistered
in [PREREGISTRATION.md](PREREGISTRATION.md).

The rebuilds are deliberately generous to the earlier designs. Input decompression runs on
four parallel read threads rather than serially, and LUDA's CPU-assisted sort is replaced by
our faster GPU sort. Two things are not reproduced: MSST'24's direct SSD-to-GPU transfers,
which cannot help when the data is on `/dev/shm`, and the separate value storage used by
gLSM and gParaKV, which our GPU path does not support. The second is covered separately in
section 2 by running gParaKV's own code.

### Isolated compaction

CPU seconds per GiB, median of 3 warm repetitions, lower is better.

| Workload | Native | Sort/merge | Pipeline | Ours | Ours vs best earlier |
| --- | ---: | ---: | ---: | ---: | ---: |
| 32 B values, LZ4 | 16.93 | 12.40 | 6.08 | **2.31** | 2.6x |
| 128 B values, LZ4 | 11.04 | 8.21 | 5.44 | **2.10** | 2.6x |
| 1 KB values, LZ4 | 5.69 | 4.58 | 3.92 | **1.96** | 2.0x |
| 128 B values, ZSTD | 20.46 | 17.32 | 14.15 | **2.19** | 6.5x |
| 128 B values, 10% deletes, LZ4 | 10.90 | 8.26 | 5.45 | **2.13** | 2.6x |
| 128 B values, no compression | 5.10 | 3.44 | 1.81 | **1.80** | 1.0x |

The ordering holds in every compressed workload and the gap widens with compression cost,
from 2.0x at 1 KB values with LZ4 to 6.5x with ZSTD. This confirms preregistered hypothesis
H9.

### Whole database

Batched writes on 4 cores, throughput counted to the end of all resulting compaction.
Medians with 95% bootstrap confidence intervals, ratios paired against native.

| Compression | Native | Sort/merge | Pipeline | Ours |
| --- | ---: | ---: | ---: | ---: |
| LZ4 (5 runs) | 350 kops/s | 397 (1.14x) | 496 (1.41x) | **616 (1.75x)** |
| ZSTD (3 runs) | 209 kops/s | 235 (1.16x) | 246 (1.17x) | **475 (2.27x)** |
| None (3 runs) | 424 kops/s | 401 (0.98x) | 576 (1.34x) | **586 (1.40x)** |

With LZ4 the pipeline rebuild's gain over native (0.41) is less than half of ours (0.75),
confirming H10. Catch-up compaction after the run separates the designs further: 76.3 s for
native, 56.7 s for sort/merge, 22.8 s for the pipeline rebuild, and 2.7 s for ours. Write
stalls fall from 6% of the run to 0%, and p99 write latency from 6.29 ms to 1.25 ms.

At equal hardware cost on 8 cores, our design reaches 1,006 kops/s against native RocksDB's
1,057 kops/s on 32 cores, while the pipeline rebuild reaches 804 kops/s. H11 holds.

### Where the advantage comes from, and where it stops

With no compression our design and the pipeline rebuild are indistinguishable: 1.80 against
1.81 CPU seconds per GiB isolated, and 586 against 576 kops/s on the whole database. This is
the preregistered H8 result and it is the honest boundary of the contribution. Our advantage
is that we also move compression and its checksums onto the GPU. Since RocksDB normally runs
with compression enabled, compression is most of what the earlier designs leave behind, but
a deployment that disables it would see no benefit from our work over LUDA's design.

## 2. Key-value separation

gParaKV (SC'25) is the only earlier system in this area with public code
(<https://github.com/KV890/gParaKV>). We ran it unmodified under its own YCSB driver, with
one client, 4 cores and the same GPU, and connected RocksDB with and without our offload to
the same driver. Median of 3 repetitions.

| Values | System | YCSB A | CPU seconds |
| --- | --- | ---: | ---: |
| 128 B | gParaKV | **238 kops/s** | 350 |
| 128 B | RocksDB, LZ4 | 178 | 543 |
| 128 B | RocksDB + ours, LZ4 | 182 | **346** |
| 1 KB | gParaKV | **251 kops/s** | **105** |
| 1 KB | RocksDB BlobDB | 168 | 219 |
| 1 KB | RocksDB + ours, LZ4 | 160 | 221 |

**gParaKV is faster under its own driver, and we do not claim otherwise.** It is a different
design: values live in a separate uncompressed log outside the sorted files, on top of
LevelDB, so it moves far fewer bytes through compaction in the first place. A single client
also does not keep RocksDB's compaction busy, so our offload shows up as a 36% CPU reduction
at 128 B values rather than as throughput. H12 was preregistered as descriptive with no
directional hypothesis for this reason.

Value separation and compaction offload are complementary rather than competing. Combining
them is future work.

## 3. Remote and disaggregated compaction

Two lines of work move compaction off the database node. Neither uses a GPU, and we found no
published shared GPU compaction service.

**Shipping compaction results to replicas.** Tebis (EuroSys'22) has the primary compact and
ship its index to backups over RDMA. RubbleDB (ATC'23) compacts once and ships the resulting
SSTs to replicas over NVMe-oF.

**Offloading compaction to other nodes.** Nova-LSM (SIGMOD'21) offloads compaction to storage
nodes over RDMA. dLSM (ICDE'23) builds an LSM-tree on RDMA-disaggregated memory. CaaS-LSM
(SIGMOD'24) provides remote CPU compaction as a service for RocksDB.

Our remote-CPU and compact-once-on-CPU configurations stand in for these designs. Measured
on 128 B values with LZ4, median of 3 warm repetitions:

| Design | Compaction time | Database-node CPU |
| --- | ---: | ---: |
| Native RocksDB | 5.28 s | 8.17 s/GiB |
| GPU in the database process | **0.86 s** | 1.21 s/GiB |
| Remote CPU server over RDMA | 10.39 s | 1.27 s/GiB |
| Remote GPU over RDMA plus relay | 11.88 s | **1.28 s/GiB** |
| Remote GPU over TCP (1 GbE) | 11.62 s | 4.46 s/GiB |

Remote compaction removes 6.4x of the database node's compaction CPU, which is the result
these systems are built to deliver, and our RDMA path delivers it. **Wall-clock time is a
loss on our testbed.** The GPU machine is not on the RoCE fabric, so every remote GPU job
crosses a 1 GbE link, and 10.57 s of the 11.88 s total is that single hop. RDMA and TCP are
within 2% of each other here because neither transport is the bottleneck, so preregistered
H13 and H14 cannot be evaluated on this hardware. A database-level run shows the same effect:
throughput falls from 545 kops/s native to 210 kops/s with remote GPU compaction.

Putting the GPU on the RoCE fabric is a topology change, not a design change, but until it is
measured we make no claim about remote GPU compaction latency.

## 4. Workload models

The real-world workloads come from published models rather than invented ones. Meta's
production access patterns use the ZippyDB model from Cao et al., "Characterizing, Modeling,
and Benchmarking RocksDB Key-Value Workloads at Facebook" (FAST'20), run through RocksDB's
own `db_bench --benchmarks=mixgraph`. The stream-processing state store, storage metadata and
time-series models are described in [../docs/workloads.md](../docs/workloads.md).

## 5. What is new here

1. **The whole compaction on the GPU, including compression.** Earlier GPU and FPGA designs
   stop before compression. Rebuilt on the same harness, that gap is worth 2.6x to 6.5x in
   compaction CPU and up to 1.93x in database throughput. Without compression the advantage
   disappears.
2. **Measured inside production RocksDB.** RocksDB 11.8 with its own file cuts, index,
   filters and installation, verified byte-for-byte against native output on every run,
   rather than a standalone merge kernel or a research store.
3. **Earlier designs rebuilt rather than cited.** Each comparison runs in the same process,
   on the same hardware, against the same inputs, under preregistered fairness rules.
4. **A remote GPU compaction path over RDMA.** We are not aware of a prior shared GPU
   compaction service. The CPU-offload result is measured; the latency result is negative on
   this testbed and reported as such.

## Citations to complete

Short names, venues and years below were checked against the papers. Full BibTeX entries
still need to be written before submission.

| Key | Work | Venue | Status |
| --- | --- | --- | --- |
| `cao2020` | Characterizing, Modeling, and Benchmarking RocksDB Key-Value Workloads at Facebook | FAST'20 | full title known |
| `tebis2022` | Tebis | EuroSys'22 | needs authors and title |
| `rubbledb2023` | RubbleDB | ATC'23 | needs authors and title |
| `novalsm2021` | Nova-LSM | SIGMOD'21 | needs authors and title |
| `dlsm2023` | dLSM | ICDE'23 | needs authors and title |
| `caaslsm2024` | CaaS-LSM | SIGMOD'24 | needs authors and title |
| `luda2024` | LUDA | MSST'24 | needs authors and title |
| `fpga2020` | Alibaba FPGA compaction | FAST'20 | needs authors and title |
| `glsm` | gLSM | — | needs venue, authors, title |
| `gparakv2025` | gParaKV | SC'25 | code at github.com/KV890/gParaKV |
