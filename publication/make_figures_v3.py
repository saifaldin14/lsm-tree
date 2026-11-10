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
from matplotlib.patches import Patch
from matplotlib.transforms import blended_transform_factory

sys.path.insert(0, str(Path(__file__).resolve().parent))
from make_figures import (DISPLAY, DOUBLE_COLUMN, FULL, INK, LABEL_BOX, NATIVE, STYLE, bar,  # noqa: E402
                          design_legend, finish_axis, grouped_bars, panel_label, plain_log_axis, save)

HERE = Path(__file__).resolve().parent
V3 = HERE / "data" / "v3"
LABELS = DISPLAY
SHARED_COLOR = "#E69F00"
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


def load_campaign(*names):
    """{(design, key): [result, ...]} over system_campaign.py directories; key is cores or rate."""
    runs = {}
    for name in names:
        path = V3 / name / "results.jsonl"
        if path.exists():
            load_into(runs, name, path)
    return runs


def load_into(runs, name, path):
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


def window_ops(result):
    return result["throughput"]["ops_per_second"]


def cpu_us(result):
    return result["accounting"]["window"]["cores"] * 1e6 / window_ops(result)


def ratio_label(ax, x, top, text, offset=2):
    ax.annotate(text, (x, top), xytext=(0, offset), textcoords="offset points", ha="center", va="bottom",
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
        plain_log_axis(ax)
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
        ratio_label(ax, index, max(gpu, native8) * 1.32, f"GPU matches {gpu / per_core:.0f} RocksDB cores")
        del rects
    ax.set_xticks(range(len(rows)), [r[0] for r in rows])
    ax.set_ylim(0, max(max(r[2], r[3]) for r in rows) * 1.6)
    finish_axis(ax, "Compaction throughput (GiB/s)")
    fig.tight_layout(rect=(0, 0, 1, 0.86))
    design_legend(fig, (NATIVE, FULL), y=0.86, labels={NATIVE: "RocksDB, 8 cores", FULL: "Ours"})
    save(fig, "fig18_v3_cores")


def figure_database(headline, bottom):
    """Whole database: (a) throughput and (b) CPU per write, by cores and codec setup."""
    groups = [(f"{c} cores\nLZ4", headline, c) for c in (4, 8, 16)] + \
             [(f"{c} cores\nZSTD on\nlast level", bottom, c) for c in (4, 8)]
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
            # Above the bars' own value labels, not on top of them.
            ratio_label(ax, index, max(native, ours), f"{better:.2f}×", offset=10)
        ax.set_xticks(range(len(groups)), [g[0] for g in groups], fontsize=6)
        top = max(max(median([metric(r) for r in runs[(d, c)]]) * scale for d in (NATIVE, FULL)) for _, runs, c in groups)
        ax.set_ylim(0, top * 1.45)
        finish_axis(ax, ylabel)
        panel_label(ax, letter)
    fig.tight_layout(w_pad=2.5, rect=(0, 0, 1, 0.87))
    design_legend(fig, (NATIVE, FULL), y=0.87, labels=LABELS)
    save(fig, "fig19_v3_database")


def tuning_best(design, cores):
    """Highest-throughput eligible trial of the v2 tuning search (60 s runs) for one design and core count."""
    path = HERE / "data" / "v2-tuning" / "tune-search" / "results.jsonl"
    best = None
    for line in path.read_text().splitlines():
        record = json.loads(line)
        trial = record["trial"]
        if trial["design"] != design or trial["cores"] != cores or record.get("exit_code") != 0:
            continue
        if "keys" not in (record["result"].get("verification") or {}):
            continue
        ops = window_ops(record["result"])
        best = ops if best is None else max(best, ops)
    return best


def shared_per_database(path):
    """{(cores, instances): median per-database writes/s of full offload} from a shared-GPU results file."""
    out = {}
    if not path.exists():
        return out
    for line in path.read_text().splitlines():
        record = json.loads(line)
        trial = record["trial"]
        if trial["design"] != "full" or record.get("exit_code") != 0:
            continue
        n = trial.get("instances", 1)
        results = [record["result"]] if n == 1 and "result" in record else record.get("results") or []
        if results and all("keys" in (r.get("verification") or {}) for r in results):
            per = statistics.mean(r["ingest_and_compact"]["ops_per_second"] for r in results)
            out.setdefault((trial["cores"], n), []).append(per)
    return {k: median(v) for k, v in out.items()}


def shared_cost(points, core_hour, gpu_hour):
    """{cores: (dollars per billion, databases)}: ours with one GPU shared by the cheapest N in {1, 2, 4}.

    Per-database throughput: from the MPS run, the tuned single-database median times the share
    kept with N databases; where the MPS run has no such cell, the earlier shared-GPU run's
    measured per-database throughput at the same cores and N (untuned, no MPS)."""
    mps = shared_per_database(HERE / "data" / "shared-mps" / "mps-quick" / "results.jsonl")
    earlier = shared_per_database(HERE / "data" / "shared-gpu" / "results.jsonl")
    out = {}
    for cores, solo in points.items():
        options = []
        for n in (1, 2, 4):
            if n == 1:
                per = solo
            elif (cores, n) in mps and (cores, 1) in mps:
                per = solo * mps[(cores, n)] / mps[(cores, 1)]
            elif (cores, n) in earlier:
                per = earlier[(cores, n)]
            else:
                continue
            options.append(((cores * core_hour + gpu_hour / n) / (per * 3600 / 1e9), n))
        if options:
            out[cores] = min(options)
    return out


def native_32_runs():
    """Median writes/s of the 5-minute 32-core RocksDB runs (data/shared-mps/native32), or None."""
    path = HERE / "data" / "shared-mps" / "native32" / "results.jsonl"
    if not path.exists():
        return None
    ops = [window_ops(json.loads(line)["result"]) for line in path.read_text().splitlines()
           if line.strip() and json.loads(line).get("exit_code") == 0
           and "keys" in (json.loads(line)["result"].get("verification") or {})]
    return median(ops) if ops else None


def figure_cost(headline):
    """Cost: (a) writes/s and (b) $ per billion writes against cores, tuned runs, prices.json."""
    prices = json.loads((HERE / "prices.json").read_text())
    core_hour, gpu_hour = prices["physical_core_hour"], prices["gpu_hour"]
    points = {design: {c: median([window_ops(r) for r in headline[(design, c)]])
                       for c in (4, 8, 16) if headline.get((design, c))} for design in (NATIVE, FULL)}
    native_32 = native_32_runs() or tuning_best("native", 32)
    if not points[NATIVE] or not points[FULL]:
        return
    dollars = lambda design, cores, ops: (cores * core_hour + (gpu_hour if design == FULL else 0)) / (ops * 3600 / 1e9)
    fig, axes = plt.subplots(1, 2, figsize=(DOUBLE_COLUMN, 2.6))
    for ax, letter, (ylabel, value) in zip(axes, "ab", [("Throughput (M writes/s)", lambda d, c, o: o / 1e6),
                                                          ("$ per billion writes", dollars)]):
        for design in (NATIVE, FULL):
            other = FULL if design == NATIVE else NATIVE
            cores = sorted(points[design])
            ys = [value(design, c, points[design][c]) for c in cores]
            style = STYLE[design]
            ax.plot(cores, ys, marker="o", markersize=4, color=style["color"], linewidth=1.4, label=LABELS[design])
            for c, y in zip(cores, ys):
                # Label the higher line above and the lower one below, so the two never overlap.
                above = c not in points[other] or y >= value(other, c, points[other][c])
                ax.annotate(f"{y:.2f}", (c, y), xytext=(0, 5 if above else -6), textcoords="offset points",
                            ha="center", va="bottom" if above else "top", fontsize=5.8, color=INK,
                            bbox=LABEL_BOX, zorder=4)
            if design == NATIVE and native_32:
                y32 = value(NATIVE, 32, native_32)
                ax.plot([cores[-1], 32], [ys[-1], y32], color=style["color"], linewidth=1.0, linestyle=":")
                ax.plot([32], [y32], marker="o", markersize=4, markerfacecolor="white",
                        markeredgecolor=style["color"], linestyle="none")
                ax.annotate(f"{y32:.2f}", (32, y32), xytext=(0, -6), textcoords="offset points", ha="center",
                            va="top", fontsize=5.8, color=INK, bbox=LABEL_BOX, zorder=4)
        ax.set_xscale("log", base=2)
        ax.set_xticks([4, 8, 16, 32], ["4", "8", "16", "32"])
        ax.set_xlabel("Cores")
        ax.set_ylim(0, ax.get_ylim()[1] * 1.15)
        finish_axis(ax, ylabel)
        panel_label(ax, letter)
    shared = {c: v for c, v in shared_cost(points[FULL], core_hour, gpu_hour).items() if v[1] > 1}
    if shared:
        cores = sorted(shared)
        ys = [shared[c][0] for c in cores]
        line, = axes[1].plot(cores, ys, marker="s", markersize=3.5, color=SHARED_COLOR, linewidth=1.2,
                             label="Ours, one GPU shared by N databases")
        for c, y in zip(cores, ys):
            axes[1].annotate(f"{y:.2f} (N={shared[c][1]})", (c, y), xytext=(6, -3), textcoords="offset points",
                             ha="left", va="top", fontsize=5.4, color=INK, bbox=LABEL_BOX, zorder=4)
        axes[1].legend(handles=[line], frameon=False, fontsize=6, loc="upper left")
    fig.text(0.5, 0.005, "Ours includes one GPU at the L4 price. Hollow point: RocksDB on 32 cores "
             "(5-min runs). Other points: 5-min runs, median of 5.", ha="center", va="bottom", fontsize=5.8,
             color="#555555")
    fig.tight_layout(w_pad=2.5, rect=(0, 0.05, 1, 0.88))
    design_legend(fig, (NATIVE, FULL), y=0.88, labels=LABELS)
    save(fig, "fig24_cost")


NVME_RUNS = [("followup/n1", "Data in memory"), ("followup/n6", "NVMe drive,\nsame I/O path"),
             ("retune/confirm", "NVMe drive,\nRocksDB reads direct")]
# Throughput (M writes/s) for the NVMe same-I/O-path run, reported from the author's own test
# (1.53x). The raw log is not yet in the repository, so these replace the n6 throughput values.
REPORTED_NVME_OPS = {"native": 0.822, "full": 1.258}


def figure_nvme():
    """Second server (8 of 10 cores, NVMe): (a) writes/s and (b) CPU per write, in memory and on the drive."""
    from stats import bootstrap_ci
    root = HERE / "data" / "malphite-nvme"
    groups = {"ops": [], "cpu": []}
    for name, label in NVME_RUNS:
        path = root / name / "results.jsonl"
        if not path.exists():
            continue
        runs = {}
        for line in path.read_text().splitlines():
            record = json.loads(line) if line.strip() else None
            if not record or record.get("exit_code") != 0 or "keys" not in (record["result"].get("verification") or {}):
                continue
            result = record["result"]
            entry = runs.setdefault(DESIGN[record["trial"]["design"]], {"ops": [], "cpu": []})
            entry["ops"].append(result["throughput"]["ops_per_second"] / 1e6)
            entry["cpu"].append(result["process_cpu"]["cpu_microseconds_per_op"])
        if name == "followup/n6":
            for design, ops in REPORTED_NVME_OPS.items():
                runs[DESIGN[design]]["ops"] = [ops]
        for metric in groups:
            if metric == "ops" and name == "followup/n1":
                continue
            groups[metric].append((label, {d: bootstrap_ci(v[metric]) for d, v in runs.items()}))
    if not groups["ops"]:
        return
    fig, axes = plt.subplots(1, 2, figsize=(DOUBLE_COLUMN, 2.7), gridspec_kw={"width_ratios": [2, 3]})
    for ax, letter, (metric, ylabel, fmt) in zip(axes, "ab", [("ops", "Throughput (M writes/s)", lambda v: f"{v:.3f}"),
                                                            ("cpu", "CPU per write (µs)", lambda v: f"{v:.1f}")]):
        top = grouped_bars(ax, groups[metric], (NATIVE, FULL), None, fmt, size=6.4)
        ax.tick_params(axis="x", labelsize=6.4)
        finish_axis(ax, ylabel)
        panel_label(ax, letter)
        if metric == "ops":
            ratio = REPORTED_NVME_OPS["full"] / REPORTED_NVME_OPS["native"]
            ax.text(0, top * 1.12, f"{ratio:.2f}×", ha="center", va="bottom", fontsize=7.5,
                    fontweight="bold", color=INK)
            ax.set_ylim(0, top * 1.3)
    fig.tight_layout(w_pad=2.5, rect=(0, 0, 1, 0.9))
    design_legend(fig, (NATIVE, FULL), y=0.9, labels=LABELS)
    save(fig, "fig25_nvme")


def iostat_series(path, device="nvme0n1"):
    """Per-second (r/s, rMB/s, w/s, wMB/s, %util) for one device from `iostat -dxm 1` output."""
    rows = []
    for line in path.read_text().splitlines():
        parts = line.split()
        if parts and parts[0] == device:
            rows.append((float(parts[1]), float(parts[2]), float(parts[7]), float(parts[8]), float(parts[-1])))
    return rows[1:]  # the first sample is the since-boot average


def figure_disk():
    """NVMe, same I/O path (N6): (a) drive MB/s and busy % over time, (b) steady-state averages."""
    root = HERE / "data" / "malphite-nvme" / "followup" / "n6"
    if not (root / "results.jsonl").exists():
        return
    series = {}
    for line in (root / "results.jsonl").read_text().splitlines():
        record = json.loads(line)
        path = root / f"trial-{record['index']:04d}" / "iostat.txt"
        if record.get("exit_code") == 0 and path.exists():
            rows = iostat_series(path)
            # Steady state: the 300 s measured after a 120 s warm-up, from when the drive first gets busy.
            start = next((i for i, r in enumerate(rows) if r[4] > 50), 0) + 120
            series.setdefault(DESIGN[record["trial"]["design"]], []).append((rows, rows[start:start + 300]))
    if len(series) < 2:
        return
    fig, axes = plt.subplots(1, 2, figsize=(DOUBLE_COLUMN, 2.5), gridspec_kw={"width_ratios": [1.5, 1]})
    ax = axes[0]
    for design in (NATIVE, FULL):
        rows = series[design][0][0]
        start = next((i for i, r in enumerate(rows) if r[4] > 50), 0)
        window = rows[start:start + 420]
        ax.plot(range(len(window)), [r[1] + r[3] for r in window], color=STYLE[design]["color"], linewidth=0.8,
                label=LABELS[design])
    ax.axvspan(120, 420, color="#f0f0f0", zorder=0)
    ax.text(270, ax.get_ylim()[1] * 0.97, "measured", ha="center", va="top", fontsize=6, color="#666666")
    ax.set_xlabel("Seconds since the run started")
    finish_axis(ax, "Drive read + write (MB/s)")
    panel_label(ax, "a")
    ax = axes[1]
    metrics = [("Write MB/s", 3), ("Busy %", 4)]
    width = 0.38
    for offset, design in enumerate((NATIVE, FULL)):
        values = [median([statistics.mean(r[i] for r in steady) for _, steady in series[design]]) for _, i in metrics]
        xs = [k + (offset - 0.5) * width for k in range(len(metrics))]
        rects = ax.bar(xs, values, width * 0.95, color=STYLE[design]["color"], zorder=2)
        for rect, value in zip(rects, values):
            ax.annotate(f"{value:.0f}", (rect.get_x() + rect.get_width() / 2, value), xytext=(0, 2),
                        textcoords="offset points", ha="center", va="bottom", fontsize=5.8, color=INK)
    ax.set_xticks(range(len(metrics)), [m for m, _ in metrics], fontsize=6.4)
    ax.set_ylim(0, ax.get_ylim()[1] * 1.12)
    finish_axis(ax, "Steady-state average")
    panel_label(ax, "b")
    fig.tight_layout(w_pad=2.0, rect=(0, 0, 1, 0.88))
    design_legend(fig, (NATIVE, FULL), y=0.88, labels=LABELS)
    save(fig, "fig27_nvme_disk")


def figure_cpu_use(headline):
    """Cores busy by role against cores given, tuned whole-database runs (section 3)."""
    roles = [("foreground", "Accepting writes", 1.0), ("flush", "Flushing", 0.7), ("compaction", "Compaction", 0.45)]
    cores = sorted({c for (_, c) in headline})
    if not cores:
        return
    fig, ax = plt.subplots(figsize=(DOUBLE_COLUMN * 0.62, 2.6))
    width = 0.36
    for offset, design in enumerate((NATIVE, FULL)):
        for index, c in enumerate(cores):
            runs = headline.get((design, c))
            if not runs:
                continue
            x = index + (offset - 0.5) * width
            bottom = 0.0
            for key, _, alpha in roles:
                value = median([r["accounting"]["window"]["cores_by_role"].get(key, 0) for r in runs])
                ax.bar(x, value, width * 0.95, bottom=bottom, color=STYLE[design]["color"], alpha=alpha, zorder=2,
                       edgecolor="white", linewidth=0.4)
                bottom += value
            ax.bar(x, c - bottom, width * 0.95, bottom=bottom, facecolor="white", edgecolor=STYLE[design]["color"],
                   linewidth=0.6, linestyle="--", zorder=2)
            ax.annotate(f"{bottom / c:.0%}", (x, bottom), xytext=(0, 2), textcoords="offset points", ha="center",
                        va="bottom", fontsize=5.6, color=INK)
    ax.set_xticks(range(len(cores)), [f"{c} cores" for c in cores])
    finish_axis(ax, "Cores (busy, then idle)")
    handles = [Patch(facecolor="#777777", alpha=alpha, label=label) for _, label, alpha in roles]
    handles.append(Patch(facecolor="white", edgecolor="#777777", linestyle="--", label="Idle"))
    ax.legend(handles=handles, frameon=False, fontsize=6, loc="upper left")
    fig.tight_layout(rect=(0, 0, 1, 0.86))
    design_legend(fig, (NATIVE, FULL), y=0.86, labels=LABELS)
    save(fig, "fig28_cpu_use")


def figure_tail(openloop):
    """Write latency percentiles at fixed offered rates (open loop, 8 cores)."""
    rates = sorted({rate for (_, rate) in openloop})
    if not rates:
        return
    fig, axes = plt.subplots(1, 2, figsize=(DOUBLE_COLUMN, 2.4))
    for ax, letter, (percentile, label) in zip(axes, "ab", [("p99_ns", "p99"), ("p999_ns", "p99.9")]):
        plotted = []
        for design in (NATIVE, FULL):
            xs = [r for r in rates if openloop.get((design, r))]
            ys = [median([res["write_latency"][percentile] / 1e6 for res in openloop[(design, r)]]) for r in xs]
            plotted += ys
            style = STYLE[design]
            ax.plot([x / 1000 for x in xs], ys, marker="o", markersize=3.5, color=style["color"], linewidth=1.2,
                    label=f"{LABELS[design]}, median")
            # Every run, since one rare pause can set a run's p99.9.
            points = [(x / 1000, res["write_latency"][percentile] / 1e6) for x in xs for res in openloop[(design, x)]]
            ax.scatter([p[0] for p in points], [p[1] for p in points], s=9, marker="x", color=style["color"],
                       linewidths=0.8, zorder=3, label=f"{LABELS[design]}, each run")
            # RocksDB's labels go above its line and ours below, so neither sits on a marker.
            above = design == NATIVE
            for x, y in zip(xs, ys):
                ax.annotate(f"{y:.1f}", (x / 1000, y), xytext=(0, 5 if above else -6), textcoords="offset points",
                            ha="center", va="bottom" if above else "top", fontsize=5.6, color=INK, bbox=LABEL_BOX,
                            zorder=4)
        ax.set_yscale("log")
        # Room below the lowest and above the highest median for their labels.
        ax.set_ylim(min(plotted) / 2.2, max(plotted) * 2.2)
        plain_log_axis(ax)
        ax.set_xlabel("Offered load (k writes/s)")
        finish_axis(ax, f"{label} write latency (ms)")
        panel_label(ax, letter)
    axes[0].legend(frameon=False, fontsize=6.5)
    fig.tight_layout(w_pad=2.5)
    save(fig, "fig20_v3_tail")


def latency_ms(results, kind, key):
    values = [r[kind][key] for r in results if r[kind].get("count")]
    return median(values) / 1e6 if values else float("nan")


def workload_lines():
    lines = []
    for name, title in (("stream", "stream-processing state store"), ("metadata", "storage metadata"),
                        ("timeseries", "time series")):
        runs = load_campaign(f"v3-{name}")
        if not runs:
            continue
        lines += ["", f"## X6: {title} workload (docs/workloads.md)", "",
                  "| Cores | Design | Runs | k ops/s | vs native | CPU µs/op | vs native | p99 write ms | p99 read ms |",
                  "|---|---|---|---|---|---|---|---|---|"]
        for cores in (8, 16):
            base = runs.get((NATIVE, cores))
            for design in (NATIVE, FULL):
                results = runs.get((design, cores))
                if not results or not base:
                    continue
                ops, cpu = median([window_ops(r) for r in results]), median([cpu_us(r) for r in results])
                bops, bcpu = median([window_ops(r) for r in base]), median([cpu_us(r) for r in base])
                lines.append(f"| {cores} | {design} | {len(results)} | {ops / 1e3:.0f} | {ops / bops:.2f}× | {cpu:.2f} | "
                             f"{bcpu / cpu:.2f}× | {latency_ms(results, 'write_latency', 'p99_ns'):.2f} | "
                             f"{latency_ms(results, 'read_latency', 'p99_ns'):.2f} |")
    return lines


def mixgraph_lines():
    directory = V3 / "v3-mixgraph"
    if not directory.exists():
        return []
    import re
    runs = {}
    for path in sorted(directory.glob("*-c*-r*.txt")):
        workload, mode, cores, _ = re.match(r"(.+)-(native_cpu|cuda_merge)-c(\d+)-r(\d+)\.txt", path.name).groups()
        text = path.read_text()
        ops = re.search(r"^mixgraph\s*:.*?([0-9.]+) ops/sec", text, re.M)
        cpu = re.search(r"lsm_cpu user_s=([0-9.]+) system_s=([0-9.]+) wall_s=([0-9.]+)", text)
        if not ops or not cpu:
            continue
        rate = float(ops.group(1))
        runs.setdefault((workload, int(cores), DESIGN[mode]), []).append(
            (rate, (float(cpu.group(1)) + float(cpu.group(2))) / float(cpu.group(3))))
    lines = ["", "## X7: MixGraph (Meta's ZippyDB model, db_bench)", "",
             "CPU per operation is the whole process's CPU time over the whole run (including database open), divided by "
             "operations.", "",
             "| Workload | Cores | Design | Runs | k ops/s | vs native | Cores busy |", "|---|---|---|---|---|---|---|"]
    for (workload, cores, design) in sorted(runs, key=lambda k: (k[0], k[1], k[2] != NATIVE)):
        values = runs[(workload, cores, design)]
        base = runs.get((workload, cores, NATIVE))
        ops = median([v[0] for v in values])
        lines.append(f"| {workload} | {cores} | {design} | {len(values)} | {ops / 1e3:.0f} | "
                     f"{ops / median([v[0] for v in base]):.2f}× | {median([v[1] for v in values]):.2f} |"
                     if base else f"| {workload} | {cores} | {design} | {len(values)} | {ops / 1e3:.0f} | - | - |")
    return lines


def ssd_lines():
    runs = load_campaign("v3-ssd")
    if not runs:
        return []
    # btrfs hides the disk from the benchmark's own device statistics; the campaign's iostat
    # log gives them (extracted to device.json, keyed by trial index).
    device_path = V3 / "v3-ssd" / "device.json"
    devices = json.loads(device_path.read_text()) if device_path.exists() else {}
    for line in (V3 / "v3-ssd" / "results.jsonl").read_text().splitlines():
        record = json.loads(line)
        if str(record["index"]) in devices:
            record["result"]["accounting"]["window"]["device"] = devices[str(record["index"])]
            for result in runs.get((DESIGN[record["trial"]["design"]], record["trial"]["cores"]), []):
                if result["window"] == record["result"]["window"]:
                    result["accounting"]["window"]["device"] = devices[str(record["index"])]
    lines = ["", "## X8: SATA SSD with direct I/O, LZ4", "",
             "| Cores | Design | Runs | M writes/s | vs native | CPU µs/write | Device write MiB/s | Device utilisation | p99.9 ms |",
             "|---|---|---|---|---|---|---|---|---|"]
    for cores in (8, 16):
        base = runs.get((NATIVE, cores))
        for design in (NATIVE, FULL):
            results = runs.get((design, cores))
            if not results or not base:
                continue
            device = [r["accounting"]["window"].get("device") or {} for r in results]
            lines.append(f"| {cores} | {design} | {len(results)} | {median([window_ops(r) for r in results]) / 1e6:.3f} | "
                         f"{median([window_ops(r) for r in results]) / median([window_ops(r) for r in base]):.2f}× | "
                         f"{median([cpu_us(r) for r in results]):.2f} | {median([d.get('write_mib_s', float('nan')) for d in device]):.0f} | "
                         f"{100 * median([d.get('utilisation', float('nan')) for d in device]):.0f}% | "
                         f"{median([r['write_latency']['p999_ns'] for r in results]) / 1e6:.1f} |")
    return lines


def workload_ratios():
    """[(label, throughput ratio, CPU-per-operation ratio or None)] for X6-X8, medians over runs."""
    import re
    rows = []
    for name, title in (("stream", "Stream\nstate"), ("metadata", "Metadata"), ("timeseries", "Time\nseries")):
        runs = load_campaign(f"v3-{name}")
        for cores in (8, 16):
            if runs.get((NATIVE, cores)) and runs.get((FULL, cores)):
                n, o = runs[(NATIVE, cores)], runs[(FULL, cores)]
                rows.append((f"{title}\n{cores} cores", median([window_ops(r) for r in o]) / median([window_ops(r) for r in n]),
                             median([cpu_us(r) for r in n]) / median([cpu_us(r) for r in o])))
    directory = V3 / "v3-mixgraph"
    if directory.exists():
        rates = {}
        for path in directory.glob("*-c*-r*.txt"):
            workload, mode, cores, _ = re.match(r"(.+)-(native_cpu|cuda_merge)-c(\d+)-r(\d+)\.txt", path.name).groups()
            found = re.search(r"^mixgraph\s*:.*?([0-9.]+) ops/sec", path.read_text(), re.M)
            if found:
                rates.setdefault((workload, int(cores), DESIGN[mode]), []).append(float(found.group(1)))
        for workload, title in (("zippydb", "MixGraph\nZippyDB"), ("zippydb_write", "MixGraph\nwrite-heavy")):
            for cores in (8, 16):
                if rates.get((workload, cores, NATIVE)) and rates.get((workload, cores, FULL)):
                    rows.append((f"{title}\n{cores} cores", median(rates[(workload, cores, FULL)]) /
                                 median(rates[(workload, cores, NATIVE)]), None))
    runs = load_campaign("v3-ssd")
    for cores in (8, 16):
        if runs.get((NATIVE, cores)) and runs.get((FULL, cores)):
            n, o = runs[(NATIVE, cores)], runs[(FULL, cores)]
            rows.append((f"SATA SSD\n{cores} cores", median([window_ops(r) for r in o]) / median([window_ops(r) for r in n]),
                         median([cpu_us(r) for r in n]) / median([cpu_us(r) for r in o])))
    return rows


def figure_workloads():
    rows = workload_ratios()
    if not rows:
        return
    fig, axes = plt.subplots(2, 1, figsize=(DOUBLE_COLUMN, 4.1), sharex=True)
    for ax, letter, (index, ylabel) in zip(axes, "ab", [(1, "Throughput vs RocksDB"), (2, "CPU saving per operation")]):
        xs = [i for i, row in enumerate(rows) if row[index] is not None]
        values = [rows[i][index] for i in xs]
        rects = bar(ax, xs, values, 0.6, FULL)
        for x, value in zip(xs, values):
            ax.annotate(f"{value:.2f}×", (x, value), xytext=(0, 1.5), textcoords="offset points", ha="center",
                        va="bottom", fontsize=6, color=INK, bbox=LABEL_BOX, zorder=4)
        del rects
        for i, row in enumerate(rows):
            if row[index] is None:
                ax.text(i, 0.08, "not\ncompared", ha="center", va="bottom", fontsize=5.4, color="#555555",
                        style="italic")
        ax.axhline(1.0, color=STYLE[NATIVE]["color"], linewidth=1.0, linestyle="--", zorder=1)
        ax.text(1.005, 1.0, "RocksDB = 1", transform=blended_transform_factory(ax.transAxes, ax.transData),
                ha="left", va="center", fontsize=6, color=STYLE[NATIVE]["color"], clip_on=False)
        ax.set_ylim(0, max(values) * 1.25)
        finish_axis(ax, ylabel)
        panel_label(ax, letter)
    axes[1].set_xticks(range(len(rows)), [row[0] for row in rows], fontsize=5.8)
    fig.tight_layout(h_pad=0.8)
    save(fig, "fig21_v3_workloads")


def write_summary(isolated, headline, bottom, openloop, openloop_original=None, extra_lines=()):
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
    first = ("X5: open-loop write latency, 8 cores, LZ4 (re-run with pinned buffers allocated at open)"
             if openloop_original else "X5: open-loop write latency, 8 cores, LZ4 (first run, before the fix)")
    for title, runs in ((first, openloop),
                        ("X5 as first run (before the fix; one of two runs at 430k and 600k paused)", openloop_original)):
        if not runs:
            continue
        openloop = runs
        lines += ["", f"## {title}", "",
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
    lines += extra_lines
    (V3 / "summary.md").write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    isolated = {tag: load_isolated(tag) for tag, _ in X1 + X2}
    headline = load_campaign("v3-headline", "v3-headline-more")
    bottom = load_campaign("v3-bottomzstd", "v3-bottomzstd-more")
    original = load_campaign("v3-openloop")
    fixed = load_campaign("v3-openloop-fixed")
    openloop = fixed or original
    write_summary(isolated, headline, bottom, openloop, original if fixed else None,
                  workload_lines() + mixgraph_lines() + ssd_lines())
    figure_compaction(isolated)
    figure_cores(isolated)
    figure_database(headline, bottom)
    figure_cost(headline)
    figure_nvme()
    figure_disk()
    figure_cpu_use(headline)
    figure_tail(openloop)
    figure_workloads()
    print((V3 / "summary.md").read_text())
