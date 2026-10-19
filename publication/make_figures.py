#!/usr/bin/env python3
"""Figures and summary tables for the GPU compaction results.

Inputs (publication/data/):
  compaction/v<value>-<codec>[-del<f>]-s3.jsonl  lsm_compaction_bench: native_cpu, gpu_output, cuda_merge (full)
  compaction/v<value>-<codec>[-del<f>]-s2.jsonl  lsm_compaction_bench: cuda_merge, LSM_DISABLE_GPU_BUILD=1 (merge)
  campaigns/<campaign>.jsonl                      system_campaign.py (see PREREGISTRATION.md)
Isolated compaction bars are medians of repetitions 1-3 (repetition 0 is a cold
start) with min-max whiskers. Whole-database bars are medians over repetition
blocks with 95% bootstrap CIs; each design is shown at its best background-job
setting. Every plotted run must have matched native RocksDB / passed verification.
Run: python3 publication/make_figures.py
"""

import json
import statistics
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
from matplotlib.patches import Patch

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import campaigns  # noqa: E402

DATA = HERE / "data"
OUT = HERE / "figures"

# Okabe-Ito colourblind-safe palette; hatches keep designs distinct in greyscale.
NATIVE, OUTPUT_ONLY, MERGE, FULL = ("Native RocksDB", "GPU compression only", "GPU merge",
                                    "GPU merge + block building")
DESIGNS = (NATIVE, OUTPUT_ONLY, MERGE, FULL)
KEY = {"native": NATIVE, "compression_only": OUTPUT_ONLY, "merge": MERGE, "full": FULL}
STYLE = {
    NATIVE: {"color": "#8c8c8c", "hatch": ""},
    OUTPUT_ONLY: {"color": "#56B4E9", "hatch": "///"},
    MERGE: {"color": "#0072B2", "hatch": "..."},
    FULL: {"color": "#D55E00", "hatch": ""},
}
INK = "#1a1a1a"
GRID = "#e6e6e6"
DOUBLE_COLUMN = 7.0  # inches

plt.rcParams.update({
    "font.family": "sans-serif",
    "font.sans-serif": ["Helvetica", "Arial", "DejaVu Sans"],
    "font.size": 8,
    "axes.labelsize": 8,
    "axes.titlesize": 8,
    "xtick.labelsize": 7.5,
    "ytick.labelsize": 7.5,
    "legend.fontsize": 7.5,
    "axes.edgecolor": INK,
    "axes.labelcolor": INK,
    "axes.linewidth": 0.6,
    "axes.spines.top": False,
    "axes.spines.right": False,
    "xtick.color": INK,
    "ytick.color": INK,
    "xtick.major.width": 0.6,
    "ytick.major.width": 0.6,
    "xtick.major.size": 2.5,
    "ytick.major.size": 2.5,
    "hatch.linewidth": 0.5,
    "hatch.color": "white",
    "legend.frameon": False,
    "savefig.bbox": "tight",
    "savefig.pad_inches": 0.02,
    "savefig.dpi": 300,
    "pdf.fonttype": 42,
    "ps.fonttype": 42,
})

WORKLOAD_ORDER = [("v32-lz4", "32 B\nLZ4"), ("v128-lz4", "128 B\nLZ4"), ("v1024-lz4", "1 KB\nLZ4"),
                  ("v128-zstd", "128 B\nZSTD"), ("v128-none", "128 B\nno compression"),
                  ("v128-lz4-del0.1", "128 B, LZ4\n10% deletes")]


# ---------------------------------------------------------------- isolated compaction

def load_compaction():
    """{tag: {design: {"wall": [...], "cpu": [...], "input_bytes": [...]}}} for repetitions 1-3."""
    result = {}
    for tag, _ in WORKLOAD_ORDER:
        if not (DATA / "compaction" / f"{tag}-s3.jsonl").exists():
            continue
        runs = {}
        for stage in ("s3", "s2"):
            path = DATA / "compaction" / f"{tag}-{stage}.jsonl"
            if not path.exists():
                continue
            for line in path.read_text().splitlines():
                run = json.loads(line)
                if not run["matches_reference"]:
                    raise SystemExit(f"{tag}: {run['mode']} repetition {run['repetition']} differs from native")
                if run["repetition"] == 0:
                    continue
                design = {"native_cpu": NATIVE, "gpu_output": OUTPUT_ONLY}.get(run["mode"], FULL if stage == "s3" else MERGE)
                entry = runs.setdefault(design, {"wall": [], "cpu": [], "input_bytes": []})
                entry["wall"].append(run["wall_seconds"])
                entry["cpu"].append(run["process_cpu_seconds"])
                entry["input_bytes"].append(run["input_bytes"])
        # The compression-only design offloads LZ4 only; elsewhere it runs as native RocksDB.
        if "lz4" not in tag:
            runs.pop(OUTPUT_ONLY, None)
        result[tag] = runs
    return result


def throughput_mib(entry):
    return [b / 2**20 / w for b, w in zip(entry["input_bytes"], entry["wall"])]


def cpu_per_gib(entry):
    return [c / (b / 2**30) for b, c in zip(entry["input_bytes"], entry["cpu"])]


COMPACTION = load_compaction()


# ---------------------------------------------------------------- drawing helpers

def bar(ax, x, height, width, design, **kwargs):
    style = STYLE[design]
    return ax.bar(x, height, width, color=style["color"], hatch=style["hatch"], edgecolor="white",
                  linewidth=0.4, zorder=2, **kwargs)


def value_labels(ax, rects, fmt, size=6.2, offsets=None):
    for index, rect in enumerate(rects):
        top = rect.get_height() if offsets is None else offsets[index]
        ax.annotate(fmt(rect.get_height()), (rect.get_x() + rect.get_width() / 2, top), xytext=(0, 1.5),
                    textcoords="offset points", ha="center", va="bottom", fontsize=size, color=INK)


def whiskers(ax, xs, ci_list):
    points = [ci[0] for ci in ci_list]
    lows = [ci[0] - ci[1] for ci in ci_list]
    highs = [ci[2] - ci[0] for ci in ci_list]
    ax.errorbar(xs, points, yerr=[lows, highs], fmt="none", ecolor=INK, elinewidth=0.6, capsize=1.2, zorder=3)
    return [p + h for p, h in zip(points, highs)]


def finish_axis(ax, ylabel):
    ax.set_ylabel(ylabel)
    ax.yaxis.grid(True, color=GRID, linewidth=0.6, zorder=0)
    ax.set_axisbelow(True)
    ax.tick_params(axis="x", length=0)


def panel_label(ax, letter):
    ax.text(-0.12, 1.04, f"({letter})", transform=ax.transAxes, fontsize=9, fontweight="bold",
            va="bottom", ha="left", color=INK)


def save(fig, name):
    OUT.mkdir(exist_ok=True)
    fig.savefig(OUT / f"{name}.pdf")
    fig.savefig(OUT / f"{name}.png")
    plt.close(fig)


def design_legend(fig, designs, y=0.9):
    handles = [Patch(facecolor=STYLE[d]["color"], hatch=STYLE[d]["hatch"], edgecolor="white", linewidth=0.4, label=d)
               for d in designs]
    fig.legend(handles=handles, loc="lower center", ncol=len(designs), bbox_to_anchor=(0.5, y),
               handlelength=1.6, columnspacing=1.4)


def grouped_bars(ax, groups, designs, value, fmt, size=5.8):
    """groups: list of (label, {design: (point, low, high)}). Draws bars with CI whiskers."""
    width = 0.8 / len(designs)
    top = 0.0
    for offset, design in enumerate(designs):
        xs, cis = [], []
        for index, (_, by_design) in enumerate(groups):
            ci = by_design.get(design)
            if ci is not None:
                xs.append(index + (offset - (len(designs) - 1) / 2) * width)
                cis.append(ci)
        if not xs:
            continue
        rects = bar(ax, xs, [c[0] for c in cis], width * 0.95, design)
        tops = whiskers(ax, xs, cis)
        value_labels(ax, rects, fmt, size=size, offsets=tops)
        top = max(top, max(tops))
    ax.set_xticks(range(len(groups)), [label for label, _ in groups])
    ax.set_ylim(0, top * 1.16 if top else 1)
    return top


# ---------------------------------------------------------------- figures

def figure_compaction():
    """(a) compaction throughput, MiB/s of input; (b) CPU seconds per GiB of input."""
    tags = [(tag, label) for tag, label in WORKLOAD_ORDER if tag in COMPACTION]
    if not tags:
        return
    fig, axes = plt.subplots(2, 1, figsize=(DOUBLE_COLUMN, 4.1))
    metrics = [("Throughput (MiB/s)", throughput_mib, lambda v: f"{v:.0f}"),
               ("CPU time (s per GiB)", cpu_per_gib, lambda v: f"{v:.1f}")]
    for ax, letter, (ylabel, metric, fmt) in zip(axes, "ab", metrics):
        groups = []
        for tag, label in tags:
            by_design = {}
            for design, entry in COMPACTION[tag].items():
                values = metric(entry)
                by_design[design] = (statistics.median(values), min(values), max(values))
            groups.append((label, by_design))
        grouped_bars(ax, groups, DESIGNS, None, fmt, size=6.0)
        finish_axis(ax, ylabel)
        ax.text(-0.07, 1.02, f"({letter})", transform=ax.transAxes, fontsize=9, fontweight="bold", va="bottom")
    fig.tight_layout(h_pad=1.2, rect=(0, 0, 1, 0.94))
    design_legend(fig, DESIGNS, y=0.94)
    save(fig, "fig1_compaction")


def by_design(entries, metric, **match):
    """{design name: CI} for entries whose config matches **match."""
    out = {}
    for entry in entries:
        if all(entry["config"].get(k) == v for k, v in match.items()) and entry.get(metric):
            out[KEY[entry["design"]]] = entry[metric]
    return out


def figure_database(main):
    """Main campaign, batched writes: throughput, p99 write latency, CPU per op, energy per op."""
    batched = [e for e in main if e["config"].get("writes") == "batched"]
    if not batched:
        return
    designs = [d for d in DESIGNS if any(KEY[e["design"]] == d for e in batched)]
    panels = [("Throughput (kops/s)", "ops_per_second", 1e-3, lambda v: f"{v:.0f}"),
              ("p99 write latency (ms)", "p99_write_ms", 1.0, lambda v: f"{v:.1f}"),
              ("CPU time per op (µs)", "cpu_us_per_op", 1.0, lambda v: f"{v:.2f}"),
              ("Energy per op (µJ)", "energy_uj_per_op", 1.0, lambda v: f"{v:.0f}")]
    fig, axes = plt.subplots(1, 4, figsize=(DOUBLE_COLUMN, 2.1))
    for ax, letter, (ylabel, metric, scale, fmt) in zip(axes, "abcd", panels):
        values = by_design(batched, metric)
        groups = [("", {d: tuple(v * scale for v in ci) for d, ci in values.items()})]
        grouped_bars(ax, groups, designs, None, fmt, size=6.0)
        ax.set_xticks([])
        finish_axis(ax, ylabel)
        panel_label(ax, letter)
    fig.tight_layout(w_pad=1.4, rect=(0, 0, 1, 0.88))
    design_legend(fig, designs, y=0.88)
    save(fig, "fig2_database_batched")


def figure_deferred(main):
    """Throughput during the run vs including catch-up compaction (native and full offload)."""
    fig, axes = plt.subplots(1, 2, figsize=(DOUBLE_COLUMN, 2.25))
    drawn = False
    for ax, letter, (title, writes) in zip(axes, "ab", (("Single writes, WAL on", "single"),
                                                       ("Batched writes, WAL off", "batched"))):
        entries = {e["design"]: e for e in main if e["config"].get("writes") == writes and e["design"] in ("native", "full")}
        if not entries:
            ax.set_visible(False)
            continue
        drawn = True
        width = 0.36
        top = 0
        labels = []
        for index, key in enumerate(("native", "full")):
            if key not in entries:
                continue
            entry, design = entries[key], KEY[key]
            during = tuple(v / 1000 for v in entry["window_ops_per_second"])
            total = tuple(v / 1000 for v in entry["ops_per_second"])
            style = STYLE[design]
            light = ax.bar(index - width / 2, during[0], width, color=style["color"], alpha=0.35,
                           edgecolor=style["color"], linewidth=0.6, zorder=2)
            solid = bar(ax, [index + width / 2], [total[0]], width, design)
            tops = whiskers(ax, [index - width / 2, index + width / 2], [during, total])
            value_labels(ax, list(light) + list(solid), lambda v: f"{v:.0f}", size=6.5, offsets=tops)
            top = max(top, *tops)
            labels.append(f"{design}\n(catch-up {entry['drain_seconds'][0]:.1f} s)")
        ax.set_xticks(range(len(labels)), labels)
        ax.set_ylim(0, top * 1.15)
        ax.set_title(title, loc="left", fontsize=8, color=INK, pad=9)
        finish_axis(ax, "Throughput (kops/s)")
        panel_label(ax, letter)
    if not drawn:
        plt.close(fig)
        return
    handles = [Patch(facecolor="#8c8c8c", alpha=0.35, edgecolor="#8c8c8c", label="During the run"),
               Patch(facecolor="#8c8c8c", edgecolor="white", label="Including catch-up compaction")]
    fig.tight_layout(w_pad=3, rect=(0, 0, 1, 0.9))
    fig.legend(handles=handles, loc="lower center", ncol=2, bbox_to_anchor=(0.5, 0.9), handlelength=1.6)
    save(fig, "fig3_deferred_compaction")


def figure_equal_cost(main, equal):
    """Throughput against physical cores: native at 4-32 cores, full offload at 4 and 8."""
    points = {}
    for entry in [e for e in main if e["config"].get("writes") == "batched"] + equal:
        if entry["design"] not in ("native", "full"):
            continue
        points.setdefault(entry["design"], {})[entry["config"]["cores"]] = entry["ops_per_second"]
    if "native" not in points or len(points["native"]) < 2:
        return
    fig, ax = plt.subplots(figsize=(3.4, 2.3))
    for key, marker in (("native", "o"), ("full", "s")):
        series = sorted(points.get(key, {}).items())
        if not series:
            continue
        xs = [c for c, _ in series]
        ys = [ci[0] / 1000 for _, ci in series]
        lows = [(ci[0] - ci[1]) / 1000 for _, ci in series]
        highs = [(ci[2] - ci[0]) / 1000 for _, ci in series]
        ax.errorbar(xs, ys, yerr=[lows, highs], color=STYLE[KEY[key]]["color"], marker=marker, markersize=4,
                    linewidth=1.2, capsize=1.5, elinewidth=0.6, label=KEY[key], zorder=3)
        for x, y in zip(xs, ys):
            ax.annotate(f"{y:.0f}", (x, y), xytext=(0, 4), textcoords="offset points", ha="center", fontsize=6.2)
    ax.set_xscale("log", base=2)
    ax.set_xticks([4, 8, 16, 32], ["4", "8", "16", "32"])
    ax.set_xlabel("Physical cores")
    ax.set_ylim(bottom=0)
    finish_axis(ax, "Throughput (kops/s)")
    ax.tick_params(axis="x", length=2.5)
    ax.legend(loc="lower right")
    fig.tight_layout()
    save(fig, "fig4_equal_cost")


def figure_ycsb(entries):
    if not entries:
        return
    workloads = [w for w in ("A", "B", "F", "W") if any(e["config"].get("workload") == w for e in entries)]
    labels = {"A": "A\n50% reads", "B": "B\n95% reads", "F": "F\n50% RMW", "W": "Writes\nonly"}
    fig, axes = plt.subplots(1, 2, figsize=(DOUBLE_COLUMN, 2.35))
    designs = [NATIVE, FULL]
    for ax, letter, dist in zip(axes, "ab", ("uniform", "zipfian")):
        groups = []
        for w in workloads:
            values = by_design(entries, "ops_per_second", workload=w, distribution=dist)
            groups.append((labels[w], {d: tuple(v / 1000 for v in ci) for d, ci in values.items()}))
        grouped_bars(ax, groups, designs, None, lambda v: f"{v:.0f}", size=6.0)
        ax.set_title("Uniform keys" if dist == "uniform" else "Zipfian keys (θ = 0.99)", loc="left", fontsize=8, pad=9)
        finish_axis(ax, "Throughput (kops/s)")
        panel_label(ax, letter)
    fig.tight_layout(w_pad=2.5, rect=(0, 0, 1, 0.9))
    design_legend(fig, designs)
    save(fig, "fig5_ycsb")


def figure_contention(main, entries):
    rows = []
    for e in [m for m in main if m["config"].get("writes") == "batched" and m["config"].get("cores") == 4] + entries:
        if e["design"] in ("native", "full"):
            rows.append(e)
    conditions = [(2, 0, "2 cores"), (4, 0, "4 cores"), (8, 0, "8 cores"), (16, 0, "16 cores"),
                  (4, 40, "4 cores,\n40% load"), (4, 80, "4 cores,\n80% load")]
    groups = []
    for cores, burn, label in conditions:
        values = {KEY[e["design"]]: tuple(v / 1000 for v in e["ops_per_second"]) for e in rows
                  if e["config"].get("cores") == cores and e["config"].get("burn_percent", 0) == burn}
        if values:
            groups.append((label, values))
    if len(groups) < 2:
        return
    fig, ax = plt.subplots(figsize=(DOUBLE_COLUMN, 2.2))
    grouped_bars(ax, groups, [NATIVE, FULL], None, lambda v: f"{v:.0f}", size=6.0)
    finish_axis(ax, "Throughput (kops/s)")
    fig.tight_layout(rect=(0, 0, 1, 0.88))
    design_legend(fig, [NATIVE, FULL], y=0.88)
    save(fig, "fig6_contention")


def figure_read_tradeoff(entries):
    if not entries:
        return
    shapes = [("default", "Default"), ("aggressive", "Aggressive"), ("aggressive-zstd", "Aggressive + ZSTD")]
    fig, axes = plt.subplots(1, 3, figsize=(DOUBLE_COLUMN, 2.1))
    panels = [("Throughput (kops/s)", "ops_per_second", 1e-3, lambda v: f"{v:.0f}"),
              ("p99 read latency (ms)", "p99_read_ms", 1.0, lambda v: f"{v:.2f}"),
              ("CPU time per op (µs)", "cpu_us_per_op", 1.0, lambda v: f"{v:.1f}")]
    for ax, letter, (ylabel, metric, scale, fmt) in zip(axes, "abc", panels):
        groups = []
        for shape, label in shapes:
            values = by_design(entries, metric, shape=shape)
            if values:
                groups.append((label, {d: tuple(v * scale for v in ci) for d, ci in values.items()}))
        grouped_bars(ax, groups, [NATIVE, FULL], None, fmt, size=5.8)
        finish_axis(ax, ylabel)
        panel_label(ax, letter)
    fig.tight_layout(w_pad=1.6, rect=(0, 0, 1, 0.88))
    design_legend(fig, [NATIVE, FULL], y=0.88)
    save(fig, "fig7_read_tradeoff")


def figure_shared_gpu(entries):
    if not entries:
        return
    fig, axes = plt.subplots(1, 2, figsize=(DOUBLE_COLUMN, 2.25))
    for ax, letter, (ylabel, metric, scale) in zip(axes, "ab", (("Aggregate throughput (kops/s)", "ops_per_second", 1e-3),
                                                               ("Energy per op (µJ)", "energy_uj_per_op", 1.0))):
        for key, marker in (("native", "o"), ("full", "s")):
            series = sorted((e["config"]["instances"], e[metric]) for e in entries if e["design"] == key and e.get(metric))
            if not series:
                continue
            xs = [n for n, _ in series]
            ys = [ci[0] * scale for _, ci in series]
            ax.errorbar(xs, ys, yerr=[[(ci[0] - ci[1]) * scale for _, ci in series],
                                      [(ci[2] - ci[0]) * scale for _, ci in series]],
                        color=STYLE[KEY[key]]["color"], marker=marker, markersize=4, linewidth=1.2, capsize=1.5,
                        elinewidth=0.6, label=KEY[key], zorder=3)
        ax.set_xscale("log", base=2)
        ax.set_xticks([1, 2, 4, 8], ["1", "2", "4", "8"])
        ax.set_xlabel("Database instances (4 cores each)")
        ax.set_ylim(bottom=0)
        finish_axis(ax, ylabel)
        ax.tick_params(axis="x", length=2.5)
        panel_label(ax, letter)
    axes[0].legend(loc="upper left")
    fig.tight_layout(w_pad=3)
    save(fig, "fig8_shared_gpu")


def figure_ssd(entries):
    if not entries:
        return
    workloads = [("batched", "Batched writes"), ("ycsb-A", "YCSB A")]
    fig, axes = plt.subplots(1, 3, figsize=(DOUBLE_COLUMN, 2.1))
    panels = [("Throughput (kops/s)", "ops_per_second", 1e-3, lambda v: f"{v:.0f}"),
              ("p99 write latency (ms)", "p99_write_ms", 1.0, lambda v: f"{v:.1f}"),
              ("CPU time per op (µs)", "cpu_us_per_op", 1.0, lambda v: f"{v:.1f}")]
    for ax, letter, (ylabel, metric, scale, fmt) in zip(axes, "abc", panels):
        groups = []
        for workload, label in workloads:
            values = by_design(entries, metric, workload=workload)
            if values:
                groups.append((label, {d: tuple(v * scale for v in ci) for d, ci in values.items()}))
        grouped_bars(ax, groups, [NATIVE, FULL], None, fmt, size=6.0)
        finish_axis(ax, ylabel)
        panel_label(ax, letter)
    fig.tight_layout(w_pad=1.6, rect=(0, 0, 1, 0.88))
    design_legend(fig, [NATIVE, FULL], y=0.88)
    save(fig, "fig9_ssd")


# ---------------------------------------------------------------- tables

def write_summary(summaries):
    lines = ["# Numbers behind the figures", "", "Generated by `make_figures.py` from `data/`.", ""]
    if COMPACTION:
        lines += ["## Isolated compaction (median of 3 warm repetitions; min-max in brackets)", "",
                  "| Workload | Design | Wall s | Throughput MiB/s | Speed-up | CPU s | CPU s/GiB | CPU reduction |",
                  "|---|---|---|---|---|---|---|---|"]
        for tag, label in WORKLOAD_ORDER:
            if tag not in COMPACTION:
                continue
            base = COMPACTION[tag][NATIVE]
            for design in DESIGNS:
                if design not in COMPACTION[tag]:
                    continue
                entry = COMPACTION[tag][design]
                wall = statistics.median(entry["wall"])
                cpu = statistics.median(entry["cpu"])
                mib = throughput_mib(entry)
                gib = cpu_per_gib(entry)
                lines.append(f"| {label.replace(chr(10), ' ')} | {design} | {wall:.2f} | {statistics.median(mib):.0f} "
                             f"[{min(mib):.0f}-{max(mib):.0f}] | {statistics.median(base['wall']) / wall:.2f}x | {cpu:.2f} | "
                             f"{statistics.median(gib):.2f} [{min(gib):.2f}-{max(gib):.2f}] | "
                             f"{statistics.median(base['cpu']) / cpu:.2f}x |")
    for name, entries in summaries.items():
        if not entries:
            continue
        lines += ["", f"## Campaign `{name}` (medians, 95% bootstrap CI; ratios paired against native)", "",
                  "| Configuration | Design | Bg | Runs | kops/s | vs native | p99 write ms | p99 read ms | CPU µs/op | "
                  "vs native | Energy µJ/op | vs native | Catch-up s | Stalled % |",
                  "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
        for e in entries:
            config = ", ".join(f"{k}={v}" for k, v in e["config"].items()
                               if v not in (0, "0", None) and k not in ("batch-size", "disable-wal", "verify", "records",
                                                                        "warmup-seconds", "duration-seconds",
                                                                        "adaptive-yield", "read-percent")
                               and not (k == "instances" and v == 1))
            lines.append(f"| {config} | {KEY[e['design']]} | {e['background_jobs']} | {e['runs']} | "
                         f"{campaigns.fmt(e['ops_per_second'], 0, 1e-3)} | {campaigns.fmt(e.get('ratio_ops_per_second'), 2)} | "
                         f"{campaigns.fmt(e['p99_write_ms'], 2)} | {campaigns.fmt(e['p99_read_ms'], 3)} | "
                         f"{campaigns.fmt(e['cpu_us_per_op'], 2)} | {campaigns.fmt(e.get('ratio_cpu_us_per_op'), 2)} | "
                         f"{campaigns.fmt(e['energy_uj_per_op'], 0)} | {campaigns.fmt(e.get('ratio_energy_uj_per_op'), 2)} | "
                         f"{campaigns.fmt(e['drain_seconds'], 1)} | {campaigns.fmt(e['stall_percent'], 0)} |")
    lines += cost_table(summaries)
    (DATA / "summary.md").write_text("\n".join(lines) + "\n")


def cost_table(summaries):
    """Cloud cost per billion operations, batched writes, for each core budget measured."""
    prices = json.loads((HERE / "prices.json").read_text())
    rows = [e for e in summaries.get("main", []) if e["config"].get("writes") == "batched"] + summaries.get("equal-cost", [])
    rows = [e for e in rows if e["design"] in ("native", "full")]
    if not rows:
        return []
    lines = ["", "## Cost per billion operations (batched writes)", "",
             f"Prices: {prices['source']}. Core: ${prices['physical_core_hour']}/h ({prices['physical_core_hour_basis']}). "
             f"GPU: ${prices['gpu_hour']}/h ({prices['gpu_hour_basis']}).", "",
             "| Design | Cores | kops/s | $/hour | $ per billion ops |", "|---|---|---|---|---|"]
    for e in sorted(rows, key=lambda e: (e["design"], e["config"]["cores"])):
        cores = e["config"]["cores"]
        hourly = cores * prices["physical_core_hour"] + (prices["gpu_hour"] if e["design"] == "full" else 0.0)
        ops = e["ops_per_second"][0]
        lines.append(f"| {KEY[e['design']]} | {cores} | {ops / 1000:,.0f} | {hourly:.3f} | {hourly / (ops * 3600) * 1e9:.2f} |")
    return lines


if __name__ == "__main__":
    names = ("main", "equal-cost", "ycsb", "contention", "read-tradeoff", "shared-gpu", "ssd-scale")
    summaries = {name: campaigns.summarise(campaigns.load(name)) for name in names}
    write_summary(summaries)
    figure_compaction()
    figure_database(summaries["main"])
    figure_deferred(summaries["main"])
    figure_equal_cost(summaries["main"], summaries["equal-cost"])
    figure_ycsb(summaries["ycsb"])
    figure_contention(summaries["main"], summaries["contention"])
    figure_read_tradeoff(summaries["read-tradeoff"])
    figure_shared_gpu(summaries["shared-gpu"])
    figure_ssd(summaries["ssd-scale"])
    print("wrote", sorted(p.name for p in OUT.glob("*.pdf")))
