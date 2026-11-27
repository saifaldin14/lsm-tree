# Paper guide

The one working document for writing the paper. The paper itself is
[publish_paper/main.tex](publish_paper/main.tex) and nothing else lives in that folder. Every
number in the paper comes from archived runs in [results/data](results/data); the long-form
results are in [results/FULL-RESULTS.md](results/FULL-RESULTS.md).

Build the paper: `cd publish_paper && tectonic main.tex`.

Contents: 1 How papers like ours are written · 2 What is new in ours · 3 The story ·
4 Experiment plan · 5 Critique log

---

## 1. How papers like ours are written

Twelve papers were read from their full text. They fall into three groups: designs that put
compaction on a GPU or FPGA (our direct peers), designs that move compaction to another
machine or share it as a service, and well-known LSM papers from VLDB, FAST and ATC that show
what reviewers in this area are used to.

### 1.1 The papers

| Paper | Venue | One-line claim | What proves the problem | Benchmarks | Main metrics |
|---|---|---|---|---|---|
| FPGA compaction (Zhang et al.) | FAST'20 | FPGA makes compaction 2–5× faster, up to 23% more throughput, 31.7% better energy | Throughput rises with compaction threads, then falls as cores saturate | Single compaction; db_bench (5 patterns); YCSB; inside X-Engine | Compaction speed, throughput, CPU, latency, energy |
| LUDA (Xu et al.) | arXiv 2020 | GPU compaction, up to 2× throughput, steadier p99 | Under 80% competing CPU load RocksDB compaction takes 3× longer | YCSB A only, under stress-ng CPU load; 5–50 GB | Throughput, p99, CPU/GPU use. Leaves Snappy compression as future work |
| gParaKV (Sun et al.) | SC'25 | GPU merge-sort and garbage collection for key-value-separated stores | Compaction time split into CPU and I/O; throughput under CPU load | db_bench (128 B–256 KB values, 100 GB); YCSB Load + A–F | Throughput, p90/p99, power, space amplification, GPU use |
| dLSM (Wang et al.) | ICDE'23 | LSM index for disaggregated memory, compaction near the data, 1.6–11.7× writes | RDMA: small vs large transfers differ 100× | db_bench (100 M pairs) over 100 Gb RDMA | Throughput, read latency, CPU |
| CaaS-LSM (Yu et al.) | SIGMOD'24 | Compaction as a stateless service with a scheduler, up to 8× throughput, 98% lower p99 | Throughput timeline drops as network bandwidth falls; uneven CPU use across instances | db_bench; YCSB on Cassandra; **real applications: Kvrocks (Redis) and Nebula (LDBC graph)**; failure tests; scalability | Throughput, p99, write stalls, CPU, network traffic, scheduler rate |
| RubbleDB (Li et al.) | ATC'23 | Compact once on the primary, ship files over NVMe-oF, 1.9× throughput | Compaction is up to 72% of CPU cycles | **All YCSB workloads; five Twitter production traces**; failure recovery | **Throughput per core**, tail latency, CPU time, disk and network |
| Evolution of RocksDB (Dong et al.) | FAST'21 | Lessons from production; priorities moved from write to space amplification to CPU | Production tables | None (experience paper) | — |
| Meta workloads (Cao et al.) | FAST'20 | Real RocksDB workloads differ from YCSB; MixGraph models them | YCSB reads 5× more bytes than real workloads | Trace replay | — |
| Compaction design space (Sarkar et al.) | VLDB'21 | Four primitives describe all compaction strategies; ten compared | Qualitative figure; "compaction is a black box" | Own YCSB-like workload suite | Latency, amplification, reads. **12 numbered observations, 7 takeaways** |
| Spooky (Dayan et al.) | VLDB'22 | Partitioned compaction: >2× less space and write amplification | Write amplification vs storage use | db_bench, SATA and NVMe | Amplification, throughput, p99.9. "Key takeaway" labels |
| SILK (Balmau et al.) | ATC'19 | Schedule internal I/O, up to 100× lower p99 | p99 spike timelines | **YCSB A–F; 24-hour Nutanix production trace** | p99 over time, throughput, stalls. **Numbered lessons** |
| ADOC (Yu et al.) | FAST'23 | Tune threads and batch size, 87.9% shorter stalls | Stall timelines; stalls vs threads and CPU | db_bench fillrandom; YCSB; PM, NVMe, SATA, HDD | Throughput, stall time, p99, CPU |

Not read in full (could not get the text): gLSM (TOS'24), GPComp (TPDS'25), GCLComp
(ICPADS'25), the MSST'24 GPU design, Nova-LSM, Tebis, and Meta's "Disaggregating RocksDB"
(SIGMOD'23). Their claims are taken from their abstracts and our earlier rebuilds.

### 1.2 What the typical paper does

**Section order.** Almost all use: Introduction → Background and Motivation → Design →
Implementation → Evaluation → Related Work → Conclusion. The motivation section always
proves the problem with **one measurement** before any design appears: a CPU share, a
throughput curve under CPU load, or a stall timeline. Our draft already follows this order.

**Evaluation questions.** Three to six, often stated as a list at the start of the
evaluation (LUDA, CaaS-LSM, RubbleDB). Ours has seven.

**Benchmarks.**
- *Near-universal:* db_bench write patterns, **YCSB** (7 of the 10 papers that evaluate a system; SILK,
  RubbleDB and gParaKV run **all of A–F**), a stock RocksDB baseline, throughput and p99.
- *Common:* a single-compaction micro-benchmark (FPGA, gParaKV), a value-size sweep, CPU
  use, runs under competing CPU load (LUDA, FPGA, gParaKV), datasets much larger than
  memory (LUDA 50 GB, gParaKV 100 GB), energy or power (FPGA, gParaKV).
- *How application benchmarks are used:* they come **last in the evaluation** and support
  one claim: "it also works for a real user". CaaS-LSM runs two real applications on top
  (Kvrocks, Nebula); RubbleDB and SILK replay production traces; the FPGA paper runs inside
  X-Engine. None of them builds the story on the application result.
- *Service papers* (CaaS-LSM, RubbleDB) also test **failure handling**: kill a worker and
  show the database keeps going.
- *Rare:* cost analysis, multi-tenant sharing of an accelerator, rebuilding earlier designs
  in one harness, registering hypotheses in advance, a separate limitations section.

**Presentation.** VLDB papers number their findings (Sarkar: 12 observations, 7 takeaways;
SILK and ADOC: numbered lessons). Headline numbers sit in the abstract and the last
paragraph of the introduction.

**How intros are told.** Three patterns:
1. *Bottleneck first* (LUDA, FPGA, gParaKV, RubbleDB): faster drives make compaction
   CPU-bound → measurement → accelerator → speedup.
2. *Earlier fixes don't work* (SILK, ADOC): a symptom → the obvious fixes fail →
   the real cause → design.
3. *Resource sharing* (CaaS-LSM, Evolution of RocksDB): compaction is bursty and uneven
   across instances → pool it as a service → scheduler → scale results.

---

## 2. What is new in ours

### 2.1 The 80%: what we share with the community

- The bottleneck-first intro and the motivation measurement (CPU per write, split by job).
- The section order, a single-compaction micro-benchmark, whole-database db_bench runs, YCSB,
  MixGraph from Cao et al., p99/p99.9, CPU per operation (as in RubbleDB), runs under
  competing CPU load (as in LUDA and FPGA), SATA and NVMe drives (as in Spooky and ADOC).
- Stock RocksDB as the baseline, tuned by the same search for each core count.

### 2.2 The 20%: what only we add

| Delta | Closest earlier work | Evidence we have |
|---|---|---|
| **The codec is what earlier offloads leave on the CPU.** Rebuilt in one harness, the best earlier design still uses 49% (LZ4) to 69% (ZSTD) of RocksDB's compaction CPU; with compression off the gap vanishes | LUDA leaves Snappy as future work; FPGA and MSST'24 don't compress | Prior-design rebuilds, FULL-RESULTS §9 |
| **Whole compaction inside production RocksDB**, checked against unmodified RocksDB on every compaction | Others use LevelDB (LUDA, gParaKV) or their own store (FPGA in X-Engine) | Correctness checks on every run |
| **One GPU shared by many databases as a service**, over shared memory, RDMA or TCP | CaaS-LSM shares *CPU* workers; no earlier shared GPU service | 8 remote databases at 1.99× each, GPU 98% busy (FULL-RESULTS §11) |
| **When a GPU pays: a cost model with break-even prices** | None of the 12 papers reports cost | 1.39× dearer dedicated, 0.83–0.89× shared (FULL-RESULTS §7) |
| **Registered hypotheses, and the ones that failed are reported** | None | results/PREREGISTRATION.md |

The reviewer question each delta must answer:
- Codec: "Isn't this just nvCOMP?" → The point is *where* the CPU goes; the rebuild shows it.
- Service: "Why not CaaS-LSM with CPU workers?" → Needs a CPU-pool comparison at the same
  scale (gap E5 below).
- Cost: "Prices change." → Break-even price, so readers can plug in their own.

---

## 3. The story

Told to someone who knows what a database is, but not LSM-trees.

> A key-value store like RocksDB takes writes fast by putting them in memory and dumping them
> to disk as small sorted files. To keep reads fast it must keep merging those files into
> bigger ones in the background. That merging is called compaction, and in a write-heavy
> database it uses more CPU than accepting the writes themselves.
>
> People have tried moving compaction to GPUs and FPGAs for six years. Each paper reports a
> speedup on its own store and hardware, so nobody can tell which idea matters. We rebuilt
> them all inside the same RocksDB and measured them on the same machine. The surprise: they
> all leave one step on the CPU, compressing each block before it is written, and that one
> step is half to two-thirds of the CPU they were meant to remove.
>
> So we move the whole compaction to the GPU, compression included, and let RocksDB keep
> every decision about the shape of the tree. Every compaction is checked against unmodified
> RocksDB. One GPU now does the work of 15–18 CPU cores, and a database whose bottleneck is
> compaction takes 1.5–1.8× more writes using half the CPU per write.
>
> But one database uses only a fifth of a GPU, and a GPU that sits mostly idle costs more than
> the cores it replaces. So we turn the GPU into a shared compaction service that many
> databases reach over shared memory or the network. Eight databases on another machine
> share one GPU, each still gets about twice RocksDB's writes, and the GPU is 98% busy. Only
> when it is shared like this does the GPU become the cheaper way to compact.
>
> It does not help when the drive is the bottleneck (then you only save CPU), it helps little
> for read-heavy work, and the service does not yet keep a noisy database from slowing a
> quiet one.

This sets the order of the paper and its figures:
1. CPU per write, split by job (the problem) → 2. earlier designs rebuilt, codec left over
(the insight) → 3. design → 4. one compaction → 5. whole database, reads and real
workloads → 6. one GPU, many databases → 7. cost → 8. limits.

---

## 4. Experiment plan

What reviewers in this community will expect that we do not yet have, most important first.
**Status:** E1–E6 approved. The paper is rewritten first with the results we already have,
with a red TODO where each new result will go; results are added as they are archived. E7 and
E8 came from the first critique round and are proposed, not yet approved. Each run is registered in
[results/PREREGISTRATION.md](results/PREREGISTRATION.md) first and archived in results/data.

| ID | Gap | Why reviewers will ask | Set by | Have now | Proposed run |
|---|---|---|---|---|---|
| E1 | **YCSB A–F at the headline setting** | Near-universal; we lack C, D, E (scans) and run only on 4 cores | SILK, RubbleDB, gParaKV | A, B, F on 4 cores | A–F, uniform and Zipfian, 8 and 16 cores, in memory; median of 3 |
| E2 | **A real application on top of RocksDB** | CaaS-LSM, our closest service peer, runs Kvrocks and Nebula | CaaS-LSM, FPGA (X-Engine) | Workload *models* only (MixGraph, state store) | Kvrocks (Redis protocol, RocksDB inside) with our RocksDB 11.8, redis-benchmark SET/mixed. First a feasibility check: does Kvrocks build on 11.8 with our patch? |
| E3 | **Dataset much larger than memory, on NVMe** | Our headline is in memory; peers use 50–100 GB | LUDA, gParaKV, dLSM | ~7 GB; NVMe at 50 M keys | 500 M keys (~70 GB) on NVMe with a block cache, 8 cores, write-heavy and YCSB A |
| E4 | **Failure of the service** | Service papers show what happens when a worker dies | CaaS-LSM, RubbleDB | Retry and drain code, no measured run | Kill the service mid-run with 4 databases; show fallback to CPU, throughput timeline, recovery time |
| E5 | **A pool of CPU cores as the service** (CaaS-LSM style) | "Why a GPU and not remote CPUs?" | CaaS-LSM, dLSM | Remote CPU for one database (FULL-RESULTS §10) | Same 8 databases, the service backed by N remote CPU cores instead of the GPU; cores needed to match, and cost |
| E6 | **Energy counted to the end of leftover compaction** | FPGA and gParaKV report energy; ours is recorded but not claimed | FPGA, gParaKV | Energy per trial, window ends early | Analysis first; rerun only if the recorded data can't be extended |

| E7 | **Offload at matched level-0 depth** | Our faster compactions start with fewer level-0 files, so some of the stall and latency gain may come from compacting earlier, not from the GPU | Critique round 1 (PC reviewer) | Trigger grid for ours only (WRITE-AMPLIFICATION.md) | RocksDB and ours at level-0 triggers 4, 8, 16, both designs, same tuned threads; compare throughput, stalls, p99.9 and write amplification at equal mean level-0 depth |
| E8 | **Long run near saturation** | 5-minute runs with leftover compaction show finite-run throughput, not a steady state | Critique round 1 (PC reviewer) | M5: 5-minute runs, N = 6 and 8 | N = 6 and 8 for 30 minutes; pending compaction bytes, queue length, stalls and throughput over time |

**Run status:**
- E1: registered (commit 0243b26, pushed), running on Zyra since 2026-11-26 in tmux session
  `e1` (`~/lsm-tree/e1_run.sh`, output `/tmp/sa29ali/campaigns/ycsb-full`, 162 runs). To
  resume after an interruption, rerun the script. Archive to `results/data/campaigns/` when done.
- E2–E5: registered (commit 6b70430, pushed). Drivers smoke-tested on Malphite and Pyke
  (not reported). Fresh build of the registered sources on Malphite's local disk
  (`/tmp/sa29ali/lsm-e`, CUDA 12.8, sm_86). Chain `results/e_chain.sh` runs E4 → E3 → E2 →
  E5 from Malphite with a Pyke login held open (Pyke wipes `/dev/shm` at logout).
  Deviations, all in the registration: E3 uses 100M keys with direct I/O (Malphite's NVMe
  has 49 GB free, not 70 GB); E5's CPU pool is capped at 8 cores; E2 runs over TCP without
  compaction filters, with LZ4, merged stream only.
  Blocker: another user's unpinned jobs keep Malphite at full load, including cores 0–3,
  where the GPU server is pinned. **Postponed** (2026-11-26, Saif's decision): nothing has
  been run. To resume: check Malphite is quiet (`uptime`, `ps -eo user,pcpu --sort=-pcpu`),
  check `/tmp/sa29ali/e_build.log` ends in `BUILD-OK` (`/tmp` may have been cleared; if so,
  rebuild with `~/rdma-private/e_build.sh`), check Pyke `/tmp/sa29ali/e2/` still has
  `bin/kvrocks` and `kvrocks.conf` (copied 2026-11-26; if `/tmp` was cleared, copy them again
  from jax `/tmp/sa29ali/e2/`, sha256 in the E2 registration), copy the E2 run files to Pyke
  `/tmp/sa29ali/lsm-e` (see the E2 registration), post in the Teams long-experiments
  channel, then `tmux new -d -s echain 'bash /tmp/sa29ali/lsm-e/results/e_chain.sh >
  /tmp/sa29ali/echain.log 2>&1'` on Malphite.
- E6: done (analysis of archived runs). Neither hypothesis holds: at the headline, ours uses
  1.03×, 1.20× and 1.16× RocksDB's energy per write at 4, 8 and 16 cores (RocksDB without a
  GPU). Energy saved only where RocksDB is short of CPU. In the paper (Q6) and limitations.
- E7, E8: not started.

Not proposed: a new value-size sweep or a second GPU model (already covered or out of scope).

---

## 5. Critique log

Filled in by review rounds on the rewritten paper. Each entry: who reviewed, what they found,
what we changed.

### Round 1: the rewritten paper

**Reviewers:** an AI reviewer acting as a VLDB PC member who knows RocksDB and doubts GPU
offload, and an AI reader with a foundational database background. Both were asked "what is
not coming out of the story?", "why would you reject?" and "which claims lack evidence?".

**Verdict:** weak reject from the PC member. The reader found the story clear and the two
halves (offload, sharing) well linked, but the cost argument did not land.

| Finding | Who | What we changed |
|---|---|---|
| "Whole compaction" sounds like everything; it is the per-byte work | Reader | Defined it in the introduction: per-byte work on the GPU, tree decisions in RocksDB |
| Why the offload makes sharing possible is not said | Reader | Said in the introduction and in a bridge paragraph opening the service section: a request carries its input, so the GPU never needs the database's files |
| "Pays only as a shared resource" promises savings; the result is 5% more per write | Reader | Now "can only approach the cost of cores when shared"; the 5% and the \$2.73 break-even are in the introduction |
| Cost per GiB/s of compaction and cost per write are mixed up | Reader | Both places now say which one they price, and why cheaper compaction is not cheaper writes |
| "Sustain twice the writes" overstates 5-minute runs | Both | Now "over 5-minute runs, counting the leftover compaction"; the steady-state question is E8 |
| "Mainly because compaction is deferred" is wrong: counting the leftover, the total still rises 4.90M → 6.26M | PC | Corrected with both totals |
| Earlier designs are rebuilt with our kernels, so they are not reproductions | PC | Called stage-placement rebuilds, and said what they measure |
| 2.10 CPU-s/GiB in Table 2 against 1.21 in Figure 4 | PC | Two separate archived campaigns; the caption now says to compare within the table. The cause of the gap is not documented in the archive |
| "Every compaction matches RocksDB" is stronger than the checks | PC | Now says what is checked: every single-compaction run, and every key in whole-database runs |
| "Reads never get slower" is universal | PC | "No workload we measured had slower reads" |
| "The same tree as RocksDB" (G1) | PC | "RocksDB shapes the tree": the same rules, not the same tree |
| Gain may be confounded with compacting level 0 earlier | PC | Methodology now says what the tuning searched (both keep the default triggers). The limitation names the possible confound; E7 would separate it |
| Snapshot stripe and format-6 offset modifier are jargon | Reader | Replaced with plain wording |

**Not changed, with reason:**
- *Isolation failure weakens consolidation.* It is reported as a registered miss, and the
  takeaway says the service does not isolate tenants. A fix is future work.
- *Cheaper-GPU cost argument is unvalidated.* The paper already says we did not measure one
  and gives break-even prices. E5 adds the matched-capacity CPU-service comparison.
- *Correctness coverage for snapshots, iterators, recovery.* The checks are listed in §4.5.
  The artifact's test suite covers more, and the paper should cite it once the artifact URL
  exists.


### Round 2: the PC reviewer again, after the fixes

**Verdict:** still weak reject, "materially improved". The narrowed claims, the drain-adjusted
capacity totals and the split between compaction cost and write cost are accepted.

| Finding | What we changed |
|---|---|
| The abstract and conclusion still said "the best of them" and "sustain" | Now "rebuilt stage by stage", "the stronger family" and "over 5-minute runs" |
| E7 must report the level-0 depth reached, not just the trigger | Added to E7: compare at equal mean level-0 depth |
| E8 must show pending bytes **and the service queue** stay bounded | Added to E8: queue length over time |

**Still open:**
- *Why Table 2 (2.10 CPU-s/GiB) differs from Figure 4 (1.21).* The prior-design campaign ran
  with an earlier configuration (`LSM_DISABLE_GPU_INPUT=1`, 4 read threads; PREREGISTRATION.md
  line 15). That may explain the gap, but nothing in the archive confirms it. Check the run
  configs in results/data before stating a cause.
- *Novelty against GPComp and GCLComp.* Their full texts were not available; confirm that
  neither compresses blocks on the GPU (TODO in related work).
- *Isolation, oversized jobs, wider correctness coverage, cheaper-GPU and TCP cost.* Out of
  reach of writing changes; listed as limitations.
