#!/usr/bin/env python3
"""Figures and summary for the v3 campaigns (PREREGISTRATION.md, "Methodology v3").

  python3 publication/make_figures_v3.py

Reads publication/data/v3/ (isolated/*.jsonl from v3_isolated.sh; v3-headline/,
v3-bottomzstd/ and v3-openloop/ from system_campaign.py) and writes
figures/fig17_v3_compaction, fig18_v3_cores, fig19_v3_database, fig20_v3_tail and
data/v3/summary.md. Runs that failed their correctness check are refused.
"""

from __future__ import annotations

import json
import statistics
import sys
from pathlib import Path

import matplotlib.pyplot as plt

sys.path.insert(0, str(Path(__file__).resolve().parent))
from make_figures import (DOUBLE_COLUMN, FULL, INK, NATIVE, STYLE, bar, design_legend,  # noqa: E402
                          finish_axis, panel_label, save)

HERE = Path(__file__).resolve().parent
V3 = HERE / "data" / "v3"
OURS = "GPU offload (ours)"
LABELS = {NATIVE: "Native RocksDB (tuned)", FULL: OURS}
DESIGN = {"native_cpu": NATIVE, "cuda_merge": FULL, "native": NATIVE, "full": FULL}
X1 = [("x1-v32-lz4", "32 B\nLZ4"), ("x1-v128-lz4", "128 B\nLZ4"), ("x1-v1024-lz4", "1 KB\nLZ4"),
      ("x1-v128-zstd", "128 B\nZSTD"), ("x1-v128-none", "128 B\nnone"), ("x1-v128-lz4-del0.1", "128 B LZ4\n10% del.")]
X2 = [("x2-v128-lz4", "128 B LZ4"), ("x2-v128-zstd", "128 B ZSTD")]


def median(values):
    return statistics.median(values) if values else float("nan")


def load_isolated(tag):
    """{design: {"wall": [...], "cpu": [...], "gib": [...]}} over warm repetitions."""
    path = V3 / "isolated" / f"{tag}.jsonl"
    if not path.exists():
        return None
    runs = {}
    for line in path.read_text().splitlines():
        record = json.loads(line)
        if not record["matches_reference"]:
            raise SystemExit(f"{tag}: {record['mode']} differs from native")
        if record["repetition"] == 0:
            continue
        gib = sum(job["input_bytes"] for job in record["jobs"]) / 2**30
        entry = runs.setdefault(DESIGN[record["mode"]], {"wall": [], "cpu": [], "gib": []})
        entry["wall"].append(record["wall_seconds"])
        entry["cpu"].append(record["process_cpu_seconds"])
        entry["gib"].append(gib)
    return runs


def cpu_per_gib(entry):
    return [c / g for c, g in zip(entry["cpu"], entry["gib"])]


def gib_per_second(entry):
    return [g / w for g, w in zip(entry["gib"], entry["wall"])]


def load_campaign(name):
    """{(design, key): [result, ...]} for a system_campaign.py directory; key is cores or rate."""
    path = V3 / name / "results.jsonl"
    if not path.exists():
        return {}
    runs = {}
    for line in path.read_text().splitlines():
        record = json.loads(line)
        if record.get("exit_code") != 0:
            continue
        result = record["result"]
        if "keys" not in (result.get("verification") or {}):
            raise SystemExit(f"{name}: trial {record['index']} failed verification")
        flags = record["flags"]
        key = flags.get("rate") or record["trial"]["cores"]
        runs.setdefault((DESIGN[record["trial"]["design"]], key), []).append(result)
    return runs


def window_ops(result):
    return result["throughput"]["ops_per_second"]


def cpu_us(result):
    return result["accounting"]["window"]["cores"] * 1e6 / window_ops(result)


def ratio_label(ax, x, top, text):
    ax.annotate(text, (x, top), xytext=(0, 2), textcoords="offset points", ha="center", va="bottom",
                fontsize=7, fontweight="bold", color=INK)


def figure_compaction(isolated):
    """(a) time of one compaction job and (b) host CPU per GiB, native against ours, log scale."""
    tags = [(tag, label) for tag, label in X1 if isolated.get(tag)]
    if not tags:
        return
    fig, axes = plt.subplots(1, 2, figsize=(DOUBLE_COLUMN, 2.5))
    metrics = [("Time of one compaction (s)", lambda e: e["wall"], "{:.1f}", "faster"),
               ("Host CPU (s per GiB of input)", cpu_per_gib, "{:.2f}", "less")]
    width = 0.38
    for ax, letter, (ylabel, metric, fmt, word) in zip(axes, "ab", metrics):
        for index, (tag, _) in enumerate(tags):
            native = median(metric(isolated[tag][NATIVE]))
            ours = median(metric(isolated[tag][FULL]))
            bar(ax, [index - width / 2], [native], width * 0.95, NATIVE)
            bar(ax, [index + width / 2], [ours], width * 0.95, FULL)
            ax.annotate(fmt.format(native), (index - width / 2, native), xytext=(0, 1.5), textcoords="offset points",
                        ha="center", va="bottom", fontsize=5.6, color=INK)
            ax.annotate(fmt.format(ours), (index + width / 2, ours), xytext=(0, 1.5), textcoords="offset points",
                        ha="center", va="bottom", fontsize=5.6, color=INK)
            ratio_label(ax, index, native * 1.9, f"{native / ours:.1f}×")
        ax.set_yscale("log")
        ax.set_xticks(range(len(tags)), [label for _, label in tags])
        low = min(median(metric(isolated[t][FULL])) for t, _ in tags)
        high = max(median(metric(isolated[t][NATIVE])) for t, _ in tags)
        ax.set_ylim(low / 2.5, high * 6)
        finish_axis(ax, ylabel)
        panel_label(ax, letter)
    fig.tight_layout(w_pad=2.5, rect=(0, 0, 1, 0.87))
    design_legend(fig, (NATIVE, FULL), y=0.87, labels=LABELS)
    save(fig, "fig17_v3_compaction")


def figure_cores(isolated):
    """Compaction throughput with 8 concurrent subcompactions, and native cores needed to match the GPU."""
    rows = []
    for tag, label in X2:
        x1_tag = tag.replace("x2", "x1")
        if not isolated.get(tag) or not isolated.get(x1_tag):
            continue
        per_core = median(gib_per_second(isolated[x1_tag][NATIVE]))
        rows.append((label, per_core, median(gib_per_second(isolated[tag][NATIVE])),
                     median(gib_per_second(isolated[tag][FULL])),
                     median([c / w for c, w in zip(isolated[tag][FULL]["cpu"], isolated[tag][FULL]["wall"])]),
                     median([c / w for c, w in zip(isolated[tag][NATIVE]["cpu"], isolated[tag][NATIVE]["wall"])])))
    if not rows:
        return
    fig, ax = plt.subplots(figsize=(4.6, 2.5))
    width = 0.38
    for index, (label, per_core, native8, gpu, gpu_cores, native_cores) in enumerate(rows):
        rects = bar(ax, [index - width / 2], [native8], width * 0.95, NATIVE)
        ax.annotate(f"{native8:.2f}\n({native_cores:.1f} cores)", (index - width / 2, native8), xytext=(0, 1.5),
                    textcoords="offset points", ha="center", va="bottom", fontsize=5.6, color=INK)
        bar(ax, [index + width / 2], [gpu], width * 0.95, FULL)
        ax.annotate(f"{gpu:.2f}\n(GPU + {gpu_cores:.1f} cores)", (index + width / 2, gpu), xytext=(0, 1.5),
                    textcoords="offset points", ha="center", va="bottom", fontsize=5.6, color=INK)
        ratio_label(ax, index, max(gpu, native8) * 1.32, f"GPU matches {gpu / per_core:.0f} native cores")
        del rects
    ax.set_xticks(range(len(rows)), [r[0] for r in rows])
    ax.set_ylim(0, max(max(r[2], r[3]) for r in rows) * 1.6)
    finish_axis(ax, "Compaction throughput (GiB/s)")
    fig.tight_layout(rect=(0, 0, 1, 0.86))
    design_legend(fig, (NATIVE, FULL), y=0.86, labels={NATIVE: "Native, 8 cores", FULL: "GPU offload (ours)"})
    save(fig, "fig18_v3_cores")


def figure_database(headline, bottom):
    """Whole database: (a) throughput and (b) CPU per write, by cores and codec setup."""
    groups = [(f"{c} cores\nLZ4", headline, c) for c in (4, 8, 16)] + \
             [(f"{c} cores\nZSTD bottom", bottom, c) for c in (4, 8)]
    groups = [g for g in groups if g[1].get((NATIVE, g[2])) and g[1].get((FULL, g[2]))]
    if not groups:
        return
    fig, axes = plt.subplots(1, 2, figsize=(DOUBLE_COLUMN, 2.5))
    width = 0.38
    for ax, letter, (ylabel, metric, scale, fmt) in zip(
            axes, "ab", [("Throughput (M writes/s)", window_ops, 1e-6, "{:.2f}"),
                         ("CPU per write (µs)", cpu_us, 1, "{:.1f}")]):
        for index, (_, runs, cores) in enumerate(groups):
            native = median([metric(r) for r in runs[(NATIVE, cores)]]) * scale
            ours = median([metric(r) for r in runs[(FULL, cores)]]) * scale
            for value, design, offset in ((native, NATIVE, -width / 2), (ours, FULL, width / 2)):
                bar(ax, [index + offset], [value], width * 0.95, design)
                ax.annotate(fmt.format(value), (index + offset, value), xytext=(0, 1.5), textcoords="offset points",
                            ha="center", va="bottom", fontsize=5.6, color=INK)
            better = ours / native if letter == "a" else native / ours
            ratio_label(ax, index, max(native, ours) * 1.17, f"{better:.2f}×")
        ax.set_xticks(range(len(groups)), [g[0] for g in groups], fontsize=6.4)
        top = max(max(median([metric(r) for r in runs[(d, c)]]) * scale for d in (NATIVE, FULL)) for _, runs, c in groups)
        ax.set_ylim(0, top * 1.38)
        finish_axis(ax, ylabel)
        panel_label(ax, letter)
    fig.tight_layout(w_pad=2.5, rect=(0, 0, 1, 0.87))
    design_legend(fig, (NATIVE, FULL), y=0.87, labels=LABELS)
    save(fig, "fig19_v3_database")


def figure_tail(openloop):
    """Write latency percentiles at fixed offered rates (open loop, 8 cores)."""
    rates = sorted({rate for (_, rate) in openloop})
    if not rates:
        return
    fig, axes = plt.subplots(1, 2, figsize=(DOUBLE_COLUMN, 2.4))
    for ax, letter, (percentile, label) in zip(axes, "ab", [("p99_ns", "p99"), ("p999_ns", "p99.9")]):
        for design in (NATIVE, FULL):
            xs = [r for r in rates if openloop.get((design, r))]
            ys = [median([res["write_latency"][percentile] / 1e6 for res in openloop[(design, r)]]) for r in xs]
            style = STYLE[design]
            ax.plot([x / 1000 for x in xs], ys, marker="o", markersize=3.5, color=style["color"], linewidth=1.2,
                    label=f"{LABELS[design]}, median")
            # Every run, since one rare pause can set a run's p99.9.
            points = [(x / 1000, res["write_latency"][percentile] / 1e6) for x in xs for res in openloop[(design, x)]]
            ax.scatter([p[0] for p in points], [p[1] for p in points], s=9, marker="x", color=style["color"],
                       linewidths=0.8, zorder=3, label=f"{LABELS[design]}, each run")
            for x, y in zip(xs, ys):
                ax.annotate(f"{y:.1f}", (x / 1000, y), xytext=(0, 3), textcoords="offset points", ha="center",
                            fontsize=5.6, color=INK)
        ax.set_yscale("log")
        ax.set_xlabel("Offered load (k writes/s)")
        finish_axis(ax, f"{label} write latency (ms)")
        panel_label(ax, letter)
    axes[0].legend(frameon=False, fontsize=6.5)
    fig.tight_layout(w_pad=2.5)
    save(fig, "fig20_v3_tail")


def write_summary(isolated, headline, bottom, openloop):
    lines = ["# v3 results", "", "Generated by `make_figures_v3.py` from `data/v3/`. Medians over warm repetitions "
             "(isolated) or blocks (whole database); every run matched native RocksDB.", ""]
    lines += ["## X1: isolated compaction, one job", "",
              "| Workload | Native s | Ours s | Faster | Native CPU-s/GiB | Ours CPU-s/GiB | Less CPU |",
              "|---|---|---|---|---|---|---|"]
    for tag, label in X1:
        runs = isolated.get(tag)
        if not runs:
            continue
        nw, ow = median(runs[NATIVE]["wall"]), median(runs[FULL]["wall"])
        nc, oc = median(cpu_per_gib(runs[NATIVE])), median(cpu_per_gib(runs[FULL]))
        lines.append(f"| {label.replace(chr(10), ' ')} | {nw:.2f} | {ow:.3f} | {nw / ow:.1f}× | {nc:.2f} | {oc:.2f} | {nc / oc:.1f}× |")
    lines += ["", "## X2: concurrent compaction (8 subcompactions, 8 cores)", "",
              "| Workload | Native GiB/s (cores busy) | Ours GiB/s (cores busy) | Native GiB/s per core (X1) | Native cores to match the GPU |",
              "|---|---|---|---|---|"]
    for tag, label in X2:
        runs, single = isolated.get(tag), isolated.get(tag.replace("x2", "x1"))
        if not runs or not single:
            continue
        busy = lambda e: median([c / w for c, w in zip(e["cpu"], e["wall"])])
        per_core = median(gib_per_second(single[NATIVE]))
        gpu = median(gib_per_second(runs[FULL]))
        lines.append(f"| {label} | {median(gib_per_second(runs[NATIVE])):.2f} ({busy(runs[NATIVE]):.1f}) | "
                     f"{gpu:.2f} ({busy(runs[FULL]):.1f}) | {per_core:.3f} | {gpu / per_core:.1f} |")
    for title, runs, keys in (("X3: whole database, LZ4", headline, (4, 8, 16)),
                              ("X4: whole database, LZ4 above, ZSTD bottom", bottom, (4, 8))):
        if not runs:
            continue
        lines += ["", f"## {title}", "",
                  "| Cores | Design | Runs | M writes/s | vs native | CPU µs/write | vs native | Catch-up-inclusive M writes/s | p99.9 ms | Stall % |",
                  "|---|---|---|---|---|---|---|---|---|---|"]
        for cores in keys:
            base = runs.get((NATIVE, cores))
            for design in (NATIVE, FULL):
                results = runs.get((design, cores))
                if not results or not base:
                    continue
                ops, cpu = median([window_ops(r) for r in results]), median([cpu_us(r) for r in results])
                bops, bcpu = median([window_ops(r) for r in base]), median([cpu_us(r) for r in base])
                lines.append(f"| {cores} | {design} | {len(results)} | {ops / 1e6:.3f} | {ops / bops:.2f}× | {cpu:.2f} | "
                             f"{bcpu / cpu:.2f}× | {median([r['ingest_and_compact']['ops_per_second'] for r in results]) / 1e6:.3f} | "
                             f"{median([r['write_latency']['p999_ns'] for r in results]) / 1e6:.1f} | "
                             f"{100 * median([r['stall_fraction'] for r in results]):.1f} |")
    if openloop:
        lines += ["", "## X5: open-loop write latency, 8 cores, LZ4", "",
                  "| Offered k writes/s | Design | Runs | Achieved k writes/s | Max lag ms | p50 ms | p99 ms | p99.9 ms | CPU µs/write |",
                  "|---|---|---|---|---|---|---|---|---|"]
        for rate in sorted({r for _, r in openloop}):
            for design in (NATIVE, FULL):
                results = openloop.get((design, rate))
                if not results:
                    continue
                pct = lambda key: median([r["write_latency"][key] for r in results]) / 1e6
                lines.append(f"| {rate / 1000:.0f} | {design} | {len(results)} | {median([window_ops(r) for r in results]) / 1000:.0f} | "
                             f"{max(r['open_loop']['max_schedule_lag_ms'] for r in results):.1f} | {pct('p50_ns'):.2f} | "
                             f"{pct('p99_ns'):.2f} | {pct('p999_ns'):.2f} | {median([cpu_us(r) for r in results]):.2f} |")
    (V3 / "summary.md").write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    isolated = {tag: load_isolated(tag) for tag, _ in X1 + X2}
    headline, bottom, openloop = load_campaign("v3-headline"), load_campaign("v3-bottomzstd"), load_campaign("v3-openloop")
    write_summary(isolated, headline, bottom, openloop)
    figure_compaction(isolated)
    figure_cores(isolated)
    figure_database(headline, bottom)
    figure_tail(openloop)
    print((V3 / "summary.md").read_text())
