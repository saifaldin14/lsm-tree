#!/usr/bin/env python3
"""Decompose write amplification per design per campaign from archived results.jsonl files.

Read-only over the repository. Usage: python3 wa_decompose.py [--per-trial]
"""
import json
import os
DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data")
import statistics
import sys
from collections import defaultdict

REPO = DATA
CAMPAIGNS = [
    ("N6 nvme buffered", "malphite-nvme/followup/n6/results.jsonl"),
    ("N1 malphite tmpfs", "malphite-nvme/followup/n1/results.jsonl"),
    ("NVMe confirm (direct)", "malphite-nvme/retune/confirm/results.jsonl"),
    ("NVMe tune (direct)", "malphite-nvme/retune/tune/results.jsonl"),
    ("X8 nvme", "malphite-nvme/x8/out/results.jsonl"),
    ("H31", "malphite-nvme/h31/results.jsonl"),
    ("v3-ssd SATA", "v3/v3-ssd/results.jsonl"),
    ("wa-trigger run (zyra)", "wa-trigger/run/results.jsonl"),
    ("wa-trigger run16 (zyra)", "wa-trigger/run16/results.jsonl"),
    ("W (zyra)", "wa-trigger/w/results.jsonl"),
]

GB = 1e9


def cell_key(row):
    t = row["trial"]
    c = row["result"]["config"]
    parts = [t["design"], f"{t.get('cores')}c"]
    if t.get("cell"):
        parts.append(t["cell"])
    parts.append(f"sub{c.get('subcompactions')}")
    if c.get("l0_trigger"):
        parts.append(f"l0={c['l0_trigger']}")
    if c.get("bottommost_compression"):
        parts.append(f"bm={c['bottommost_compression']}")
    if c.get("use_direct_io"):
        parts.append("dio")
    f = t.get("flags", {})
    if "pipelined-write" in f and "tune" in row.get("_campaign", ""):
        parts.append(f"pw={int(f['pipelined-write'])},th={f.get('threads')},ay={int(f.get('adaptive-yield', 0))}")
    return " ".join(parts)


def metrics(row):
    r = row["result"]
    w = r["window_counters"]
    cj = w["compactions"]
    user = w["user_bytes_written"]
    win_s = r["window"]["seconds"]
    writes = r["throughput"]["writes_per_second"] * win_s
    tp = r.get("table_properties") or {}
    m = {}
    m["writes/s (M)"] = r["throughput"]["writes_per_second"] / 1e6
    m["WA window"] = r["write_amplification"]
    m["WA ingest+drain"] = r["ingest_and_compact"]["write_amplification"]
    m["flush/user"] = w["flush_write_bytes"] / user
    m["compact/user"] = w["compact_write_bytes"] / user
    m["compact read/user"] = w["compact_read_bytes"] / user
    m["comp in/user"] = cj["input_bytes"] / user
    m["comp out/in"] = cj["output_bytes"] / max(1, cj["input_bytes"])
    m["jobs"] = cj["jobs"]
    m["jobs per GB user"] = cj["jobs"] / (user / GB)
    m["MB in/job"] = cj["input_bytes"] / max(1, cj["jobs"]) / 1e6
    m["MB out/job"] = cj["output_bytes"] / max(1, cj["jobs"]) / 1e6
    m["in records/user write"] = cj["input_records"] / writes
    m["B/in record"] = cj["input_bytes"] / max(1, cj["input_records"])
    m["user B/write"] = user / writes
    m["flush B/write"] = w["flush_write_bytes"] / writes
    acc = r.get("accounting", {}).get("window", {})
    if acc.get("process_write_mib_s"):
        m["proc write/user"] = acc["process_write_mib_s"] / r["throughput"]["user_mib_per_second"]
    m["stall frac"] = r["stall_fraction"]
    if tp.get("num_entries"):
        n = tp["num_entries"]
        m["data B/entry"] = tp["data_size"] / n
        m["filter B/entry"] = tp["filter_size"] / n
        m["index B/entry"] = tp["index_size"] / n
        m["data B/block"] = tp["data_size"] / tp["num_data_blocks"]
        m["entries/block"] = n / tp["num_data_blocks"]
        m["raw/data ratio"] = (tp["raw_key_size"] + tp["raw_value_size"]) / tp["data_size"]
        m["entries (M)"] = n / 1e6
        m["live SST B/entry"] = r["live_sst_bytes_after_run"] / n
    m["live SST GB"] = r["live_sst_bytes_after_run"] / GB
    lv = {l["level"]: l for l in r["levels_after_run"]}
    m["L0 files end"] = lv.get(0, {}).get("files", 0)
    m["n levels end"] = len([l for l in r["levels_after_run"] if l["level"] > 0])
    m["files end"] = sum(l["files"] for l in r["levels_after_run"])
    m["MB/file end (L>0)"] = (sum(l["bytes"] for l in r["levels_after_run"] if l["level"] > 0) /
                              max(1, sum(l["files"] for l in r["levels_after_run"] if l["level"] > 0)) / 1e6)
    tl = [s for s in r["timeline"] if s["phase"] == "measurement"]
    if tl:
        m["L0 files mean"] = statistics.mean(s["l0_files"] for s in tl)
        m["L0 files p90"] = sorted(s["l0_files"] for s in tl)[int(0.9 * len(tl))]
        m["running comp mean"] = statistics.mean(s["running_compactions"] for s in tl)
        m["pending comp GB mean"] = statistics.mean(s["pending_compaction_bytes"] for s in tl) / GB
        m["imm memtables mean"] = statistics.mean(s["immutable_memtables"] for s in tl)
        m["delayed s frac"] = sum(1 for s in tl if s["delayed_write_rate"]) / len(tl)
    tr = r.get("transform") or {}
    if tr.get("merge_jobs"):
        m["gpu merge_fallbacks"] = tr.get("merge_fallbacks", 0)
        m["gpu built B/block"] = tr["merge_built_bytes"] / max(1, tr["merge_built_blocks"])
    m["drain s"] = r["ingest_and_compact"]["drain_seconds"]
    return m


def load(path):
    rows = []
    for line in open(os.path.join(REPO, path)):
        row = json.loads(line)
        if row.get("exit_code") != 0 or not row.get("result"):
            continue
        row["_campaign"] = path
        rows.append(row)
    return rows


def main():
    per_trial = "--per-trial" in sys.argv
    out = {}
    for name, path in CAMPAIGNS:
        if not os.path.exists(os.path.join(REPO, path)):
            continue
        groups = defaultdict(list)
        for row in load(path):
            groups[cell_key(row)].append(metrics(row))
        out[name] = {}
        for key, ms in sorted(groups.items()):
            med = {k: statistics.median([m[k] for m in ms if k in m]) for k in ms[0]}
            med["n"] = len(ms)
            out[name][key] = med
            if per_trial:
                for m in ms:
                    print(name, key, json.dumps({k: round(v, 3) for k, v in m.items()}))
    json.dump(out, open(os.path.join(os.path.dirname(__file__), "wa_medians.json"), "w"), indent=1)
    keys = list(next(iter(next(iter(out.values())).values())).keys())
    for name, cells in out.items():
        print(f"\n=== {name}")
        cols = list(cells)
        print("metric".ljust(24) + "".join(c[:30].rjust(32) for c in cols))
        allk = []
        for c in cols:
            for k in cells[c]:
                if k not in allk:
                    allk.append(k)
        for k in allk:
            print(k.ljust(24) + "".join((f"{cells[c][k]:.3f}" if k in cells[c] else "-").rjust(32) for c in cols))


if __name__ == "__main__":
    main()
