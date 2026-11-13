#!/usr/bin/env python3
"""Results of the GPU multiplexing campaigns (PREREGISTRATION.md, "GPU multiplexing").

  python3 publication/multiplex/analyze.py

Reads publication/data/multiplex/{m0,m1,m2,m3,m4}, prints the tables and the outcome of
hypotheses X1 to X5, and writes publication/figures/fig42 to fig47. Every number is computed
from the raw records; a campaign whose data is missing is skipped.
"""

from __future__ import annotations

import json
import statistics
import sys
from collections import defaultdict
from pathlib import Path

import matplotlib.pyplot as plt

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
from make_figures import DOUBLE_COLUMN, INK, finish_axis, save  # noqa: E402

DATA = HERE.parent / "data" / "multiplex"
PRICES = json.loads((HERE.parent / "prices.json").read_text())
CORE_HOUR, GPU_HOUR = PRICES["physical_core_hour"], PRICES["gpu_hour"]
SERVER_CORES = 4

ARMS = {
    "native": ("RocksDB (no GPU)", "#4d4d4d", "s"),
    "separate": ("GPU used by each process", "#0072B2", "o"),
    "mps": ("Each process, through CUDA MPS", "#009E73", "^"),
    "service-fair": ("Shared GPU service, fair", "#D55E00", "D"),
    "service-fifo": ("Shared GPU service, FIFO", "#E69F00", "d"),
}


def median(values):
    values = [v for v in values if v is not None]
    return statistics.median(values) if values else float("nan")


def jsonl(path: Path) -> list[dict]:
    return [json.loads(line) for line in path.open()] if path.exists() else []


def cost_per_billion(writes_per_second: float, cores: int, databases: int, gpu: bool, server: bool) -> float:
    """Dollars per billion writes for one database: its cores, plus its share of the GPU and of the
    service's cores."""
    hourly = cores * CORE_HOUR
    if gpu:
        hourly += GPU_HOUR / databases
    if server:
        hourly += SERVER_CORES * CORE_HOUR / databases
    return hourly / (writes_per_second * 3600) * 1e9


# ---------------------------------------------------------------------------------------------
# M0: capacity of the service against the number of engines.

def m0() -> dict[int, dict]:
    runs = defaultdict(list)
    for record in jsonl(DATA / "m0" / "m0.jsonl"):
        runs[record["engines"]].append(record)
    summary = {}
    for k, records in sorted(runs.items()):
        summary[k] = {"gib_s": median([r["replay"]["input_gib_per_second"] for r in records]),
                      "p50_s": median([r["replay"]["latency_p50_s"] for r in records]),
                      "failures": sum(r["replay"]["failures"] for r in records),
                      "oom_retries": sum(r["server"].get("merge_oom_retries") or 0 for r in records),
                      "gpu_memory_gib": max(r["gpu_memory_used_max_mib"] for r in records) / 1024}
    return summary


def chosen_engines(summary: dict[int, dict]) -> int:
    best = max(s["gib_s"] for s in summary.values())
    return min(k for k, s in summary.items() if s["gib_s"] >= 0.95 * best)


# ---------------------------------------------------------------------------------------------
# M1, M3, M4: system_campaign records (one per trial, N databases).

def databases(record: dict) -> list[dict]:
    return [record["result"]] if "result" in record else record.get("results", [])


def fallbacks(result: dict) -> int:
    return (result.get("transform") or {}).get("merge_fallbacks") or 0


def trial_summary(record: dict) -> dict:
    results = databases(record)
    ops = [r["throughput"]["ops_per_second"] for r in results]
    server = record.get("server_statistics") or {}
    return {"ops": ops, "mean": statistics.mean(ops), "slowest_share": min(ops) / max(ops),
            "aggregate": sum(ops), "fallbacks": sum(fallbacks(r) for r in results),
            "verified": all(r["verification"]["keys"] == r["config"]["records"] for r in results),
            "oom_retries": server.get("merge_oom_retries"), "gpu": record.get("gpu_utilization_mean"),
            "p999_ms": [r["write_latency"]["p999_ns"] / 1e6 for r in results],
            "p99_ms": [r["write_latency"]["p99_ns"] / 1e6 for r in results],
            "server_cpu": record.get("server_cpu_seconds"),
            "seconds": max(r["ingest_and_compact"]["seconds"] for r in results),
            "tenants": (server.get("scheduler") or {}).get("tenants", {})}


def m1() -> dict[tuple, list[dict]]:
    runs = defaultdict(list)
    for record in jsonl(DATA / "m1" / "results.jsonl"):
        trial = record["trial"]
        if record["exit_code"] != 0:
            raise SystemExit(f"M1 trial {record['index']} failed")
        runs[(trial["cores"], trial["design"], trial["instances"])].append(trial_summary(record))
    return runs


def row(runs: list[dict]) -> dict:
    return {"per_db": median([r["mean"] for r in runs]), "slowest_share": median([r["slowest_share"] for r in runs]),
            "aggregate": median([r["aggregate"] for r in runs]), "fallbacks": [r["fallbacks"] for r in runs],
            "verified": all(r["verified"] for r in runs), "oom": [r["oom_retries"] for r in runs],
            "gpu": median([r["gpu"] for r in runs]), "p999_ms": median([max(r["p999_ms"]) for r in runs]),
            "slowest_db": min(min(r["ops"]) for r in runs)}


def print_m1(runs) -> dict:
    print("\n## M1: databases on zyra sharing its GPU (medians of 3 blocks)\n")
    print("| Cores | Arm | N | Writes/s per database | Share of N = 1 | Slowest / fastest database | All databases | "
          "CPU fallbacks (per block) | Worst p99.9 (ms) | GPU busy | $ per billion writes |")
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    table = {}
    for cores in (4, 8):
        for design in ARMS:
            counts = sorted(n for c, d, n in runs if c == cores and d == design)
            for n in counts:
                r = row(runs[(cores, design, n)])
                base = row(runs[(cores, design, 1)])["per_db"] if (cores, design, 1) in runs else float("nan")
                r["share"] = r["per_db"] / base
                r["cost"] = cost_per_billion(r["per_db"], cores, n, design != "native", design.startswith("service"))
                table[(cores, design, n)] = r
                print(f"| {cores} | {ARMS[design][0]} | {n} | {r['per_db'] / 1e3:,.0f}k | {r['share']:.2f} | "
                      f"{r['slowest_share']:.2f} | {r['aggregate'] / 1e6:.2f}M | {r['fallbacks']} | {r['p999_ms']:.1f} | "
                      f"{r['gpu']:.0f}% | {r['cost']:.3f} |")
    return table


def queueing(runs) -> None:
    print("\n### Service queueing (median of blocks)\n")
    print("| Cores | Policy | N | Jobs per database | Mean wait per job (ms) | Mean service per job (ms) | "
          "Server cores busy |")
    print("|---|---|---|---|---|---|---|")
    for (cores, design, n), trials in sorted(runs.items()):
        if not design.startswith("service"):
            continue
        waits, services, jobs, busy = [], [], [], []
        for t in trials:
            count = sum(v["jobs"] for v in t["tenants"].values())
            waits.append(sum(v["wait_ns"] for v in t["tenants"].values()) / count / 1e6)
            services.append(sum(v["service_ns"] for v in t["tenants"].values()) / count / 1e6)
            jobs.append(count / n)
            busy.append(t["server_cpu"] / t["seconds"] if t["server_cpu"] else None)
        print(f"| {cores} | {design.split('-')[1]} | {n} | {median(jobs):.0f} | {median(waits):.0f} | "
              f"{median(services):.0f} | {median(busy):.1f} |")


def figure_local(table) -> None:
    """Writes/s per database against the number of databases, one panel per core count."""
    fig, axes = plt.subplots(1, 2, figsize=(DOUBLE_COLUMN, 2.6), sharey=False)
    for ax, cores in zip(axes, (4, 8)):
        counts = sorted({n for c, _, n in table if c == cores})
        for design, (label, colour, marker) in ARMS.items():
            points = [(n, table[(cores, design, n)]["per_db"] / 1e3) for n in counts if (cores, design, n) in table]
            if not points:
                continue
            xs, ys = zip(*points)
            style = "--" if design == "native" else "-"
            ax.plot(xs, ys, style, color=colour, marker=marker, markersize=4, linewidth=1.3, label=label, zorder=3)
        most = counts[-1]
        gains = [table[(cores, d, most)]["per_db"] / table[(cores, "native", most)]["per_db"]
                 for d in ARMS if d != "native" and (cores, d, most) in table]
        ax.annotate(f"{min(gains):.2f}–{max(gains):.2f}× RocksDB\nat {most} databases",
                    (most, table[(cores, "native", most)]["per_db"] / 1e3), xytext=(0, 8), textcoords="offset points",
                    ha="right", va="bottom", fontsize=6.8, fontweight="bold", color=INK)
        ax.set_xticks(counts)
        ax.set_xlabel("Databases sharing one GPU")
        ax.set_title(f"{cores} cores per database", fontsize=8, color=INK)
        ax.set_ylim(0, None)
        finish_axis(ax, "Writes/s per database (thousands)")
    handles, labels = axes[1].get_legend_handles_labels()
    fig.legend(handles, labels, loc="lower center", ncol=3, bbox_to_anchor=(0.5, 0.86), handlelength=2.0,
               columnspacing=1.2, fontsize=7)
    fig.tight_layout(rect=(0, 0, 1, 0.84))
    save(fig, "fig42_sharing_local")


def figure_local_most(table) -> None:
    """At the most databases: throughput, slowest database, CPU fallbacks, tail latency."""
    cases = [(8, 4), (4, 6)]
    arms = ["separate", "mps", "service-fair", "service-fifo"]
    metrics = [("per_db", "Writes/s per database\n(thousands)", lambda v: v / 1e3, "{:,.0f}"),
               ("slowest_db", "Slowest database in any block\n(thousands of writes/s)", lambda v: v / 1e3, "{:,.0f}"),
               ("fallbacks", "Compactions that fell back\nto the CPU (3 blocks)", sum, "{:d}"),
               ("p999_ms", "Slowest 0.1% of writes\n(ms, worst database)", lambda v: v, "{:.0f}")]
    fig, axes = plt.subplots(len(cases), len(metrics), figsize=(DOUBLE_COLUMN, 3.9))
    width = 0.8
    for row_index, (cores, n) in enumerate(cases):
        for col, (key, title, transform, fmt) in enumerate(metrics):
            ax = axes[row_index][col]
            values = [transform(table[(cores, design, n)][key]) for design in arms]
            for x, (design, value) in enumerate(zip(arms, values)):
                ax.bar(x, value, width, color=ARMS[design][1], edgecolor="white", linewidth=0.4, zorder=2)
                ax.annotate(fmt.format(value), (x, value), xytext=(0, 1.5), textcoords="offset points", ha="center",
                            va="bottom", fontsize=6.2, color=INK)
            ax.set_xticks([])
            ax.set_ylim(0, max(max(values) * 1.25, 1))
            finish_axis(ax, "")
            if row_index == 0:
                ax.set_title(title, fontsize=7, color=INK)
            if col == 0:
                ax.set_ylabel(f"{n} databases × {cores} cores", fontsize=7.4, fontweight="bold")
    handles = [plt.Rectangle((0, 0), 1, 1, color=ARMS[d][1]) for d in arms]
    fig.legend(handles, [ARMS[d][0] for d in arms], loc="lower center", ncol=4, bbox_to_anchor=(0.5, 0.0),
               fontsize=7, handlelength=1.2, columnspacing=1.2)
    fig.tight_layout(rect=(0, 0.05, 1, 1))
    save(fig, "fig43_sharing_most")


# ---------------------------------------------------------------------------------------------
# M2: databases on pyke sharing malphite's GPU over RDMA (rdma_campaign.py r9).

def m2() -> dict[tuple, list[dict]]:
    runs = defaultdict(list)
    for record in jsonl(DATA / "m2" / "r9-sharing.jsonl"):
        results = list(record["results"].values())
        if len(results) != record["instances"] or any(code != 0 for code in record["exit_codes"].values()):
            raise SystemExit(f"M2 repetition {record['repetition']} {record['design']} N={record['instances']} failed")
        ops = [r["throughput"]["ops_per_second"] for r in results]
        server = record.get("server_statistics") or {}
        runs[(record["design"], record["instances"])].append({
            "ops": ops, "mean": statistics.mean(ops), "slowest_share": min(ops) / max(ops), "aggregate": sum(ops),
            "fallbacks": sum(fallbacks(r) for r in results),
            "verified": all(r["verification"]["keys"] == r["config"]["records"] for r in results),
            "oom_retries": server.get("merge_oom_retries"), "gpu": None,
            "p999_ms": [r["write_latency"]["p999_ns"] / 1e6 for r in results]})
    return runs


def print_m2(runs) -> dict:
    if not runs:
        return {}
    print("\n## M2: databases on pyke sharing malphite's GPU over RDMA (medians of 3 repetitions)\n")
    print("| Arm | N | Writes/s per database | Share of N = 1 | Slowest / fastest | All databases | CPU fallbacks | "
          "Worst p99.9 (ms) |")
    print("|---|---|---|---|---|---|---|---|")
    table = {}
    for design in ("native", "service-fifo", "service-fair"):
        for n in sorted(n for d, n in runs if d == design):
            r = row(runs[(design, n)])
            r["share"] = r["per_db"] / row(runs[(design, 1)])["per_db"] if (design, 1) in runs else float("nan")
            table[(design, n)] = r
            print(f"| {ARMS[design][0]} | {n} | {r['per_db'] / 1e3:,.0f}k | {r['share']:.2f} | {r['slowest_share']:.2f} | "
                  f"{r['aggregate'] / 1e6:.2f}M | {r['fallbacks']} | {r['p999_ms']:.1f} |")
    return table


def figure_remote(table) -> None:
    if not table:
        return
    fig, ax = plt.subplots(figsize=(DOUBLE_COLUMN * 0.6, 2.6))
    counts = sorted({n for _, n in table})
    for design in ("native", "service-fifo", "service-fair"):
        label, colour, marker = ARMS[design]
        points = [(n, table[(design, n)]["per_db"] / 1e3) for n in counts if (design, n) in table]
        xs, ys = zip(*points)
        ax.plot(xs, ys, "--" if design == "native" else "-", color=colour, marker=marker, markersize=4, linewidth=1.3,
                label=label.replace("Shared GPU service", "GPU service on malphite"), zorder=3)
    for n in counts:
        if ("native", n) in table and ("service-fair", n) in table:
            gain = table[("service-fair", n)]["per_db"] / table[("native", n)]["per_db"]
            ax.annotate(f"{gain:.2f}×", (n, table[("service-fair", n)]["per_db"] / 1e3), xytext=(0, 5),
                        textcoords="offset points", ha="center", va="bottom", fontsize=7, fontweight="bold", color=INK)
    ax.set_xticks(counts)
    ax.set_xlabel("Databases on pyke (4 cores each)")
    ax.set_ylim(0, None)
    finish_axis(ax, "Writes/s per database (thousands)")
    ax.legend(fontsize=6.8, loc="lower left", frameon=False)
    fig.tight_layout()
    save(fig, "fig44_sharing_remote")


# ---------------------------------------------------------------------------------------------
# M3 and M4: one heavy tenant and three light tenants through one engine.

def m3() -> dict[str, list[dict]]:
    runs = defaultdict(list)
    for record in jsonl(DATA / "m3" / "results.jsonl"):
        trial = record["trial"]
        if record["exit_code"] != 0:
            raise SystemExit(f"M3 trial {record['index']} failed")
        results = databases(record)
        key = "solo" if trial["role"] == "light-solo" else trial["scheduler"]
        light = results if key == "solo" else results[1:]
        runs[key].append({"heavy": None if key == "solo" else results[0]["throughput"]["ops_per_second"],
                          "light_p999_ms": [r["write_latency"]["p999_ns"] / 1e6 for r in light],
                          "light_p99_ms": [r["write_latency"]["p99_ns"] / 1e6 for r in light],
                          "light_ops": [r["throughput"]["ops_per_second"] for r in light],
                          "lag_ms": [r["open_loop"]["max_schedule_lag_ms"] for r in light],
                          "tenants": (record.get("server_statistics") or {}).get("scheduler", {}).get("tenants", {})})
    return runs


def print_m3(runs) -> dict:
    if not runs:
        return {}
    print("\n## M3: one heavy and three light tenants through one engine (medians of 3 blocks)\n")
    print("| Policy | Light p99.9 (ms, worst of 3) | × solo | Light p99 (ms) | Light writes/s | Heavy writes/s | "
          "Light mean wait per job (ms) | Heavy mean wait per job (ms) |")
    print("|---|---|---|---|---|---|---|---|")
    solo = median([max(r["light_p999_ms"]) for r in runs.get("solo", [])])
    table = {}
    for key in ("solo", "fifo", "fair", "stall"):
        if key not in runs:
            continue
        rs = runs[key]
        p999 = median([max(r["light_p999_ms"]) for r in rs])
        waits = {"light": [], "heavy": []}
        for r in rs:
            for tenant, v in r["tenants"].items():
                waits["heavy" if tenant == "db0" and key != "solo" else "light"].append(v["wait_ns"] / max(v["jobs"], 1) / 1e6)
        table[key] = {"p999": p999, "ratio": p999 / solo, "heavy": median([r["heavy"] for r in rs])}
        heavy = table[key]["heavy"]
        print(f"| {key} | {p999:.1f} | {p999 / solo:.2f} | {median([max(r['light_p99_ms']) for r in rs]):.1f} | "
              f"{median([statistics.mean(r['light_ops']) for r in rs]) / 1e3:,.0f}k | "
              f"{'' if heavy != heavy else f'{heavy / 1e3:,.0f}k'} | {median(waits['light']):.0f} | "
              f"{median(waits['heavy']) if waits['heavy'] else float('nan'):.0f} |")
    return table


def figure_isolation(table) -> None:
    if not table:
        return
    keys = [k for k in ("solo", "fifo", "fair", "stall") if k in table]
    labels = {"solo": "Light tenant\nalone", "fifo": "FIFO", "fair": "Fair", "stall": "Stalls first"}
    colours = {"solo": "#4d4d4d", "fifo": ARMS["service-fifo"][1], "fair": ARMS["service-fair"][1], "stall": "#CC79A7"}
    fig, ax = plt.subplots(figsize=(DOUBLE_COLUMN * 0.55, 2.5))
    for x, key in enumerate(keys):
        value = table[key]["p999"]
        ax.bar(x, value, 0.7, color=colours[key], edgecolor="white", linewidth=0.4, zorder=2)
        text = f"{value:.0f} ms" if key == "solo" else f"{value:.0f} ms\n{table[key]['ratio']:.1f}× alone"
        ax.annotate(text, (x, value), xytext=(0, 1.5), textcoords="offset points", ha="center", va="bottom",
                    fontsize=6.4, color=INK)
    ax.set_xticks(range(len(keys)), [labels[k] for k in keys], fontsize=7)
    ax.set_ylim(0, max(t["p999"] for t in table.values()) * 1.35)
    finish_axis(ax, "Light tenants: slowest 0.1% of writes (ms)")
    fig.tight_layout()
    save(fig, "fig45_isolation")


def m4() -> dict[str, list[dict]]:
    runs = defaultdict(list)
    for record in jsonl(DATA / "m4" / "results.jsonl"):
        if record["exit_code"] != 0:
            raise SystemExit(f"M4 trial {record['index']} failed")
        runs[record["trial"]["scheduler"]].append(databases(record))
    return runs


def per_second(result: dict, origin: float) -> list[tuple[float, float]]:
    start = result["ingest_and_compact"]["start_unix"]
    return [(start - origin + point["second"], point["ops"]) for point in result["timeline"]]


def figure_burst(runs) -> None:
    if not runs:
        return
    policies = [p for p in ("fifo", "fair") if p in runs]
    fig, axes = plt.subplots(1, len(policies), figsize=(DOUBLE_COLUMN, 2.4), sharey=True)
    axes = axes if len(policies) > 1 else [axes]
    print("\n## M4: light tenants' writes/s before, during and after the heavy tenant (median of blocks)\n")
    print("| Policy | Light writes/s before | during | after |")
    print("|---|---|---|---|")
    for ax, policy in zip(axes, policies):
        blocks = runs[policy]
        results = blocks[0]
        origin = min(r["ingest_and_compact"]["start_unix"] for r in results)
        heavy = per_second(results[0], origin)
        light = [per_second(r, origin) for r in results[1:]]
        ax.plot(*zip(*heavy), color=INK, linewidth=1.0, label="Heavy tenant")
        for index, series in enumerate(light):
            ax.plot(*zip(*series), color=ARMS[f"service-{policy}"][1], linewidth=0.8, alpha=0.85,
                    label="Light tenants" if index == 0 else None)
        ax.set_title("FIFO" if policy == "fifo" else "Fair", fontsize=8, color=INK)
        ax.set_xlabel("Seconds")
        finish_axis(ax, "Writes/s" if ax is axes[0] else "")
        phases = {"before": [], "during": [], "after": []}
        for results in blocks:
            origin = min(r["ingest_and_compact"]["start_unix"] for r in results)
            h_start = results[0]["ingest_and_compact"]["start_unix"] - origin
            h_end = results[0]["ingest_and_compact"]["end_unix"] - origin
            for r in results[1:]:
                for t, ops in per_second(r, origin):
                    phase = "before" if t < h_start else "during" if t < h_end else "after"
                    phases[phase].append(ops)
        print(f"| {policy} | {median(phases['before']) / 1e3:,.0f}k | {median(phases['during']) / 1e3:,.0f}k | "
              f"{median(phases['after']) / 1e3:,.0f}k |")
    axes[0].legend(fontsize=6.8, frameon=False, loc="upper right")
    fig.tight_layout()
    save(fig, "fig46_burst")


def figure_capacity(summary) -> None:
    fig, ax = plt.subplots(figsize=(DOUBLE_COLUMN * 0.5, 2.3))
    ks = sorted(summary)
    values = [summary[k]["gib_s"] for k in ks]
    ax.plot(ks, values, "-o", color=ARMS["service-fair"][1], markersize=4, linewidth=1.3, zorder=3)
    for k, v in zip(ks, values):
        ax.annotate(f"{v:.2f}", (k, v), xytext=(0, 4), textcoords="offset points", ha="center", fontsize=6.4, color=INK)
    ax.set_xticks(ks)
    ax.set_xlabel("Engines in the service")
    ax.set_ylim(0, max(values) * 1.25)
    finish_axis(ax, "Compaction input (GiB/s)")
    fig.tight_layout()
    save(fig, "fig47_service_capacity")


# ---------------------------------------------------------------------------------------------

def hypotheses(local: dict, remote: dict, isolation: dict, local_runs, remote_runs) -> None:
    print("\n## Hypotheses\n")
    service = [r for (c, d, n), runs in local_runs.items() if d.startswith("service") for r in runs]
    service += [r for (d, n), runs in remote_runs.items() if d.startswith("service") for r in runs]
    total = sum(r["fallbacks"] for r in service)
    print(f"- X1: {total} CPU fallbacks in {len(service)} service runs "
          f"({'supported' if total == 0 else 'not supported'}).")
    fair, mps = local[(8, "service-fair", 4)], local[(8, "mps", 4)]
    print(f"- X2: service-fair 8 cores N = 4 keeps {fair['share']:.2f} of its N = 1 (threshold 0.81: "
          f"{'met' if fair['share'] >= 0.81 else 'not met'}); per database {fair['per_db'] / 1e3:,.0f}k against MPS "
          f"{mps['per_db'] / 1e3:,.0f}k ({'above' if fair['per_db'] > mps['per_db'] else 'not above'}); share "
          f"{fair['share']:.2f} against MPS {mps['share']:.2f}.")
    if ("service-fair", 4) in remote:
        share = remote[("service-fair", 4)]["share"]
        print(f"- X3: remote 4 cores N = 4 keeps {share:.2f} of N = 1 (threshold 0.80: "
              f"{'supported' if share >= 0.80 else 'not supported'}).")
    if isolation:
        ok = {k: isolation[k]["ratio"] <= 1.5 for k in ("fifo", "fair", "stall") if k in isolation}
        print(f"- X4: light p99.9 / solo: " + ", ".join(f"{k} {isolation[k]['ratio']:.2f}" for k in ok)
              + f" ({'supported' if ok.get('fair') and ok.get('stall') and not ok.get('fifo') else 'not supported'}).")
    print(f"- X5: service-fair 8 cores N = 4 per database {fair['per_db'] / 1e3:,.0f}k against the registered "
          f"1,125k ({'supported' if fair['per_db'] >= 1_125_000 else 'not supported'}); cost "
          f"${fair['cost']:.3f} per billion writes against RocksDB N = 1 ${local[(8, 'native', 1)]['cost']:.3f} "
          f"and N = 4 ${local[(8, 'native', 4)]['cost']:.3f}.")


def main() -> int:
    capacity = m0()
    if capacity:
        print("## M0: service capacity against engines (median of 3)\n")
        print("| Engines | Input GiB/s | Median job (s) | Failures | Out-of-memory re-queues | GPU memory (GiB) |")
        print("|---|---|---|---|---|---|")
        for k, s in capacity.items():
            print(f"| {k} | {s['gib_s']:.2f} | {s['p50_s']:.2f} | {s['failures']} | {s['oom_retries']} | "
                  f"{s['gpu_memory_gib']:.1f} |")
        print(f"\nChosen K (smallest within 95% of the best): {chosen_engines(capacity)}")
        figure_capacity(capacity)
    local_runs = m1()
    local = print_m1(local_runs)
    queueing(local_runs)
    figure_local(local)
    figure_local_most(local)
    remote_runs = m2()
    remote = print_m2(remote_runs)
    figure_remote(remote)
    isolation = print_m3(m3())
    figure_isolation(isolation)
    figure_burst(m4())
    hypotheses(local, remote, isolation, local_runs, remote_runs)
    return 0


if __name__ == "__main__":
    sys.exit(main())
