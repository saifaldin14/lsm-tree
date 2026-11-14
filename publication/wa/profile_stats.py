#!/usr/bin/env python3
"""Profile runs with full RocksDB statistics (zyra, in memory, 8 cores, 30 s + 60 s):
count L0->base compactions via NUM_SUBCOMPACTIONS_SCHEDULED (recorded only for compactions that
form >= 2 subcompactions, which in leveled style are L0->Lbase compactions larger than 2 target files)."""
import os
DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data")
import json
import re

D = os.path.join(DATA, "profiles/phase-c")


def tick(stats, name):
    m = re.search(re.escape(name) + r" COUNT : (\d+)", stats)
    return int(m.group(1)) if m else None


def hist(stats, name):
    m = re.search(re.escape(name) + r" P50 : ([\d.]+) P95 : ([\d.]+) P99 : ([\d.]+) P100 : ([\d.]+) COUNT : (\d+) SUM : (\d+)", stats)
    return dict(zip(["p50", "p95", "p99", "p100", "count", "sum"], map(float, m.groups()))) if m else None


for d in ["full", "native"]:
    j = json.load(open(f"{D}/{d}/bench.json"))
    r = j.get("result", j)
    s = r["rocksdb_statistics"]
    user = tick(s, "rocksdb.bytes.written")
    flush = tick(s, "rocksdb.flush.write.bytes")
    comp = tick(s, "rocksdb.compact.write.bytes")
    flushes = hist(s, "rocksdb.db.flush.micros")["count"]
    sub = hist(s, "rocksdb.num.subcompactions.scheduled")
    ctimes = hist(s, "rocksdb.compaction.times.micros")
    print(f"{d}: subcompactions={r['config']['subcompactions']} writes/s={r['throughput']['writes_per_second']/1e6:.3f}M "
          f"WA(window)={r['write_amplification']:.2f} WA(run, tickers)={(flush+comp)/user:.2f}")
    print(f"  user GB={user/1e9:.2f} flush GB={flush/1e9:.2f} compaction-write GB={comp/1e9:.2f} compact/user={comp/user:.2f}")
    print(f"  flushes={flushes:.0f} compactions(non-trivial)={ctimes['count']:.0f} "
          f"L0->Lbase compactions with >=2 subcompactions={sub['count']:.0f} (median subcompactions {sub['p50']:.1f})")
    print(f"  flushes per such L0 compaction={flushes/sub['count']:.1f}; user GB per L0 compaction={user/sub['count']/1e9:.2f}; "
          f"compaction time p50={ctimes['p50']/1e6:.2f}s p95={ctimes['p95']/1e6:.2f}s")
