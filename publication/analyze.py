#!/usr/bin/env python3
"""Independent re-analysis of the CUDA compaction experiments.

`extract` reads raw Zyra result bundles and writes compact per-trial and
per-job CSVs. `report` reads only those CSVs and produces every table, figure,
and statistic used in publication/RESULTS.md, so the published numbers can be
regenerated without the multi-gigabyte raw request logs.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
from collections import defaultdict
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
DATA = HERE / "data"
FIGURES = HERE / "figures"
TABLES = HERE / "tables"

CAMPAIGNS = {
    "stress": ["gpu-tail-final"],
    "stress-confirm": ["gpu-tail-5k-confirm/run"],
    "mixed": ["gpu-realistic-confirm/*/run"],
    "read-capacity": ["gpu-realistic-read-capacity/run"],
    "mixed-screen": ["gpu-realistic-screen-64b/*/run"],
}

WORKLOADS = {0: "write-only", 10: "write-heavy", 25: "update-heavy", 50: "balanced", 90: "read-dominant"}
WORKLOAD_ORDER = ["write-only", "write-heavy", "update-heavy", "balanced", "read-dominant"]
MODES = ("native_cpu", "cuda_local")
MS = 1e-3  # microseconds -> milliseconds
# Native jobs never fall below ~62 ms; GPU jobs cluster near 51 ms. 60 ms separates
# a GPU job that realized the offload benefit from one that ran at CPU speed.
SLOW_JOB_MS = 60.0
# The first jobs of each trial include engine/CUDA warm-up and run during load warmup.
WARMUP_JOBS = 5


# --------------------------------------------------------------------------- extract


def _jsonl(path: Path) -> list[dict]:
    with path.open() as handle:
        return [json.loads(line) for line in handle if line.strip()]


def _series(samples: list[dict], select) -> tuple[np.ndarray, np.ndarray] | None:
    points = []
    for sample in samples:
        if sample.get("type") != "sample":
            continue
        value = select(sample["metrics"])
        if value is not None:
            points.append((sample["sample_end_ns"], value))
    if len(points) < 2:
        return None
    points.sort()
    return np.array([p[0] for p in points], dtype=float), np.array([p[1] for p in points], dtype=float)


def _delta_in_window(series, start_ns: int, end_ns: int) -> float | None:
    """Counter delta across the window, linearly interpolated between 1 Hz samples."""
    if series is None:
        return None
    times, values = series
    if times[0] > start_ns or times[-1] < end_ns:
        return None
    return float(np.interp(end_ns, times, values) - np.interp(start_ns, times, values))


def _process_cpu(metrics: list[dict]) -> float | None:
    values = {m["name"]: m.get("value") for m in metrics if m.get("scope") == "process"}
    if values.get("cpu_user_ns") is None or values.get("cpu_system_ns") is None:
        return None
    return values["cpu_user_ns"] + values["cpu_system_ns"]


def _gpu_energy(metrics: list[dict]) -> float | None:
    for metric in metrics:
        if (
            metric.get("scope") == "host_gpu"
            and metric.get("name") == "energy_joules"
            and metric.get("status", "ok") == "ok"
            and metric.get("value") is not None
        ):
            return float(metric["value"])
    return None


def _window_compaction(snapshots: list[dict], start_s: float, end_s: float) -> dict:
    """Compaction work between the 1 Hz snapshots at or before the window edges.

    Snapshots are not aligned to the window and stop before its end, so this is
    an ~8 s interval that starts ~0.25 s before the measurement window. Its
    bounds (seconds after load start) are returned so that other counters can
    be differenced over exactly the same interval.
    """
    timed = sorted(
        ((s["load_elapsed_ns"] / 1e9, s["statistics"]["compactions"]) for s in snapshots if "statistics" in s),
        key=lambda item: item[0],
    )
    before = [item for item in timed if item[0] <= start_s]
    inside = [item for item in timed if item[0] <= end_s]
    if not before or not inside:
        return {}
    (t0, c0), (t1, c1) = before[-1], inside[-1]
    span = t1 - t0
    if span <= 0:
        return {}
    busy_ms = (c1["elapsed_micros_total"] - c0["elapsed_micros_total"]) * MS
    return {
        "win_start_s": t0,
        "win_end_s": t1,
        "win_span_s": span,
        "win_jobs": c1["successful_jobs"] - c0["successful_jobs"],
        "win_input_bytes": c1["input_bytes"] - c0["input_bytes"],
        "win_compaction_ms": busy_ms,
        "win_compaction_cpu_ms": (c1["cpu_micros_total"] - c0["cpu_micros_total"]) * MS,
        "win_compaction_share": busy_ms / (span * 1e3),
    }


def _hist_ms(result: dict, name: str, quantile: str) -> float | None:
    value = result.get(name, {}).get(quantile)
    return None if value is None else value / 1e6


def _rapl_series(campaign_dir: Path, campaign: dict) -> dict:
    """Cumulative CPU package and DRAM energy from `perf stat -I` on CLOCK_MONOTONIC."""
    start = campaign["rapl"]["start_monotonic_ns"]
    points: dict[str, list[tuple[float, float]]] = defaultdict(list)
    for line in (campaign_dir / campaign["rapl"]["file"]).read_text().splitlines():
        fields = line.strip().split(",")
        if len(fields) < 4 or line.startswith("#"):
            continue
        try:
            offset_s, joules = float(fields[0]), float(fields[1])
        except ValueError:
            continue
        event = "pkg" if "energy-pkg" in fields[3] else "dram" if "energy-ram" in fields[3] else None
        if event:
            points[event].append((start + offset_s * 1e9, joules))
    series = {}
    for event, values in points.items():
        values.sort()
        times = np.array([start] + [t for t, _ in values], dtype=float)
        cumulative = np.concatenate([[0.0], np.cumsum([j for _, j in values])])
        series[event] = (times, cumulative)
    return series


def _trial_rows(campaign: str, bundle: Path, raw: Path, extra: dict | None = None,
                rapl: dict | None = None) -> tuple[list[dict], list[dict]]:
    run = json.loads((bundle / "run.json").read_text())
    plan = json.loads((bundle / "plan.json").read_text())
    build = json.loads((bundle / "build.json").read_text())
    config = plan["config"]
    read_percent = int(config["read_percent"])
    order = {trial["id"]: index for index, trial in enumerate(plan["trials"])}

    pair_first: dict[tuple, str] = {}
    for trial in plan["trials"]:
        pair_first.setdefault((trial["rate"], trial["repetition"]), trial["mode"])

    rows, jobs = [], []
    for record in run["trials"]:
        directory = bundle / "trials" / record["id"]
        summary = json.loads((directory / "primary-summary.json").read_text())
        result = json.loads((directory / "load" / "result.json").read_text())
        compactions = summary["compactions"]
        transform = summary.get("local_transform") or {}
        measurement = result["measurement"]
        window = result["resource_window"]
        warmup_s = result["warmup_ns"] / 1e9
        end_s = warmup_s + result["offered_window_ns"] / 1e9

        job_list = compactions.get("jobs", [])
        if len(job_list) != compactions["successful_jobs"]:
            raise SystemExit(f"{directory}: per-job samples were dropped")
        elapsed = np.array([j["elapsed_micros"] for j in job_list], dtype=float) * MS
        cpu = np.array([j["cpu_micros"] for j in job_list], dtype=float) * MS
        input_gib = compactions["input_bytes"] / 2**30

        win_start, win_end = window["measurement_start_ns"], window["measurement_end_ns"]
        cpu_series = _series(_jsonl(directory / "primary-resources.jsonl"), _process_cpu)
        energy_series = _series(_jsonl(directory / "host-resources.jsonl"), _gpu_energy)
        win_cpu_ns = _delta_in_window(cpu_series, win_start, win_end)
        gpu_joules = _delta_in_window(energy_series, win_start, win_end)
        snapshots = _jsonl(directory / "load" / "primary-metrics.jsonl")
        verification = record.get("verification") or {}

        offered = measurement["offered"]
        row = {
            "campaign": campaign,
            "bundle": str(bundle.relative_to(raw)).removesuffix("/run"),
            "experiment": config["name"],
            "workload": WORKLOADS[read_percent],
            "read_percent": read_percent,
            "rate": record["rate"],
            "write_rate": round(record["rate"] * (100 - read_percent) / 100),
            "repetition": record["repetition"],
            "mode": record["mode"],
            "trial": record["id"],
            "plan_order": order[record["id"]],
            "ran_first_in_pair": pair_first[(record["rate"], record["repetition"])] == record["mode"],
            "lsm_db_sha256": build["binaries"]["lsm_db"],
            "lsm_worker_sha256": build["binaries"]["lsm_worker"],
            "valid": bool(record.get("valid")) and result.get("valid") is True,
            "verification_ok": verification.get("status") == "ok",
            "full_payloads_verified": bool(verification.get("full_payloads_verified")),
            "native_checksums_verified": bool(verification.get("native_checksums_verified")),
            "offered": offered,
            "completed": measurement["completed"],
            "completed_in_window_frac": measurement["completed_in_window"] / offered,
            "failed": measurement["failed"],
            "queue_rejected": measurement["queue_rejected"],
            "expired_before_send": measurement["expired_before_send"],
            "payload_mismatches": measurement["payload_mismatches"] + result["warmup"]["payload_mismatches"],
            "jobs": len(job_list),
            "failed_jobs": compactions["failed_jobs"],
            "input_bytes": compactions["input_bytes"],
            "output_bytes": compactions["output_bytes"],
            "compaction_total_ms": float(elapsed.sum()),
            "compaction_mean_ms": float(elapsed.mean()),
            "compaction_median_ms": float(np.median(elapsed)),
            "compaction_p95_ms": float(np.percentile(elapsed, 95)),
            "compaction_max_ms": float(elapsed.max()),
            "compaction_steady_median_ms": float(np.median(elapsed[WARMUP_JOBS:])),
            "slow_job_frac": float((elapsed > SLOW_JOB_MS).mean()),
            "slow_job_frac_steady": float((elapsed[WARMUP_JOBS:] > SLOW_JOB_MS).mean()),
            "compaction_reported_p50_ms": compactions["elapsed_micros_p50"] * MS,
            "compaction_reported_p99_ms": compactions["elapsed_micros_p99"] * MS,
            "compaction_cpu_ms": float(cpu.sum()),
            "compaction_ms_per_gib": float(elapsed.sum()) / input_gib,
            "compaction_cpu_ms_per_gib": float(cpu.sum()) / input_gib,
            "compaction_throughput_mib_s": compactions["input_bytes"] / 2**20 / (elapsed.sum() / 1e3),
            "process_cpu_s": (summary["process"]["user_cpu_us"] + summary["process"]["system_cpu_us"]) / 1e6,
            "process_cpu_window_s": None if win_cpu_ns is None else win_cpu_ns / 1e9,
            "load_cpu_s": (result["load_process"]["user_cpu_us"] + result["load_process"]["system_cpu_us"]) / 1e6,
            "gpu_energy_window_j": gpu_joules,
            "gpu_power_window_w": None if gpu_joules is None else gpu_joules / ((win_end - win_start) / 1e9),
            "gpu_output_batches": transform.get("output_batches"),
            "gpu_output_gpu_batches": transform.get("output_gpu_batches"),
            "gpu_cpu_fallback_batches": transform.get("cpu_fallback_batches"),
            "gpu_output_blocks": transform.get("output_blocks"),
            "gpu_kernel_ms": None if transform.get("kernel_ns") is None else transform["kernel_ns"] / 1e6,
            "pending_compaction_bytes_end": summary["properties"]["rocksdb.estimate-pending-compaction-bytes"],
            "l0_files_end": summary["properties"]["rocksdb.num-files-at-level0"],
            "live_sst_bytes_end": summary["properties"]["rocksdb.live-sst-files-size"],
            "write_p50_ms": _hist_ms(result, "write_latency", "p50"),
            "write_p99_ms": _hist_ms(result, "write_latency", "p99"),
            "read_p50_ms": _hist_ms(result, "read_latency", "p50"),
            "read_p99_ms": _hist_ms(result, "read_latency", "p99"),
            "queue_p99_ms": _hist_ms(result, "queue_delay", "p99"),
            "rpc_p99_ms": _hist_ms(result, "rpc_latency", "p99"),
            "gpu_device": transform.get("device"),
            **(extra or {}),
        }
        if rapl:
            row["cpu_pkg_window_j"] = _delta_in_window(rapl.get("pkg"), win_start, win_end)
            row["dram_window_j"] = _delta_in_window(rapl.get("dram"), win_start, win_end)
        compaction_window = _window_compaction(snapshots, warmup_s, end_s)
        if compaction_window:
            # Same steady clock: snapshot offsets are relative to load start.
            snap_start = window["load_start_ns"] + round(compaction_window["win_start_s"] * 1e9)
            snap_end = window["load_start_ns"] + round(compaction_window["win_end_s"] * 1e9)
            compaction_window["win_gpu_energy_j"] = _delta_in_window(energy_series, snap_start, snap_end)
            snap_cpu_ns = _delta_in_window(cpu_series, snap_start, snap_end)
            compaction_window["win_process_cpu_s"] = None if snap_cpu_ns is None else snap_cpu_ns / 1e9
        row.update(compaction_window)
        rows.append(row)
        for index, job in enumerate(job_list):
            jobs.append(
                {
                    "campaign": campaign,
                    "workload": row["workload"],
                    "rate": row["rate"],
                    "repetition": row["repetition"],
                    "mode": row["mode"],
                    "trial": f"{row['bundle']}/{row['trial']}",
                    "condition": row.get("condition"),
                    "job_index": index,
                    "elapsed_ms": job["elapsed_micros"] * MS,
                    "cpu_ms": job["cpu_micros"] * MS,
                    "input_bytes": job["input_bytes"],
                    "output_bytes": job["output_bytes"],
                    "input_files": job["input_files"],
                    "input_records": job["input_records"],
                    "output_records": job["output_records"],
                }
            )
    return rows, jobs


def _write_csv(path: Path, rows: list[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fields: list[str] = []
    for row in rows:
        fields.extend(key for key in row if key not in fields)
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


THREAD_GROUPS = {"rocksdb:low": "compaction", "rocksdb:high": "flush", "grpcpp_sync_ser": "rpc",
                 "event_engine": "rpc", "grpc_global_tim": "rpc", "lsm_db": "main", "cuda-EvtHandlr": "cuda"}


def extract_profiles(profiles: list[Path]) -> None:
    """Flatten perf attribution summaries (publication/profile.sh) into one CSV."""
    rows = []
    for directory in profiles:
        config = json.loads((directory / "config.json").read_text())
        summary = json.loads((directory / "summary.json").read_text())
        run = json.loads((directory / "run" / "run.json").read_text())
        build = json.loads((directory / "run" / "build.json").read_text())
        env = build.get("lsm_environment", {})
        for line in (directory / "trials.txt").read_text().split("\n"):
            if not line.strip():
                continue
            trial, mode, pid = line.split()
            entry = summary[pid.split("=")[1]]
            jobs = json.loads((directory / "run" / "trials" / trial / "primary-summary.json").read_text())["compactions"]
            base = {"profile": config["name"], "sync": config["name"].split("-")[1], "mode": mode,
                    "gpu_input": env.get("LSM_DISABLE_GPU_INPUT") != "1",
                    "process_cpu_s": entry["cpu_seconds"], "compaction_jobs": jobs["successful_jobs"],
                    "rocksdb_reported_compaction_cpu_s": jobs["cpu_micros_total"] / 1e6,
                    "compaction_thread_ids": entry.get("distinct_thread_ids", {}).get("rocksdb:low")}
            for thread, categories in entry["thread_categories_cpu_s"].items():
                for category, seconds in categories.items():
                    rows.append({**base, "thread": thread, "thread_group": THREAD_GROUPS.get(thread, "other"),
                                 "category": category, "cpu_s": seconds})
            assert run["trials"]
    _write_csv(DATA / "profile-attribution.csv", rows)
    print(f"wrote {len(rows)} attribution rows to {DATA / 'profile-attribution.csv'}")


def extract(raw: Path, revisions: list[Path] | None = None) -> None:
    all_rows, all_jobs = [], []
    for campaign, patterns in CAMPAIGNS.items():
        bundles = sorted({path for pattern in patterns for path in raw.glob(pattern)})
        if not bundles:
            raise SystemExit(f"missing raw bundle for {campaign}: {patterns}")
        for bundle in bundles:
            rows, jobs = _trial_rows(campaign, bundle, raw)
            all_rows.extend(rows)
            all_jobs.extend(jobs)
    for revision in revisions or []:
        campaign = json.loads((revision / "campaign.json").read_text())
        label = campaign.get("label", "revision")
        rapl = _rapl_series(revision, campaign)
        for run in campaign["runs"]:
            bundle = revision / "runs" / run["id"]
            if not (bundle / "run.json").exists():
                continue
            sync, node, jobs_setting = campaign["conditions"][run["condition"]]
            extra = {"condition": run["condition"], "block": run["block"], "cuda_sync_setting": sync,
                     "primary_numa_node": node, "background_jobs": jobs_setting, "cell": run["workload"]}
            rows, jobs = _trial_rows(label, bundle, revision.parent, extra, rapl)
            all_rows.extend(rows)
            all_jobs.extend(jobs)
    _write_csv(DATA / "trials.csv", all_rows)
    _write_csv(DATA / "jobs.csv", all_jobs)
    print(f"wrote {len(all_rows)} trials and {len(all_jobs)} compaction jobs to {DATA}")


# --------------------------------------------------------------------------- statistics

# Two-sided 95% Student t critical values for df = 1..30.
_T975 = [12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365, 2.306, 2.262, 2.228, 2.201, 2.179, 2.160, 2.145,
         2.131, 2.120, 2.110, 2.101, 2.093, 2.086, 2.080, 2.074, 2.069, 2.064, 2.060, 2.056, 2.052, 2.048,
         2.045, 2.042]


def t975(df: int) -> float:
    if df <= 30:
        return _T975[df - 1]
    # Cornish-Fisher expansion of the Student-t quantile; error < 1e-5 for df > 30.
    z = 1.959963984540054
    return (z + (z**3 + z) / (4 * df) + (5 * z**5 + 16 * z**3 + 3 * z) / (96 * df**2)
            + (3 * z**7 + 19 * z**5 + 17 * z**3 - 15 * z) / (384 * df**3))


def geomean_ci(ratios: list[float]) -> tuple[float, float, float]:
    logs = np.log(np.asarray(ratios, dtype=float))
    centre = logs.mean()
    if len(logs) < 2:
        return math.exp(centre), math.nan, math.nan
    half = t975(len(logs) - 1) * logs.std(ddof=1) / math.sqrt(len(logs))
    return math.exp(centre), math.exp(centre - half), math.exp(centre + half)


def point_level(groups: list[list[float]]) -> dict:
    """Each operating point contributes one mean log ratio (summary-statistics approach).

    With only 3-6 pairs per point, per-point variances are too unstable for
    inverse-variance weighting, so points are weighted equally; the interval
    and tests then reflect between-point variation, not pair counts.
    """
    means = [float(np.mean(np.log(np.asarray(g, dtype=float)))) for g in groups if len(g) > 0]
    ratios = [math.exp(m) for m in means]
    gm, lo, hi = geomean_ci(ratios)
    return {"points": len(means), "ratio": gm, "ci_low": lo, "ci_high": hi,
            "points_gpu_better": sum(m < 0 for m in means), "wilcoxon_p": wilcoxon_exact(ratios),
            "sign_p": sign_test(ratios)}


def holm(p_values: list[float]) -> list[float]:
    """Holm-Bonferroni step-down adjusted p-values (same order as input)."""
    order = sorted(range(len(p_values)), key=lambda i: p_values[i])
    adjusted = [0.0] * len(p_values)
    running = 0.0
    for rank, index in enumerate(order):
        running = max(running, min(1.0, (len(p_values) - rank) * p_values[index]))
        adjusted[index] = running
    return adjusted


def sign_test(ratios: list[float]) -> float:
    """Exact two-sided sign test of ratio == 1 (ties dropped)."""
    above = sum(r > 1 for r in ratios)
    below = sum(r < 1 for r in ratios)
    n = above + below
    if n == 0:
        return 1.0
    tail = sum(math.comb(n, i) for i in range(min(above, below) + 1)) / 2**n
    return min(1.0, 2 * tail)


def wilcoxon_exact(ratios: list[float]) -> float:
    """Exact two-sided Wilcoxon signed-rank p-value on log ratios."""
    d = np.log(np.asarray(ratios, dtype=float))
    d = d[d != 0]
    n = len(d)
    if n == 0:
        return 1.0
    ranks = np.argsort(np.argsort(np.abs(d))) + 1
    w_plus = int(ranks[d > 0].sum())
    total = n * (n + 1) // 2
    counts = [0] * (total + 1)
    counts[0] = 1
    for rank in range(1, n + 1):
        for s in range(total, rank - 1, -1):
            counts[s] += counts[s - rank]
    low = sum(counts[: min(w_plus, total - w_plus) + 1]) / 2**n
    return float(min(1.0, 2 * low))


# --------------------------------------------------------------------------- report helpers


def _read_csv(path: Path) -> list[dict]:
    with path.open() as handle:
        rows = list(csv.DictReader(handle))
    for row in rows:
        for key, value in row.items():
            if value in ("", "None"):
                row[key] = None
            elif value in ("True", "False"):
                row[key] = value == "True"
            else:
                try:
                    row[key] = int(value)
                except ValueError:
                    try:
                        row[key] = float(value)
                    except ValueError:
                        pass
    return rows


def pairs(rows: list[dict], campaign: str) -> list[tuple[dict, dict]]:
    index = {}
    for row in rows:
        if row["campaign"] == campaign:
            index[(row["bundle"], row["workload"], row["rate"], row["repetition"], row["mode"])] = row
    out = []
    for key, native in sorted(index.items(), key=lambda kv: kv[0]):
        if key[-1] != "native_cpu":
            continue
        gpu = index.get(key[:-1] + ("cuda_local",))
        if gpu is not None:
            out.append((native, gpu))
    return out


def _pct(ratio: float) -> str:
    return f"{(ratio - 1) * 100:+.1f}%"


def _fmt_p(p: float) -> str:
    return "<0.001" if p < 0.001 else f"{p:.3f}"


def summarize(pair_list: list[tuple[dict, dict]], metric: str) -> dict:
    ratios = [g[metric] / n[metric] for n, g in pair_list if n[metric] and g[metric] is not None]
    gm, lo, hi = geomean_ci(ratios)
    return {
        "metric": metric,
        "n_pairs": len(ratios),
        "gpu_better": sum(r < 1 for r in ratios),
        "geomean_ratio": gm,
        "ci_low": lo,
        "ci_high": hi,
        "median_ratio": float(np.median(ratios)),
        "min_ratio": min(ratios),
        "max_ratio": max(ratios),
        "sign_p": sign_test(ratios),
        "wilcoxon_p": wilcoxon_exact(ratios),
    }


def by_point(pair_list: list[tuple[dict, dict]]) -> dict[tuple, list[tuple[dict, dict]]]:
    grouped: dict[tuple, list] = defaultdict(list)
    for native, gpu in pair_list:
        grouped[(native["workload"], native["rate"])].append((native, gpu))
    return dict(sorted(grouped.items(), key=lambda kv: (WORKLOAD_ORDER.index(kv[0][0]), kv[0][1])))


def mean(values) -> float:
    values = [v for v in values if v is not None]
    return float(np.mean(values)) if values else math.nan


def median(values) -> float:
    values = [v for v in values if v is not None]
    return float(np.median(values)) if values else math.nan


# --------------------------------------------------------------------------- report


HEADLINE_METRICS = [
    ("compaction_median_ms", "Median compaction-job duration"),
    ("compaction_mean_ms", "Mean compaction-job duration"),
    ("compaction_ms_per_gib", "Compaction wall time per GiB"),
    ("compaction_cpu_ms_per_gib", "Compaction-thread CPU per GiB"),
    ("compaction_max_ms", "Slowest compaction job"),
    ("process_cpu_s", "Whole-process CPU (lsm_db lifetime)"),
    ("process_cpu_window_s", "Whole-process CPU (measurement window)"),
]

FOREGROUND_METRICS = [
    ("write_p50_ms", "Write p50"),
    ("write_p99_ms", "Write p99"),
    ("read_p50_ms", "Read p50"),
    ("read_p99_ms", "Read p99"),
]


def report() -> dict:
    rows = _read_csv(DATA / "trials.csv")
    jobs = _read_csv(DATA / "jobs.csv")
    for directory in (TABLES, FIGURES):
        directory.mkdir(parents=True, exist_ok=True)
        for stale in directory.iterdir():
            if stale.is_file():
                stale.unlink()

    stress = pairs(rows, "stress") + pairs(rows, "stress-confirm")
    mixed = pairs(rows, "mixed")
    stats: dict = {
        "integrity": integrity(rows),
        "stress": {m: summarize(stress, m) for m, _ in HEADLINE_METRICS + FOREGROUND_METRICS[:2]},
        "mixed": {m: summarize(mixed, m) for m, _ in HEADLINE_METRICS + FOREGROUND_METRICS},
        "work_equivalence": work_equivalence(stress + mixed),
        "order_effect": order_effect(rows),
        "gpu_energy": gpu_energy(stress + mixed),
        "cpu_accounting": cpu_accounting(stress + mixed),
        "window_share": window_share(stress),
        "reported_vs_recomputed": reported_vs_recomputed(rows),
        "points": point_table(stress + mixed),
        "job_level": job_level(jobs),
        "capacity": capacity(rows),
        "regime": regime(rows, stress + mixed),
        "pooled": pooled(stress, mixed),
        "sensitivity": sensitivity(jobs, stress + mixed),
        "revision": revision(rows, jobs),
        "revision_gpu_input": revision(rows, jobs, "revision-gpu-input"),
        "scale": scale(rows),
        "profiles": profile_attribution(),
    }

    _apply_holm(stats)
    (TABLES / "stats.json").write_text(json.dumps(stats, indent=2) + "\n")
    write_tables(stats)
    figures(rows, jobs, stats)
    print(f"wrote tables to {TABLES} and figures to {FIGURES}")
    return stats


def integrity(rows: list[dict]) -> dict:
    out = {}
    for campaign in [*CAMPAIGNS, "revision", "revision-gpu-input", "scale"]:
        subset = [r for r in rows if r["campaign"] == campaign]
        if not subset:
            continue
        gpu = [r for r in subset if r["mode"] == "cuda_local"]
        out[campaign] = {
            "trials": len(subset),
            "valid": sum(r["valid"] for r in subset),
            "verification_ok": sum(r["verification_ok"] for r in subset),
            "full_payloads_verified": sum(r["full_payloads_verified"] for r in subset),
            "native_checksums_verified": sum(r["native_checksums_verified"] for r in subset),
            "failed_requests": sum(r["failed"] for r in subset),
            "payload_mismatches": sum(r["payload_mismatches"] for r in subset),
            "failed_compaction_jobs": sum(r["failed_jobs"] for r in subset),
            "trials_with_pending_compaction_at_end": sum(bool(r["pending_compaction_bytes_end"]) for r in subset),
            "gpu_cpu_fallback_batches": sum(r["gpu_cpu_fallback_batches"] or 0 for r in gpu),
            "gpu_output_batches": sum(r["gpu_output_batches"] or 0 for r in gpu),
            "gpu_output_batches_on_gpu": sum(r["gpu_output_gpu_batches"] or 0 for r in gpu),
            "min_completed_in_window_frac": min(r["completed_in_window_frac"] for r in subset),
            "lsm_db_binaries": sorted({r["lsm_db_sha256"][:12] for r in subset}),
            "lsm_worker_binaries": sorted({r["lsm_worker_sha256"][:12] for r in subset}),
        }
    return out


def work_equivalence(pair_list) -> dict:
    """GPU and CPU must compact the same data for per-trial totals to be comparable."""
    out = {}
    for key in ("jobs", "input_bytes", "output_bytes"):
        ratios = [g[key] / n[key] for n, g in pair_list]
        out[f"{key}_ratio_min"] = min(ratios)
        out[f"{key}_ratio_max"] = max(ratios)
    return out


def order_effect(rows: list[dict]) -> dict:
    out = {}
    for campaign in ("stress", "stress-confirm", "mixed"):
        campaign_pairs = pairs(rows, campaign)
        out[campaign] = {
            "gpu_ran_first": sum(1 for _, g in campaign_pairs if g["ran_first_in_pair"]),
            "native_ran_first": sum(1 for n, _ in campaign_pairs if n["ran_first_in_pair"]),
        }
    mixed = pairs(rows, "mixed")
    for label, chooser in (("gpu_first", lambda n, g: g["ran_first_in_pair"]),
                           ("native_first", lambda n, g: n["ran_first_in_pair"])):
        subset = [(n, g) for n, g in mixed if chooser(n, g)]
        if subset:
            out[f"mixed_median_job_ratio_when_{label}"] = summarize(subset, "compaction_median_ms")["geomean_ratio"]
            out[f"mixed_pairs_{label}"] = len(subset)
    return out


def gpu_energy(pair_list) -> dict:
    deltas, gpu_w, cpu_w, per_gib = [], [], [], []
    for native, gpu in pair_list:
        if native["gpu_energy_window_j"] is None or gpu["gpu_energy_window_j"] is None:
            continue
        delta = gpu["gpu_energy_window_j"] - native["gpu_energy_window_j"]
        deltas.append(delta)
        gpu_w.append(gpu["gpu_power_window_w"])
        cpu_w.append(native["gpu_power_window_w"])
        if gpu.get("win_input_bytes") and gpu.get("win_gpu_energy_j") is not None and native.get("win_gpu_energy_j") is not None:
            # Energy and bytes over the identical snapshot interval of the GPU trial;
            # the native trial's same-length interval supplies the idle baseline.
            matched = gpu["win_gpu_energy_j"] - native["win_gpu_energy_j"] * gpu["win_span_s"] / native["win_span_s"]
            per_gib.append(matched / (gpu["win_input_bytes"] / 2**30))
    return {
        "pairs": len(deltas),
        "gpu_mode_mean_board_power_w": mean(gpu_w),
        "native_mode_mean_board_power_w": mean(cpu_w),
        "incremental_gpu_energy_j_per_window_mean": mean(deltas),
        "incremental_gpu_energy_j_per_gib_median": median(per_gib),
    }


def cpu_accounting(pair_list) -> dict:
    """Where offloaded CPU goes, over each trial's snapshot-aligned ~8 s interval."""
    thread, process = [], []
    for native, gpu in pair_list:
        if None in (native.get("win_process_cpu_s"), gpu.get("win_process_cpu_s")):
            continue
        scale = gpu["win_span_s"] / native["win_span_s"]
        thread.append((gpu["win_compaction_cpu_ms"] - native["win_compaction_cpu_ms"] * scale) / 1e3)
        process.append(gpu["win_process_cpu_s"] - native["win_process_cpu_s"] * scale)
    other = [p - t for p, t in zip(process, thread)]
    return {
        "pairs": len(thread),
        "compaction_thread_cpu_delta_s_median": median(thread),
        "process_cpu_delta_s_median": median(process),
        "other_threads_cpu_delta_s_median": median(other),
        "pairs_process_cpu_higher_on_gpu": sum(p > 0 for p in process),
    }


def window_share(pair_list) -> dict:
    out = {}
    for (workload, rate), group in by_point(pair_list).items():
        out[f"{workload}@{rate}"] = {
            "native_lifetime_total_over_8s": mean(n["compaction_total_ms"] / 8000 for n, _ in group),
            "native_in_window_share": mean(n.get("win_compaction_share") for n, _ in group),
            "gpu_in_window_share": mean(g.get("win_compaction_share") for _, g in group),
        }
    return out


def reported_vs_recomputed(rows: list[dict]) -> dict:
    return {
        "trials": len(rows),
        "reported_p99_equals_max": sum(abs(r["compaction_reported_p99_ms"] - r["compaction_max_ms"]) < 1e-9 for r in rows),
        "jobs_per_trial_min": min(r["jobs"] for r in rows),
        "jobs_per_trial_max": max(r["jobs"] for r in rows),
    }


def point_table(pair_list) -> list[dict]:
    table = []
    for (workload, rate), group in by_point(pair_list).items():
        entry = {
            "workload": workload,
            "rate": rate,
            "write_rate": group[0][0]["write_rate"],
            "campaign": "+".join(sorted({n["campaign"] for n, _ in group})),
            "n": len(group),
        }
        for metric, _ in HEADLINE_METRICS + FOREGROUND_METRICS + [("queue_p99_ms", "")]:
            native = [n[metric] for n, _ in group]
            gpu = [g[metric] for _, g in group]
            if any(v is None for v in native + gpu):
                continue
            ratios = [g / n for n, g in zip(native, gpu) if n]
            entry[f"{metric}_native_median"] = median(native)
            entry[f"{metric}_gpu_median"] = median(gpu)
            entry[f"{metric}_native_values"] = native
            entry[f"{metric}_gpu_values"] = gpu
            entry[f"{metric}_ratio_geomean"] = geomean_ci(ratios)[0]
            entry[f"{metric}_gpu_better"] = sum(r < 1 for r in ratios)
        table.append(entry)
    return table


def job_level(jobs: list[dict]) -> dict:
    out = {}
    for label, campaigns in (("stress", ("stress", "stress-confirm")), ("mixed", ("mixed",))):
        for mode in MODES:
            subset = [j for j in jobs if j["campaign"] in campaigns and j["mode"] == mode]
            values = np.array([j["elapsed_ms"] for j in subset])
            cpu = np.array([j["cpu_ms"] for j in subset])
            first = np.array([j["elapsed_ms"] for j in subset if j["job_index"] == 0])
            out[f"{label}:{mode}"] = {
                "jobs": int(values.size),
                "p50_ms": float(np.percentile(values, 50)),
                "p90_ms": float(np.percentile(values, 90)),
                "p99_ms": float(np.percentile(values, 99)),
                "max_ms": float(values.max()),
                "cpu_p50_ms": float(np.percentile(cpu, 50)),
                "first_job_median_ms": float(np.median(first)),
            }
    return out


def regime(rows: list[dict], pair_list) -> dict:
    """Separate GPU trials that realized the offload benefit from those that ran at CPU speed."""
    gpu = [g for _, g in pair_list]
    slow = [g for g in gpu if g["slow_job_frac_steady"] > 0.5]
    fast_pairs = [(n, g) for n, g in pair_list if g["slow_job_frac_steady"] <= 0.5]
    native_fast_jobs = [n["slow_job_frac"] for n, _ in pair_list]
    return {
        "slow_job_threshold_ms": SLOW_JOB_MS,
        "warmup_jobs_excluded": WARMUP_JOBS,
        "gpu_trials": len(gpu),
        "gpu_trials_cpu_speed_regime": len(slow),
        "cpu_speed_regime_trials": [f"{g['bundle']}/{g['trial']} ({g['workload']} {g['rate']}/s rep {g['repetition']}, "
                                    f"{g['slow_job_frac_steady']:.0%} slow)" for g in slow],
        "gpu_steady_slow_job_frac_median": median(g["slow_job_frac_steady"] for g in gpu),
        "native_min_slow_job_frac": min(native_fast_jobs),
        "fast_regime_median_job": summarize(fast_pairs, "compaction_median_ms"),
        "fast_regime_steady_median_job": summarize(fast_pairs, "compaction_steady_median_ms"),
        "all_pairs_steady_median_job": summarize(pair_list, "compaction_steady_median_ms"),
    }


PRIMARY_ENDPOINTS = ("compaction_median_ms", "compaction_cpu_ms_per_gib", "process_cpu_window_s",
                     "write_p99_ms", "read_p99_ms")


def _apply_holm(stats: dict) -> None:
    """Holm adjustment over every paired test reported in the main summary table."""
    entries = [s for campaign in ("stress", "mixed") for s in stats[campaign].values()]
    for entry, adjusted in zip(entries, holm([e["wilcoxon_p"] for e in entries])):
        entry["wilcoxon_p_holm"] = adjusted
        entry["primary_endpoint"] = entry["metric"] in PRIMARY_ENDPOINTS


def pooled(stress, mixed) -> dict:
    """All-pairs and point-level (random-effects) estimates for the headline metrics."""
    out = {}
    everything = stress + mixed
    for metric, _ in HEADLINE_METRICS + FOREGROUND_METRICS:
        usable = [(n, g) for n, g in everything if n[metric] is not None and g[metric] is not None]
        groups = [[g[metric] / n[metric] for n, g in group if n[metric] is not None and g[metric] is not None]
                  for group in by_point(usable).values()]
        out[metric] = {"all_pairs": summarize(usable, metric), "point_level": point_level(groups)}
    return out


def sensitivity(jobs: list[dict], pair_list) -> list[dict]:
    """Does the regime analysis depend on the slow-job threshold or warm-up exclusion?"""
    by_trial: dict[str, list[float]] = defaultdict(list)
    for job in jobs:
        by_trial[job["trial"]].append(job["elapsed_ms"])
    out = []
    for threshold in (55.0, 58.0, 60.0, 62.0):
        for warmup in (0, 3, 5, 8):
            slow, ratios = 0, []
            for native, gpu in pair_list:
                g = np.array(by_trial[f"{gpu['bundle']}/{gpu['trial']}"][warmup:])
                n = np.array(by_trial[f"{native['bundle']}/{native['trial']}"][warmup:])
                if min(g.size, n.size) < 2:
                    continue
                if (g > threshold).mean() > 0.5:
                    slow += 1
                else:
                    ratios.append(float(np.median(g) / np.median(n)))
            gm, lo, hi = geomean_ci(ratios)
            out.append({"threshold_ms": threshold, "warmup_jobs": warmup, "slow_regime_trials": slow,
                        "normal_pairs": len(ratios), "normal_ratio": gm, "ci_low": lo, "ci_high": hi})
    return out


def _total_energy(row: dict) -> float | None:
    parts = (row.get("cpu_pkg_window_j"), row.get("dram_window_j"), row.get("gpu_energy_window_j"))
    return None if any(p is None for p in parts) else sum(parts)


def _ratio_summary(ratios: list[float]) -> dict:
    ratios = [r for r in ratios if r is not None and not math.isnan(r)]
    if not ratios:
        return {"n": 0}
    gm, lo, hi = geomean_ci(ratios)
    return {"n": len(ratios), "ratio": gm, "ci_low": lo, "ci_high": hi, "below_one": sum(r < 1 for r in ratios),
            "wilcoxon_p": wilcoxon_exact(ratios)}


REVISION_METRICS = ("compaction_median_ms", "compaction_cpu_ms_per_gib", "process_cpu_window_s",
                    "write_p50_ms", "write_p99_ms", "read_p99_ms", "total_energy_window_j", "slow_job_frac_steady")


def revision(rows: list[dict], jobs: list[dict], label: str = "revision") -> dict:
    subset = [r for r in rows if r["campaign"] == label]
    if not subset:
        return {}
    for row in subset:
        row["total_energy_window_j"] = _total_energy(row)
    cells: dict[tuple, dict] = defaultdict(dict)
    for row in subset:
        cells[(row["cell"], row["condition"], row["block"])][row["mode"]] = row

    def get(cell, condition, block, mode):
        return cells.get((cell, condition, block), {}).get(mode)

    blocks = sorted({r["block"] for r in subset})
    out: dict = {"blocks": len(blocks), "trials": len(subset), "valid": sum(r["valid"] for r in subset)}

    # R1: GPU/CPU paired ratios within each workload x condition.
    paired = []
    for cell, condition in sorted({(r["cell"], r["condition"]) for r in subset}):
        entry = {"cell": cell, "condition": condition}
        for metric in REVISION_METRICS:
            ratios = []
            for b in blocks:
                n, g = get(cell, condition, b, "native_cpu"), get(cell, condition, b, "cuda_local")
                if n and g and n.get(metric) and g.get(metric) is not None:
                    ratios.append(g[metric] / n[metric])
            entry[metric] = _ratio_summary(ratios)
        for mode in MODES:
            for metric in ("process_cpu_window_s", "total_energy_window_j", "cpu_pkg_window_j",
                           "gpu_energy_window_j", "compaction_median_ms", "write_p99_ms", "queue_p99_ms",
                           "completed_in_window_frac", "slow_job_frac_steady"):
                entry[f"{mode}:{metric}:median"] = median(
                    get(cell, condition, b, mode).get(metric) for b in blocks if get(cell, condition, b, mode))
        paired.append(entry)
    out["paired"] = paired

    # R2: factor effects, block-paired, within one backend.
    effects = []
    contrasts = [
        ("sync: blocking vs auto (unpinned)", "blocking-unpinned", "auto-unpinned"),
        ("sync: blocking vs auto (node 0)", "blocking-node0", "auto-node0"),
        ("NUMA: node 0 vs unpinned (auto)", "auto-node0", "auto-unpinned"),
        ("NUMA: node 0 vs unpinned (blocking)", "blocking-node0", "blocking-unpinned"),
        ("both: blocking+node 0 vs auto+unpinned", "blocking-node0", "auto-unpinned"),
        ("2 compaction threads vs 1 (auto, unpinned)", "bg3-auto-unpinned", "auto-unpinned"),
    ]
    for cell in sorted({r["cell"] for r in subset if r["cell"] != "write-heavy-5556"}):
        for label, treated, control in contrasts:
            for mode in MODES:
                entry = {"cell": cell, "contrast": label, "mode": mode}
                for metric in ("compaction_median_ms", "compaction_cpu_ms_per_gib", "process_cpu_window_s",
                               "write_p99_ms", "total_energy_window_j"):
                    ratios = []
                    for b in blocks:
                        t, c = get(cell, treated, b, mode), get(cell, control, b, mode)
                        if t and c and c.get(metric) and t.get(metric) is not None:
                            ratios.append(t[metric] / c[metric])
                    entry[metric] = _ratio_summary(ratios)
                effects.append(entry)
    out["effects"] = effects

    # R3: equal-resource comparison: GPU (1 compaction thread) vs CPU with 2 compaction threads.
    equal = {}
    for cell in sorted({r["cell"] for r in subset if r["cell"] != "write-heavy-5556"}):
        entry = {}
        for metric in ("compaction_median_ms", "compaction_ms_per_gib", "process_cpu_window_s", "write_p50_ms",
                       "write_p99_ms", "total_energy_window_j", "win_compaction_share"):
            ratios = []
            for b in blocks:
                g, n = get(cell, "auto-unpinned", b, "cuda_local"), get(cell, "bg3-auto-unpinned", b, "native_cpu")
                if g and n and n.get(metric) and g.get(metric) is not None:
                    ratios.append(g[metric] / n[metric])
            entry[metric] = _ratio_summary(ratios)
        equal[cell] = entry
    out["gpu_vs_two_cpu_threads"] = equal

    # R4: the previously excluded write-heavy 5,556 ops/s point, every trial.
    out["write_heavy_5556"] = [
        {key: r.get(key) for key in ("block", "mode", "ran_first_in_pair", "completed_in_window_frac", "queue_p99_ms",
                                      "write_p99_ms", "read_p99_ms", "compaction_median_ms", "valid")}
        for r in sorted((r for r in subset if r["cell"] == "write-heavy-5556"), key=lambda r: (r["block"], r["mode"]))
    ]
    return out


def scale(rows: list[dict]) -> dict:
    """Larger database (2M records, 16 MiB memtables, 60 s window)."""
    pair_list = pairs(rows, "scale")
    if not pair_list:
        return {}
    for native, gpu in pair_list:
        for row in (native, gpu):
            row["total_energy_window_j"] = _total_energy(row)
    out = {"pairs": len(pair_list)}
    for metric in ("compaction_median_ms", "compaction_ms_per_gib", "compaction_cpu_ms_per_gib", "process_cpu_window_s",
                   "write_p50_ms", "write_p99_ms", "total_energy_window_j", "win_compaction_share"):
        out[metric] = _ratio_summary([g[metric] / n[metric] for n, g in pair_list
                                      if n.get(metric) and g.get(metric) is not None])
    out["trials"] = [{key: r.get(key) for key in (
        "mode", "repetition", "valid", "jobs", "input_bytes", "compaction_median_ms", "compaction_max_ms",
        "process_cpu_window_s", "write_p99_ms", "win_compaction_share", "total_energy_window_j",
        "completed_in_window_frac")} for n, g in pair_list for r in (n, g)]
    return out


def profile_attribution() -> dict:
    path = DATA / "profile-attribution.csv"
    if not path.exists():
        return {}
    out: dict = {}
    for row in _read_csv(path):
        key = f"{row['profile']}:{row['mode']}"
        entry = out.setdefault(key, {"sync": row["sync"], "mode": row["mode"], "gpu_input": row["gpu_input"],
                                     "process_cpu_s": row["process_cpu_s"], "compaction_jobs": row["compaction_jobs"],
                                     "rocksdb_reported_compaction_cpu_s": row["rocksdb_reported_compaction_cpu_s"],
                                     "compaction_thread_ids": row["compaction_thread_ids"],
                                     "groups_cpu_s": defaultdict(float), "compaction_categories_cpu_s": defaultdict(float)})
        entry["groups_cpu_s"][row["thread_group"]] += row["cpu_s"]
        if row["thread_group"] == "compaction":
            entry["compaction_categories_cpu_s"][row["category"]] += row["cpu_s"]
    for entry in out.values():
        entry["groups_cpu_s"] = dict(entry["groups_cpu_s"])
        entry["compaction_categories_cpu_s"] = dict(entry["compaction_categories_cpu_s"])
        entry["sampled_compaction_cpu_s"] = sum(entry["compaction_categories_cpu_s"].values())
    return out


def capacity(rows: list[dict]) -> list[dict]:
    out = []
    for campaign in ("read-capacity", "mixed-screen"):
        subset = sorted((r for r in rows if r["campaign"] == campaign),
                        key=lambda r: (WORKLOAD_ORDER.index(r["workload"]), r["rate"], r["mode"]))
        for row in subset:
            out.append({key: row[key] for key in (
                "campaign", "workload", "rate", "mode", "completed_in_window_frac", "queue_p99_ms",
                "write_p99_ms", "read_p99_ms", "compaction_median_ms")})
    return out


# --------------------------------------------------------------------------- tables


def write_tables(stats: dict) -> None:
    labels = dict(HEADLINE_METRICS + FOREGROUND_METRICS)
    lines = ["| Metric | Campaign | Pairs | GPU better | GPU/CPU geo-mean | 95% CI | Median ratio | Range | Wilcoxon p | Sign p |",
             "|---|---|---:|---:|---:|---|---:|---|---:|---:|"]
    latex = [r"\begin{tabular}{llrrrlr}", r"\toprule",
             r"Metric & Campaign & Pairs & GPU better & GPU/CPU & 95\% CI & Wilcoxon $p$ \\", r"\midrule"]
    for campaign in ("stress", "mixed"):
        for metric, s in stats[campaign].items():
            lines.append(
                f"| {labels[metric]} | {campaign} | {s['n_pairs']} | {s['gpu_better']}/{s['n_pairs']} | "
                f"{s['geomean_ratio']:.3f} ({_pct(s['geomean_ratio'])}) | [{s['ci_low']:.3f}, {s['ci_high']:.3f}] | "
                f"{s['median_ratio']:.3f} | "
                f"{s['min_ratio']:.3f}–{s['max_ratio']:.3f} | {_fmt_p(s['wilcoxon_p'])} | {_fmt_p(s['sign_p'])} |"
            )
            latex.append(
                f"{labels[metric]} & {campaign} & {s['n_pairs']} & {s['gpu_better']}/{s['n_pairs']} & "
                f"{s['geomean_ratio']:.3f} & [{s['ci_low']:.3f}, {s['ci_high']:.3f}] & {_fmt_p(s['wilcoxon_p'])} \\\\"
            )
        latex.append(r"\midrule" if campaign == "stress" else r"\bottomrule")
    latex.append(r"\end{tabular}")
    (TABLES / "paired-summary.md").write_text("\n".join(lines) + "\n")
    (TABLES / "paired-summary.tex").write_text("\n".join(latex) + "\n")

    def pair(p, metric):
        if f"{metric}_native_median" not in p:
            return "—"
        return f"{p[f'{metric}_native_median']:.1f} → {p[f'{metric}_gpu_median']:.1f}"

    def delta(p, metric):
        key = f"{metric}_ratio_geomean"
        return "—" if key not in p else f"{_pct(p[key])} ({p[f'{metric}_gpu_better']}/{p['n']})"

    point_lines = [
        "| Workload | Ops/s | Writes/s | Pairs | Median job, CPU → GPU (ms) | Median job Δ | Wall/GiB Δ | "
        "Compaction CPU/GiB Δ | Process CPU Δ | Write p99, CPU → GPU (ms) | Read p99, CPU → GPU (ms) |",
        "|---|---:|---:|---:|---|---:|---:|---:|---:|---|---|",
    ]
    for p in stats["points"]:
        point_lines.append(
            f"| {p['workload']} | {p['rate']:,} | {p['write_rate']:,} | {p['n']} | {pair(p, 'compaction_median_ms')} | "
            f"{delta(p, 'compaction_median_ms')} | {delta(p, 'compaction_ms_per_gib')} | "
            f"{delta(p, 'compaction_cpu_ms_per_gib')} | {delta(p, 'process_cpu_s')} | "
            f"{pair(p, 'write_p99_ms')} | {pair(p, 'read_p99_ms')} |"
        )
    (TABLES / "per-point.md").write_text("\n".join(point_lines) + "\n")

    keys = [k for k in stats["points"][0] if not k.endswith("_values")]
    with (TABLES / "per-point.csv").open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=keys, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(stats["points"])

    write_pooled_tables(stats)
    if stats.get("revision"):
        write_revision_tables(stats["revision"])
    if stats.get("revision_gpu_input"):
        write_revision_tables(stats["revision_gpu_input"], prefix="revision-gpu-input")
    if stats.get("scale"):
        write_scale_table(stats["scale"])


def _cell(summary: dict) -> str:
    if not summary or summary.get("n", summary.get("n_pairs", 0)) == 0:
        return "—"
    ratio = summary.get("ratio", summary.get("geomean_ratio"))
    n = summary.get("n", summary.get("n_pairs"))
    below = summary.get("below_one", summary.get("gpu_better"))
    return f"{_pct(ratio)} [{_pct(summary['ci_low'])}, {_pct(summary['ci_high'])}] ({below}/{n})"


def write_pooled_tables(stats: dict) -> None:
    labels = dict(HEADLINE_METRICS + FOREGROUND_METRICS)
    lines = ["| Metric | All 45 pairs: GPU vs CPU [95% CI] (GPU lower) | Median pair ratio | "
             "Point level, 14 points [95% CI] (GPU lower) | Point-level Wilcoxon p |",
             "|---|---|---:|---|---:|"]
    for metric, entry in stats["pooled"].items():
        a, p = entry["all_pairs"], entry["point_level"]
        lines.append(f"| {labels[metric]} | {_cell(a)} | {a['median_ratio']:.3f} | "
                     f"{_pct(p['ratio'])} [{_pct(p['ci_low'])}, {_pct(p['ci_high'])}] ({p['points_gpu_better']}/{p['points']}) | "
                     f"{_fmt_p(p['wilcoxon_p'])} |")
    (TABLES / "pooled.md").write_text("\n".join(lines) + "\n")

    holm_lines = ["| Metric | Campaign | Primary | Wilcoxon p | Holm-adjusted p |", "|---|---|:-:|---:|---:|"]
    for campaign in ("stress", "mixed"):
        for metric, entry in stats[campaign].items():
            holm_lines.append(f"| {labels[metric]} | {campaign} | {'✓' if entry['primary_endpoint'] else ''} | "
                              f"{_fmt_p(entry['wilcoxon_p'])} | {_fmt_p(entry['wilcoxon_p_holm'])} |")
    (TABLES / "holm.md").write_text("\n".join(holm_lines) + "\n")

    sens = ["| Slow-job threshold | Warm-up jobs excluded | CPU-speed GPU trials | Normal pairs | "
            "Normal-regime median job, GPU vs CPU [95% CI] |", "|---:|---:|---:|---:|---|"]
    for row in stats["sensitivity"]:
        sens.append(f"| {row['threshold_ms']:.0f} ms | {row['warmup_jobs']} | {row['slow_regime_trials']} | "
                    f"{row['normal_pairs']} | {_pct(row['normal_ratio'])} [{_pct(row['ci_low'])}, {_pct(row['ci_high'])}] |")
    (TABLES / "sensitivity.md").write_text("\n".join(sens) + "\n")


def write_scale_table(sc: dict) -> None:
    labels = {"compaction_median_ms": "Median compaction job", "compaction_ms_per_gib": "Compaction wall time per GiB",
              "compaction_cpu_ms_per_gib": "Compaction-thread CPU per GiB (RocksDB accounting)",
              "process_cpu_window_s": "Whole-process CPU (window)", "write_p50_ms": "Write p50",
              "write_p99_ms": "Write p99", "total_energy_window_j": "CPU pkg + DRAM + GPU energy (window)",
              "win_compaction_share": "Compaction busy share of window"}
    lines = ["| Metric | GPU vs CPU [95% CI] (GPU lower) |", "|---|---|"]
    for key, label in labels.items():
        lines.append(f"| {label} | {_cell(sc[key])} |")
    (TABLES / "scale.md").write_text("\n".join(lines) + "\n")


def write_revision_tables(rev: dict, prefix: str = "revision") -> None:
    lines = ["| Workload | Condition | Median job | Compaction CPU/GiB | Process CPU | Write p99 | Host+GPU energy |",
             "|---|---|---|---|---|---|---|"]
    for e in rev["paired"]:
        lines.append(f"| {e['cell']} | {e['condition']} | {_cell(e['compaction_median_ms'])} | "
                     f"{_cell(e['compaction_cpu_ms_per_gib'])} | {_cell(e['process_cpu_window_s'])} | "
                     f"{_cell(e['write_p99_ms'])} | {_cell(e['total_energy_window_j'])} |")
    (TABLES / f"{prefix}-paired.md").write_text("\n".join(lines) + "\n")

    lines = ["| Workload | Contrast | Backend | Median job | Compaction CPU/GiB | Process CPU | Write p99 | Host+GPU energy |",
             "|---|---|---|---|---|---|---|---|"]
    for e in rev["effects"]:
        lines.append(f"| {e['cell']} | {e['contrast']} | {'GPU' if e['mode'] == 'cuda_local' else 'CPU'} | "
                     f"{_cell(e['compaction_median_ms'])} | {_cell(e['compaction_cpu_ms_per_gib'])} | "
                     f"{_cell(e['process_cpu_window_s'])} | {_cell(e['write_p99_ms'])} | {_cell(e['total_energy_window_j'])} |")
    (TABLES / f"{prefix}-effects.md").write_text("\n".join(lines) + "\n")

    lines = ["| Workload | Median job | Compaction wall/GiB | Process CPU | Write p50 | Write p99 | Host+GPU energy | Compaction busy share |",
             "|---|---|---|---|---|---|---|---|"]
    for cell, e in rev["gpu_vs_two_cpu_threads"].items():
        lines.append(f"| {cell} | {_cell(e['compaction_median_ms'])} | {_cell(e['compaction_ms_per_gib'])} | "
                     f"{_cell(e['process_cpu_window_s'])} | {_cell(e['write_p50_ms'])} | {_cell(e['write_p99_ms'])} | "
                     f"{_cell(e['total_energy_window_j'])} | {_cell(e['win_compaction_share'])} |")
    (TABLES / f"{prefix}-equal-resource.md").write_text("\n".join(lines) + "\n")

    lines = ["| Block | Backend | Ran first | Completed in window | Queue p99 (ms) | Write p99 (ms) | Read p99 (ms) | Median job (ms) | Valid |",
             "|---:|---|:-:|---:|---:|---:|---:|---:|:-:|"]
    for r in rev["write_heavy_5556"]:
        lines.append(f"| {r['block']} | {'GPU' if r['mode'] == 'cuda_local' else 'CPU'} | {'✓' if r['ran_first_in_pair'] else ''} | "
                     f"{r['completed_in_window_frac']:.2%} | {r['queue_p99_ms']:.1f} | {r['write_p99_ms']:.1f} | "
                     f"{r['read_p99_ms']:.1f} | {r['compaction_median_ms']:.1f} | {'✓' if r['valid'] else '✗'} |")
    (TABLES / f"{prefix}-write-heavy-5556.md").write_text("\n".join(lines) + "\n")


# --------------------------------------------------------------------------- figures

COLORS = {"native_cpu": "#4C72B0", "cuda_local": "#DD8452"}
LABELS = {"native_cpu": "CPU (native RocksDB)", "cuda_local": "GPU offload (CUDA + nvCOMP)"}
SHORT = {"write-only": "write", "write-heavy": "W-heavy", "update-heavy": "update", "balanced": "balanced",
         "read-dominant": "read"}


def _rate_label(rate: int) -> str:
    return f"{rate / 1000:.3g}k"


def _style():
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    plt.rcParams.update({
        "font.family": "serif",
        "font.size": 8,
        "axes.titlesize": 8,
        "axes.labelsize": 8,
        "legend.fontsize": 7,
        "xtick.labelsize": 7,
        "ytick.labelsize": 7,
        "axes.spines.top": False,
        "axes.spines.right": False,
        "pdf.fonttype": 42,
        "ps.fonttype": 42,
        "savefig.bbox": "tight",
        "savefig.pad_inches": 0.02,
        "svg.hashsalt": "lsm-lab",
    })
    return plt


def _save(fig, name: str) -> None:
    for ext in ("pdf", "png"):
        fig.savefig(FIGURES / f"{name}.{ext}", dpi=300, metadata={"CreationDate": None} if ext == "pdf" else None)


def figures(rows: list[dict], jobs: list[dict], stats: dict) -> None:
    plt = _style()
    points = stats["points"]

    # Figure 1: per-job duration CDF.
    fig, axes = plt.subplots(1, 2, figsize=(7.0, 2.3), sharey=True)
    panels = ((("stress", "stress-confirm"), "(a) Write-only stress, 3k–5k writes/s"),
              (("mixed",), "(b) Mixed read/write, 11 operating points"))
    for ax, (campaigns, title) in zip(axes, panels):
        counts = []
        for mode in MODES:
            values = np.sort([j["elapsed_ms"] for j in jobs if j["campaign"] in campaigns and j["mode"] == mode])
            counts.append(values.size)
            ax.plot(values, np.arange(1, values.size + 1) / values.size, color=COLORS[mode], label=LABELS[mode], lw=1.4)
        ax.set_title(f"{title}\n{counts[0]:,} CPU jobs, {counts[1]:,} GPU jobs", loc="left")
        ax.set_xlabel("Compaction job duration (ms)")
        ax.set_xlim(40, 110)
        ax.grid(alpha=0.25, lw=0.5)
    axes[0].set_ylabel("Cumulative fraction of jobs")
    fig.legend(*axes[0].get_legend_handles_labels(), frameon=False, ncol=2, loc="upper center",
               bbox_to_anchor=(0.5, 1.12))
    _save(fig, "fig1-job-duration-cdf")
    plt.close(fig)

    # Figure 2: paired per-point change for compaction metrics and process CPU.
    metrics = [
        ("compaction_median_ms", "Median compaction-job duration", "#DD8452"),
        ("compaction_cpu_ms_per_gib", "Compaction-thread CPU per GiB", "#55A868"),
        ("process_cpu_s", "Whole-process CPU (all threads)", "#8172B2"),
    ]
    fig, ax = plt.subplots(figsize=(7.0, 2.6))
    width = 0.26
    xs = np.arange(len(points))
    for offset, (metric, label, color) in enumerate(metrics):
        for i, p in enumerate(points):
            ratios = np.array(p[f"{metric}_gpu_values"]) / np.array(p[f"{metric}_native_values"])
            x = xs[i] + (offset - 1) * width
            ax.bar(x, (geomean_ci(list(ratios))[0] - 1) * 100, width * 0.9, color=color,
                   label=label if i == 0 else None, alpha=0.85)
            ax.scatter(np.full(ratios.size, x), (ratios - 1) * 100, s=4, color="black", zorder=3, lw=0)
    ax.axhline(0, color="black", lw=0.6)
    groups: dict[str, list[int]] = defaultdict(list)
    for i, p in enumerate(points):
        groups[p["workload"]].append(i)
    for workload, members in groups.items():
        if members[0] > 0:
            ax.axvline(members[0] - 0.5, color="grey", lw=0.4, ls=":")
        ax.text(np.mean(members), -0.16, workload, transform=ax.get_xaxis_transform(), ha="center", va="top", fontsize=7)
    ax.set_xticks(xs)
    ax.set_xticklabels([_rate_label(p["rate"]) for p in points], fontsize=6.5)
    ax.set_ylabel("GPU relative to CPU (%)")
    ax.grid(axis="y", alpha=0.25, lw=0.5)
    ax.legend(frameon=False, ncol=3, loc="upper center", bbox_to_anchor=(0.5, 1.13))
    _save(fig, "fig2-paired-change-by-point")
    plt.close(fig)

    # Figure 3: foreground p99 per trial in the mixed confirmation; every repetition shown.
    mixed_points = [p for p in points if p["workload"] != "write-only"]
    fig, axes = plt.subplots(2, 1, figsize=(7.0, 3.6), sharex=True)
    for ax, (metric, title) in zip(axes, (("write_p99_ms", "(a) Write p99"), ("read_p99_ms", "(b) Read p99"))):
        for i, p in enumerate(mixed_points):
            for mode, side, tag in (("native_cpu", -0.12, "native"), ("cuda_local", 0.12, "gpu")):
                values = p.get(f"{metric}_{tag}_values", [])
                ax.scatter(np.full(len(values), i + side), values, s=10, color=COLORS[mode],
                           label=LABELS[mode] if i == 0 else None, zorder=3)
        ax.set_yscale("log")
        groups_fg: dict[str, list[int]] = defaultdict(list)
        for i, p in enumerate(mixed_points):
            groups_fg[p["workload"]].append(i)
        for workload, members in groups_fg.items():
            if members[0] > 0:
                ax.axvline(members[0] - 0.5, color="grey", lw=0.4, ls=":")
        ax.set_title(title + " of each trial", loc="left")
        ax.set_ylabel("ms from intended arrival")
        ax.grid(alpha=0.25, lw=0.5)
    axes[-1].set_xticks(range(len(mixed_points)))
    axes[-1].set_xticklabels([_rate_label(p["rate"]) for p in mixed_points], fontsize=6.5)
    for workload, members in groups_fg.items():
        axes[-1].text(np.mean(members), -0.2, workload, transform=axes[-1].get_xaxis_transform(), ha="center",
                      va="top", fontsize=7)
    fig.legend(*axes[0].get_legend_handles_labels(), frameon=False, ncol=2, loc="upper center",
               bbox_to_anchor=(0.5, 1.03))
    _save(fig, "fig3-foreground-p99-per-trial")
    plt.close(fig)

    # Figure 4: compaction busy share of the measurement window under the stress workload.
    fig, ax = plt.subplots(figsize=(3.4, 2.2))
    stress_rows = [r for r in rows if r["campaign"] in ("stress", "stress-confirm")]
    for mode in MODES:
        subset = [r for r in stress_rows if r["mode"] == mode and r.get("win_compaction_share") is not None]
        rates = sorted({r["rate"] for r in subset})
        means = [mean(r["win_compaction_share"] * 100 for r in subset if r["rate"] == rate) for rate in rates]
        ax.plot(rates, means, marker="o", color=COLORS[mode], label=LABELS[mode], lw=1.2, ms=3)
        ax.scatter([r["rate"] for r in subset], [r["win_compaction_share"] * 100 for r in subset],
                   color=COLORS[mode], s=5, alpha=0.5)
    ax.set_xlabel("Offered write rate (writes/s)")
    ax.set_ylabel("Compaction busy time\n(% of measurement window)")
    ax.set_xticks([3000, 4000, 5000])
    ax.grid(alpha=0.25, lw=0.5)
    ax.legend(frameon=False)
    _save(fig, "fig4-compaction-busy-share")
    plt.close(fig)

    # Figure 5: job-duration raster per trial, exposing warm-up and CPU-speed regimes.
    from matplotlib.colors import TwoSlopeNorm

    fig, axes = plt.subplots(1, 2, figsize=(7.0, 3.6), sharey=True)
    ordered = sorted(
        (r for r in rows if r["campaign"] in ("stress", "stress-confirm", "mixed")),
        key=lambda r: (WORKLOAD_ORDER.index(r["workload"]), r["rate"], r["repetition"], r["campaign"]),
    )
    by_trial: dict[str, list[float]] = defaultdict(list)
    for job in jobs:
        by_trial[job["trial"]].append(job["elapsed_ms"])
    norm = TwoSlopeNorm(vmin=45, vcenter=SLOW_JOB_MS, vmax=80)
    for ax, mode in zip(axes, MODES):
        trials = [r for r in ordered if r["mode"] == mode]
        width = max(len(by_trial[f"{r['bundle']}/{r['trial']}"]) for r in trials)
        grid = np.full((len(trials), width), np.nan)
        for i, r in enumerate(trials):
            values = by_trial[f"{r['bundle']}/{r['trial']}"]
            grid[i, : len(values)] = values
        image = ax.imshow(grid, aspect="auto", cmap="coolwarm", norm=norm, interpolation="nearest")
        ax.set_title(f"({'a' if mode == 'native_cpu' else 'b'}) {LABELS[mode]}", loc="left")
        ax.set_xlabel("Compaction job index within trial")
        ticks = [i for i, r in enumerate(trials) if i == 0 or (r["workload"], r["rate"]) != (trials[i - 1]["workload"], trials[i - 1]["rate"])]
        ax.set_yticks(ticks)
        ax.set_yticklabels([f"{SHORT[trials[i]['workload']]} {_rate_label(trials[i]['rate'])}" for i in ticks], fontsize=5.5)
    fig.colorbar(image, ax=axes, label="Job duration (ms)", shrink=0.8, pad=0.01)
    _save(fig, "fig5-job-duration-raster")
    plt.close(fig)

    revision_figure(plt, rows, stats.get("revision") or {})
    profile_figure(plt, stats.get("profiles") or {})


PROFILE_CATEGORIES = [("lz4", "LZ4", "#4C72B0"), ("crc32c", "CRC32C", "#55A868"), ("memcpy", "memcpy/memmove", "#DD8452"),
                      ("kernel", "kernel (faults, threads)", "#8172B2"), ("cuda_runtime_driver", "CUDA runtime/driver", "#C44E52"),
                      ("rocksdb", "RocksDB other", "#937860"), ("other", "other", "#8C8C8C")]


def profile_figure(plt, profiles: dict) -> None:
    """Figure 7: CPU on compaction threads (main + helpers) by category, one paired trial each."""
    chosen = [(key, value) for key, value in profiles.items() if not value["gpu_input"]]
    if not chosen:
        return
    native = [(k, v) for k, v in chosen if v["mode"] == "native_cpu" and v["sync"] == "auto"][:1]
    gpu = sorted([(k, v) for k, v in chosen if v["mode"] == "cuda_local"], key=lambda kv: kv[1]["sync"])
    bars = [("CPU (native)", native[0][1])] + [(f"GPU, {v['sync']} sync", v) for _, v in gpu] if native else []
    fig, ax = plt.subplots(figsize=(7.0, 1.9))
    for y, (label, entry) in enumerate(bars):
        left = 0.0
        for category, name, color in PROFILE_CATEGORIES:
            width = entry["compaction_categories_cpu_s"].get(category, 0.0)
            ax.barh(y, width, left=left, color=color, label=name if y == 0 else None, height=0.6)
            left += width
        threads = entry["compaction_thread_ids"]
        ax.text(left + 0.05, y, f"{left:.2f} CPU-s; {threads} thread{'s' if threads != 1 else ''}; "
                                f"RocksDB reports {entry['rocksdb_reported_compaction_cpu_s']:.2f}", va="center", fontsize=6.5)
    ax.set_yticks(range(len(bars)))
    ax.set_yticklabels([label for label, _ in bars])
    ax.invert_yaxis()
    ax.set_xlabel("CPU time on compaction threads in one write-only 5,000/s trial (s), from 1 ms cpu-clock samples")
    ax.set_xlim(0, max(sum(e["compaction_categories_cpu_s"].values()) for _, e in bars) * 1.75)
    ax.legend(frameon=False, ncol=4, loc="lower center", bbox_to_anchor=(0.5, 1.0), fontsize=6.5)
    _save(fig, "fig7-compaction-cpu-attribution")
    plt.close(fig)


CONDITION_ORDER = ["auto-unpinned", "blocking-unpinned", "auto-node0", "blocking-node0", "bg3-auto-unpinned"]
CONDITION_LABELS = {"auto-unpinned": "published", "blocking-unpinned": "blocking\nsync",
                    "auto-node0": "GPU-node\npinning", "blocking-node0": "blocking +\npinning", "bg3-auto-unpinned": "2 compact.\nthreads"}


def revision_figure(plt, rows: list[dict], rev: dict) -> None:
    """Figure 6: paired GPU-vs-CPU change under each revision condition."""
    if not rev:
        return
    metrics = [("compaction_median_ms", "Median job duration", "#DD8452"),
               ("process_cpu_window_s", "Whole-process CPU", "#8172B2"),
               ("total_energy_window_j", "CPU pkg + DRAM + GPU energy", "#C44E52")]
    cells = ["stress-5000", "balanced-8000"]
    titles = {"stress-5000": "(a) Write-only, 5,000 writes/s", "balanced-8000": "(b) Balanced 50/50, 8,000 ops/s"}
    fig, axes = plt.subplots(1, 2, figsize=(7.0, 2.6), sharey=True)
    entries = {(e["cell"], e["condition"]): e for e in rev["paired"]}
    subset = [r for r in rows if r["campaign"] == "revision"]
    for ax, cell in zip(axes, cells):
        for offset, (metric, label, color) in enumerate(metrics):
            for i, condition in enumerate(CONDITION_ORDER):
                e = entries.get((cell, condition), {}).get(metric, {})
                if not e.get("n"):
                    continue
                x = i + (offset - 1) * 0.26
                ax.bar(x, (e["ratio"] - 1) * 100, 0.24, color=color, alpha=0.85, label=label if i == 0 else None)
                pair_ratios = []
                for block in sorted({r["block"] for r in subset}):
                    pair = {r["mode"]: r for r in subset if r["cell"] == cell and r["condition"] == condition and r["block"] == block}
                    if len(pair) == 2 and pair["native_cpu"].get(metric) and pair["cuda_local"].get(metric) is not None:
                        pair_ratios.append(pair["cuda_local"][metric] / pair["native_cpu"][metric])
                ax.scatter(np.full(len(pair_ratios), x), (np.array(pair_ratios) - 1) * 100, s=4, color="black", zorder=3, lw=0)
        ax.axhline(0, color="black", lw=0.6)
        ax.set_xticks(range(len(CONDITION_ORDER)))
        ax.set_xticklabels([CONDITION_LABELS[c] for c in CONDITION_ORDER], fontsize=5.8)
        ax.set_title(titles[cell], loc="left")
        ax.grid(axis="y", alpha=0.25, lw=0.5)
    axes[0].set_ylabel("GPU relative to CPU (%)")
    fig.legend(*axes[0].get_legend_handles_labels(), frameon=False, ncol=3, loc="upper center", bbox_to_anchor=(0.5, 1.07))
    _save(fig, "fig6-revision-ablation")
    plt.close(fig)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    ext = sub.add_parser("extract", help="raw bundles -> publication/data/*.csv")
    ext.add_argument("raw", type=Path, help="directory containing the Zyra result bundles")
    ext.add_argument("--profile", type=Path, action="append", default=[],
                     help="publication/profile.sh output directory (repeatable)")
    ext.add_argument("--revision", type=Path, action="append", default=[],
                     help="campaign directory with campaign.json (repeatable)")
    sub.add_parser("report", help="CSV -> tables, statistics, figures")
    args = parser.parse_args()
    if args.command == "extract":
        extract(args.raw, args.revision)
        if args.profile:
            extract_profiles(args.profile)
    else:
        report()


if __name__ == "__main__":
    main()
