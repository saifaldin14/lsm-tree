#!/usr/bin/env python3
"""Figures and summary tables for the GPU compaction results.

Reads the raw results archived in publication/data/:
  compaction/v<value>-<codec>-s3.jsonl  lsm_compaction_bench: native_cpu, gpu_output, cuda_merge (Stage 3)
  compaction/v<value>-<codec>-s2.jsonl  lsm_compaction_bench: cuda_merge with LSM_DISABLE_GPU_BUILD=1 (Stage 2)
  database/batched-ingest.jsonl         system_campaign.py + lsm_system_bench
Isolated compaction: bars are medians of repetitions 1-3 (repetition 0 is a
cold start); error bars span the minimum and maximum. Every plotted run must
match native RocksDB exactly.
Run: python3 publication/make_figures.py
"""

import json
import statistics
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Patch

HERE = Path(__file__).resolve().parent
DATA = HERE / "data"
OUT = HERE / "figures"

# Okabe-Ito colourblind-safe palette; hatches keep designs distinct in greyscale.
NATIVE, OUTPUT_ONLY, MERGE, FULL = ("Native RocksDB", "GPU compression only", "GPU merge",
                                    "GPU merge + block building")
DESIGNS = (NATIVE, OUTPUT_ONLY, MERGE, FULL)
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

WORKLOADS = [("v32-lz4", "32 B values"), ("v128-lz4", "128 B values"), ("v1024-lz4", "1 KB values"),
             ("v128-none", "128 B values,\nuncompressed")]


def load_compaction():
    """{tag: {design: {"wall": [...], "cpu": [...], "input_bytes": [...]}}} for repetitions 1-3."""
    result = {}
    for tag, _ in WORKLOADS:
        runs = {}
        for stage in ("s3", "s2"):
            for line in (DATA / "compaction" / f"{tag}-{stage}.jsonl").read_text().splitlines():
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
        result[tag] = runs
    return result


def load_database():
    """Batched runs: each design at the background-job setting with the best end-to-end throughput."""
    design_of = {"native_cpu": NATIVE, "gpu_output": OUTPUT_ONLY, "cuda_merge": FULL}
    batched, single = {}, {}
    for line in (DATA / "database" / "batched-ingest.jsonl").read_text().splitlines():
        record = json.loads(line)
        result = record["result"]
        if record["exit_code"] != 0 or result.get("verification", {}).get("keys") != result["config"]["records"]:
            raise SystemExit(f"database run {record['index']} failed or was not verified")
        end = result["ingest_and_compact"]
        row = {"window": result["throughput"]["ops_per_second"], "end_to_end": end["ops_per_second"],
               "p99_ms": result["write_latency"]["p99_ns"] / 1e6, "cpu_us": end["cpu_microseconds_per_op"],
               "stall": result["stall_fraction"] * 100, "drain": end["drain_seconds"],
               "background_jobs": record["flags"]["background-jobs"]}
        target = batched if record["flags"].get("batch-size", 1) > 1 else single
        design = design_of[record["trial"]["mode"]]
        if design not in target or row["end_to_end"] > target[design]["end_to_end"]:
            target[design] = row
    return batched, single


def throughput_mib(entry):
    return [b / 2**20 / w for b, w in zip(entry["input_bytes"], entry["wall"])]


def cpu_per_gib(entry):
    return [c / (b / 2**30) for b, c in zip(entry["input_bytes"], entry["cpu"])]


COMPACTION = load_compaction()
BATCHED, SINGLE = load_database()


def write_summary():
    lines = ["# Numbers behind the figures", "", "Generated by `make_figures.py` from `data/`.", "",
             "## Isolated compaction (median of 3 warm repetitions; min-max in brackets)", "",
             "| Workload | Design | Wall s | Throughput MiB/s | Speed-up | CPU s | CPU s/GiB | CPU reduction |",
             "|---|---|---|---|---|---|---|---|"]
    for tag, label in WORKLOADS:
        base = COMPACTION[tag][NATIVE]
        for design in DESIGNS:
            entry = COMPACTION[tag][design]
            wall = statistics.median(entry["wall"])
            cpu = statistics.median(entry["cpu"])
            mib = throughput_mib(entry)
            gib = cpu_per_gib(entry)
            lines.append(f"| {label.replace(chr(10), ' ')} | {design} | {wall:.2f} | {statistics.median(mib):.0f} "
                         f"[{min(mib):.0f}-{max(mib):.0f}] | {statistics.median(base['wall']) / wall:.2f}x | {cpu:.2f} | "
                         f"{statistics.median(gib):.2f} [{min(gib):.2f}-{max(gib):.2f}] | "
                         f"{statistics.median(base['cpu']) / cpu:.2f}x |")
    for title, data in (("Whole database, batched writes (best background-job setting per design)", BATCHED),
                        ("Whole database, single writes", SINGLE)):
        lines += ["", f"## {title}", "",
                  "| Design | Bg jobs | Ops/s during run | Ops/s incl. catch-up | Catch-up s | CPU us/op | p99 write ms | Stalled % |",
                  "|---|---|---|---|---|---|---|---|"]
        for design, row in data.items():
            lines.append(f"| {design} | {row['background_jobs']} | {row['window']:,.0f} | {row['end_to_end']:,.0f} | "
                         f"{row['drain']:.1f} | {row['cpu_us']:.2f} | {row['p99_ms']:.3f} | {row['stall']:.0f} |")
    (DATA / "summary.md").write_text("\n".join(lines) + "\n")


def bar(ax, x, height, width, design, **kwargs):
    style = STYLE[design]
    return ax.bar(x, height, width, color=style["color"], hatch=style["hatch"], edgecolor="white",
                  linewidth=0.4, zorder=2, **kwargs)


def value_labels(ax, rects, fmt, size=6.2, offsets=None):
    for index, rect in enumerate(rects):
        top = rect.get_height() if offsets is None else offsets[index]
        ax.annotate(fmt(rect.get_height()), (rect.get_x() + rect.get_width() / 2, top), xytext=(0, 1.5),
                    textcoords="offset points", ha="center", va="bottom", fontsize=size, color=INK)


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


def figure_compaction():
    """(a) compaction throughput, MiB/s of input; (b) CPU seconds per GiB of input."""
    fig, axes = plt.subplots(1, 2, figsize=(DOUBLE_COLUMN, 2.35))
    width = 0.2
    groups = range(len(WORKLOADS))
    metrics = [("Throughput (MiB/s)", throughput_mib, lambda v: f"{v:.0f}"),
               ("CPU time (s per GiB)", cpu_per_gib, lambda v: f"{v:.1f}")]
    for ax, letter, (ylabel, metric, fmt) in zip(axes, "ab", metrics):
        top = 0
        for offset, design in enumerate(DESIGNS):
            values = [metric(COMPACTION[tag][design]) for tag, _ in WORKLOADS]
            medians = [statistics.median(v) for v in values]
            lows = [m - min(v) for m, v in zip(medians, values)]
            highs = [max(v) - m for m, v in zip(medians, values)]
            xs = [g + (offset - 1.5) * width for g in groups]
            rects = bar(ax, xs, medians, width * 0.95, design)
            ax.errorbar(xs, medians, yerr=[lows, highs], fmt="none", ecolor=INK, elinewidth=0.6, capsize=1.2,
                        zorder=3)
            value_labels(ax, rects, fmt, size=5.8, offsets=[m + h for m, h in zip(medians, highs)])
            top = max(top, max(m + h for m, h in zip(medians, highs)))
        ax.set_xticks(list(groups), [label for _, label in WORKLOADS])
        ax.set_ylim(0, top * 1.14)
        finish_axis(ax, ylabel)
        panel_label(ax, letter)
    fig.tight_layout(w_pad=2.5, rect=(0, 0, 1, 0.9))
    design_legend(fig, DESIGNS)
    save(fig, "fig1_compaction")


def figure_database():
    """Batched writes: (a) throughput incl. catch-up, (b) catch-up time, (c) p99 write latency, (d) CPU per op."""
    designs = [d for d in (NATIVE, OUTPUT_ONLY, FULL) if d in BATCHED]
    fig, axes = plt.subplots(1, 4, figsize=(DOUBLE_COLUMN, 2.1))
    panels = [
        ("Throughput (kops/s)", lambda d: BATCHED[d]["end_to_end"] / 1000, lambda v: f"{v:.0f}"),
        ("Catch-up compaction (s)", lambda d: BATCHED[d]["drain"], lambda v: f"{v:.1f}"),
        ("p99 write latency (ms)", lambda d: BATCHED[d]["p99_ms"], lambda v: f"{v:.1f}"),
        ("CPU time per op (µs)", lambda d: BATCHED[d]["cpu_us"], lambda v: f"{v:.2f}"),
    ]
    for ax, letter, (ylabel, value, fmt) in zip(axes, "abcd", panels):
        heights = [value(d) for d in designs]
        rects = [bar(ax, [index], [height], 0.7, design)[0] for index, (height, design) in enumerate(zip(heights, designs))]
        value_labels(ax, rects, fmt, size=6.5)
        ax.set_xticks([])
        ax.set_xlim(-0.6, len(designs) - 0.4)
        ax.set_ylim(0, max(heights) * 1.16)
        finish_axis(ax, ylabel)
        panel_label(ax, letter)
    fig.tight_layout(w_pad=1.6, rect=(0, 0, 1, 0.88))
    design_legend(fig, designs, y=0.88)
    save(fig, "fig2_database_batched")


def figure_deferred_compaction():
    """Throughput measured during the run vs including catch-up compaction, single and batched writes."""
    fig, axes = plt.subplots(1, 2, figsize=(DOUBLE_COLUMN, 2.25))
    width = 0.36
    for ax, letter, (title, data) in zip(axes, "ab", (("Single writes, WAL on", SINGLE),
                                                      ("Batched writes, WAL off", BATCHED))):
        designs = [d for d in (NATIVE, FULL) if d in data]
        top = 0
        for index, design in enumerate(designs):
            during = data[design]["window"] / 1000
            total = data[design]["end_to_end"] / 1000
            style = STYLE[design]
            light = ax.bar(index - width / 2, during, width, color=style["color"], alpha=0.35, edgecolor=style["color"],
                           linewidth=0.6, zorder=2)
            solid = bar(ax, [index + width / 2], [total], width, design)
            value_labels(ax, list(light) + list(solid), lambda v: f"{v:.0f}", size=6.5)
            top = max(top, during, total)
        ax.set_xticks(range(len(designs)),
                      [f"{d}\n(catch-up {data[d]['drain']:.1f} s)" for d in designs])
        ax.set_ylim(0, top * 1.15)
        ax.set_title(title, loc="left", fontsize=8, color=INK, pad=9)
        finish_axis(ax, "Throughput (kops/s)")
        panel_label(ax, letter)
    handles = [Patch(facecolor="#8c8c8c", alpha=0.35, edgecolor="#8c8c8c", label="During the run"),
               Patch(facecolor="#8c8c8c", edgecolor="white", label="Including catch-up compaction")]
    fig.tight_layout(w_pad=3, rect=(0, 0, 1, 0.9))
    fig.legend(handles=handles, loc="lower center", ncol=2, bbox_to_anchor=(0.5, 0.9), handlelength=1.6)
    save(fig, "fig3_deferred_compaction")


if __name__ == "__main__":
    write_summary()
    figure_compaction()
    figure_database()
    figure_deferred_compaction()
    print("wrote", sorted(p.name for p in OUT.glob("*.pdf")))
