"""Load and summarise whole-database campaign results (publication/data/campaigns/*.jsonl).

Each trial becomes a flat row of metrics. Rows are grouped by campaign
configuration; every design is reported at its best background-job setting
(highest median throughput), and ratios against native are paired within
repetition blocks (see PREREGISTRATION.md).
"""

from __future__ import annotations

import json
import statistics
from collections import defaultdict
from pathlib import Path

from stats import bootstrap_ci, paired_ratios

DATA = Path(__file__).resolve().parent / "data" / "campaigns"
DESIGN_NAMES = {"native": "Native RocksDB", "compression_only": "GPU compression only", "merge": "GPU merge",
                "full": "GPU merge + block building"}
# Trial fields that describe the configuration (besides flags); design-specific ones are excluded.
CONFIG_FIELDS = ("writes", "workload", "distribution", "shape", "instances", "burn_percent", "cores")
FLAG_EXCLUDE = {"background-jobs", "subcompactions"}


def design_of(trial: dict) -> str:
    if "design" in trial:
        return trial["design"]
    if trial["mode"] == "native_cpu":
        return "native"
    if trial["mode"] == "gpu_output":
        return "compression_only"
    return "merge" if trial.get("env", {}).get("LSM_DISABLE_GPU_BUILD") else "full"


def metrics(record: dict) -> dict:
    """Flat metrics of one successful trial (aggregated over instances when several ran)."""
    results = [record["result"]] if "result" in record else record["results"]
    energy = record.get("energy_joules_end_to_end", {})
    joules = sum(energy.get(key, 0.0) for key in ("power/energy-pkg/", "power/energy-ram/", "gpu"))
    ops = sum(r["ingest_and_compact"]["ops"] for r in results)
    cpu = sum(r["ingest_and_compact"]["cpu_seconds"] for r in results)
    compaction_cpu = sum(r["ingest_and_compact"]["compaction_cpu_seconds"] for r in results)
    reads = [r["read_latency"]["p99_ns"] / 1e6 for r in results if r["read_latency"]["count"]]
    writes = [r["write_latency"]["p99_ns"] / 1e6 for r in results if r["write_latency"]["count"]]
    transform = [r.get("transform") or {} for r in results]
    return {
        "ops_per_second": sum(r["ingest_and_compact"]["ops_per_second"] for r in results),
        "window_ops_per_second": sum(r["throughput"]["ops_per_second"] for r in results),
        "p99_write_ms": max(writes) if writes else None,
        "p99_read_ms": max(reads) if reads else None,
        "cpu_us_per_op": cpu * 1e6 / ops if ops else None,
        "compaction_cpu_share": compaction_cpu / cpu if cpu else None,
        "energy_uj_per_op": joules * 1e6 / ops if ops and joules else None,
        "host_energy_uj_per_op": (energy.get("power/energy-pkg/", 0) + energy.get("power/energy-ram/", 0)) * 1e6 / ops
        if ops and energy else None,
        "gpu_energy_uj_per_op": energy.get("gpu", 0) * 1e6 / ops if ops and energy else None,
        "drain_seconds": max(r["ingest_and_compact"]["drain_seconds"] for r in results),
        "stall_percent": 100 * statistics.mean(r["stall_fraction"] for r in results),
        "write_amplification": statistics.mean(r["ingest_and_compact"]["write_amplification"] for r in results),
        "sst_bytes": sum(r.get("sst_bytes_after_compaction") or r.get("sst_bytes_after_run") or 0 for r in results),
        "gpu_fallbacks": sum(t.get("merge_fallbacks") or 0 for t in transform),
        "gpu_jobs": sum(t.get("merge_jobs") or 0 for t in transform),
        "foreign_cpu_percent": max(record.get("foreign_load", {}).get(when, {}).get("other_users_cpu_percent", 0)
                                   for when in ("before", "after")),
    }


def load(campaign: str) -> list[dict]:
    rows = []
    path = DATA / f"{campaign}.jsonl"
    if not path.exists():
        return rows
    for line in path.read_text().splitlines():
        record = json.loads(line)
        trial = record["trial"]
        flags = record["flags"]
        row = {"campaign": campaign, "block": trial.get("block", 0), "design": design_of(trial),
               "background_jobs": flags.get("background-jobs"), "ok": record["exit_code"] == 0,
               "config": tuple((field, trial.get(field, 1 if field == "instances" else 0)) for field in CONFIG_FIELDS) +
               tuple(sorted((k, str(v)) for k, v in flags.items() if k not in FLAG_EXCLUDE and k not in
                            ("threads",)))}
        if row["ok"]:
            row.update(metrics(record))
        rows.append(row)
    return rows


def describe_config(config: tuple) -> dict:
    return dict(config)


def summarise(rows: list[dict], metric: str = "ops_per_second") -> list[dict]:
    """One entry per (config, design) at the design's best background-job setting,
    with bootstrap CIs for every metric and paired ratios against native."""
    valid = [r for r in rows if r["ok"]]
    groups = defaultdict(list)
    for row in valid:
        groups[(row["config"], row["design"], row["background_jobs"])].append(row)
    best = {}
    for (config, design, jobs), members in groups.items():
        median = statistics.median(m[metric] for m in members)
        if (config, design) not in best or median > best[(config, design)][0]:
            best[(config, design)] = (median, jobs, members)
    entries = []
    for (config, design), (_, jobs, members) in sorted(best.items(), key=lambda item: (str(item[0][0]), item[0][1])):
        entry = {"config": describe_config(config), "design": design, "background_jobs": jobs, "runs": len(members)}
        for key in ("ops_per_second", "window_ops_per_second", "p99_write_ms", "p99_read_ms", "cpu_us_per_op",
                    "energy_uj_per_op", "host_energy_uj_per_op", "gpu_energy_uj_per_op", "drain_seconds",
                    "stall_percent", "write_amplification", "compaction_cpu_share", "sst_bytes", "gpu_fallbacks"):
            values = [m[key] for m in members if m.get(key) is not None]
            entry[key] = bootstrap_ci(values) if values else None
        native = best.get((config, "native"))
        if native and design != "native":
            for key in ("ops_per_second", "cpu_us_per_op", "p99_write_ms", "p99_read_ms", "energy_uj_per_op"):
                treatment = {m["block"]: m[key] for m in members if m.get(key) is not None}
                baseline = {m["block"]: m[key] for m in native[2] if m.get(key) is not None}
                ratios = paired_ratios(treatment, baseline)
                entry[f"ratio_{key}"] = bootstrap_ci(ratios) if ratios else None
        entries.append(entry)
    return entries


def fmt(value, digits=0, scale=1.0):
    if value is None:
        return "–"
    point, low, high = value
    return f"{point * scale:,.{digits}f} [{low * scale:,.{digits}f}, {high * scale:,.{digits}f}]"
