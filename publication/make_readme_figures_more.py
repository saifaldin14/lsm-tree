"""More README figures: an at-a-glance overview, CPU per write by job, settings, replication.

Imported by make_readme_figures.py; every value is computed from the raw data.
"""

from __future__ import annotations

import json
import statistics
import sys
from pathlib import Path

import matplotlib.pyplot as plt
from matplotlib.ticker import FixedLocator, NullFormatter
from matplotlib.transforms import blended_transform_factory

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE / "rdma"))
import campaigns  # noqa: E402
from make_figures import (DOUBLE_COLUMN, FULL, INK, NATIVE, STYLE, bar, design_legend,  # noqa: E402
                          finish_axis, panel_label, save)
from make_figures_v3 import LABELS, X1, cpu_per_gib, cpu_us, load_campaign, load_isolated, window_ops  # noqa: E402


def median(values):
    return statistics.median(values) if values else float("nan")


def ratio_range(pairs):
    values = [native / ours for native, ours in pairs]
    return min(values), max(values)


def gain_range(pairs):
    values = [ours / native for native, ours in pairs]
    return min(values), max(values)


def overview_rows():
    """[(group, label, low, high)]: ours relative to RocksDB, bigger is better."""
    rows = []
    isolated = {tag: load_isolated(tag) for tag, _ in X1}
    isolated = {tag: runs for tag, runs in isolated.items() if runs}
    rows.append(("One compaction", "Finishes sooner",
                 *ratio_range([(median(r[NATIVE]["wall"]), median(r[FULL]["wall"])) for r in isolated.values()])))
    rows.append(("One compaction", "Uses less CPU",
                 *ratio_range([(median(cpu_per_gib(r[NATIVE])), median(cpu_per_gib(r[FULL]))) for r in isolated.values()])))

    database = []
    for names in (("v3-headline", "v3-headline-more"), ("v3-bottomzstd", "v3-bottomzstd-more")):
        runs = load_campaign(*names)
        database += [(runs[(NATIVE, c)], runs[(FULL, c)]) for c in (4, 8, 16) if runs.get((NATIVE, c)) and runs.get((FULL, c))]
    rows.append(("Whole database", "More writes per second",
                 *gain_range([(median([window_ops(r) for r in n]), median([window_ops(r) for r in o])) for n, o in database])))
    rows.append(("Whole database", "Less CPU per write",
                 *ratio_range([(median([cpu_us(r) for r in n]), median([cpu_us(r) for r in o])) for n, o in database])))
    rows.append(("Whole database", "More writes, counting catch-up",
                 *gain_range([(median([r["ingest_and_compact"]["ops_per_second"] for r in n]),
                               median([r["ingest_and_compact"]["ops_per_second"] for r in o])) for n, o in database])))
    openloop = load_campaign("v3-openloop-fixed") or load_campaign("v3-openloop")
    rates = sorted({rate for _, rate in openloop})
    rows.append(("Whole database", "Slowest 0.1% of writes faster",
                 *ratio_range([(median([r["write_latency"]["p999_ns"] for r in openloop[(NATIVE, rate)]]),
                                median([r["write_latency"]["p999_ns"] for r in openloop[(FULL, rate)]]))
                               for rate in rates if openloop.get((NATIVE, rate)) and openloop.get((FULL, rate))])))

    from make_readme_figures import workload_rows
    workloads = workload_rows()
    write_heavy = [r for group, _, settings in workloads if group == "Write-heavy" for _, r in settings]
    busy = [r for group, _, settings in workloads if group == "Busy machine" for _, r in settings]
    rows.append(("Workloads", "Write-heavy workloads", min(write_heavy), max(write_heavy)))
    rows.append(("Workloads", "Other programs on the same cores", min(busy), max(busy)))

    import analyze_r7
    root = HERE / "data" / "rdma" / "r7"
    summary = analyze_r7.summarize(analyze_r7.load(root))
    analyze_r7.summarize_direct(analyze_r7.load(root.parent / "r7-direct" / "out" / "r7"), summary)
    direct = [summary[w] for w, _ in analyze_r7.WORKLOADS if "remote-gpu-rdma" in summary.get(w, {})]
    rows.append(("GPU in another machine", "One compaction finishes sooner",
                 *ratio_range([(d["direct-native"]["wall"], d["remote-gpu-rdma"]["wall"]) for d in direct])))
    rows.append(("GPU in another machine", "Database machine uses less CPU",
                 *ratio_range([(d["direct-native"]["cpu_gib"], d["remote-gpu-rdma"]["cpu_gib"]) for d in direct])))

    from make_figures import PIPELINE, PRIOR_COMPACTION, SORT_MERGE
    from make_figures import cpu_per_gib as prior_cpu_per_gib
    best = []
    for tag in ("v32-lz4", "v128-lz4", "v1024-lz4", "v128-zstd", "v128-lz4-del0.1"):
        runs = PRIOR_COMPACTION.get(tag, {})
        if FULL in runs and PIPELINE in runs and SORT_MERGE in runs:
            earlier = min(median(prior_cpu_per_gib(runs[d])) for d in (PIPELINE, SORT_MERGE))
            best.append((earlier, median(prior_cpu_per_gib(runs[FULL]))))
    rows.append(("Earlier GPU/FPGA designs", "Less compaction CPU than the best", *ratio_range(best)))
    return rows


def figure_overview():
    rows = overview_rows()
    fig, ax = plt.subplots(figsize=(DOUBLE_COLUMN, 0.55 + 0.27 * len(rows) + 0.22 * len({r[0] for r in rows})))
    y, ticks, labels, previous = 0.0, [], [], None
    colour = STYLE[FULL]["color"]
    headers = []
    for group, label, low, high in rows:
        if group != previous:
            y += 0.9
            headers.append((group, y - 0.75))
            previous = group
        ax.plot([low, high], [y, y], color=colour, linewidth=5, solid_capstyle="round", zorder=3,
                alpha=0.35 if high > low * 1.02 else 0)
        ax.scatter([low, high], [y, y], s=22, color=colour, zorder=4)
        text = f"{low:.2f}×" if high <= low * 1.02 else f"{low:.1f}–{high:.1f}×"
        ax.annotate(text, (high, y), xytext=(7, 0), textcoords="offset points", va="center", fontsize=6.8,
                    fontweight="bold", color=INK)
        ticks.append(y)
        labels.append(label)
        y += 1
    ax.set_xscale("log")
    ax.set_xlim(0.9, 40)
    ax.xaxis.set_major_locator(FixedLocator([1, 2, 5, 10, 20]))
    ax.set_xticks([1, 2, 5, 10, 20], ["1×", "2×", "5×", "10×", "20×"])
    ax.xaxis.set_minor_formatter(NullFormatter())
    ax.axvline(1.0, color=STYLE[NATIVE]["color"], linewidth=1.0, linestyle="--", zorder=2)
    ax.text(1.0, 1.005, "RocksDB", transform=blended_transform_factory(ax.transData, ax.transAxes), ha="center",
            va="bottom", fontsize=6.2, color=STYLE[NATIVE]["color"])
    for group, at in headers:
        ax.text(-0.01, at, group.upper(), transform=blended_transform_factory(ax.transAxes, ax.transData),
                ha="right", va="center", fontsize=6.6, fontweight="bold", color="#555555")
    ax.set_yticks(ticks, labels, fontsize=7)
    ax.set_ylim(y - 0.4, 0.2)
    ax.set_xlabel("Ours relative to RocksDB (bigger is better; log scale; dots are the lowest and highest measured)")
    ax.xaxis.grid(True, color="#e6e6e6", linewidth=0.6, zorder=0)
    ax.set_axisbelow(True)
    ax.tick_params(axis="y", length=0)
    fig.tight_layout()
    save(fig, "fig38_overview")


def figure_cpu_per_write():
    """Tuned whole database, LZ4: CPU per write split by job, RocksDB against ours, 4/8/16 cores."""
    runs = load_campaign("v3-headline", "v3-headline-more")
    jobs = [("foreground", "Accepting writes", "#7f7f7f"), ("flush", "Flushing memory to files", "#b3b3b3"),
            ("compaction", "Compaction", STYLE[FULL]["color"])]
    fig, ax = plt.subplots(figsize=(DOUBLE_COLUMN, 2.5))
    width, ticks, labels = 0.36, [], []
    for index, cores in enumerate((4, 8, 16)):
        for offset, design in ((-width / 2 - 0.02, NATIVE), (width / 2 + 0.02, FULL)):
            results = runs.get((design, cores), [])
            if not results:
                continue
            x = index + offset
            bottom = 0.0
            for key, name, colour in jobs:
                value = median([r["accounting"]["window"]["cores_by_role"].get(key, 0.0) * 1e6 / window_ops(r)
                                for r in results])
                ax.bar(x, value, width, bottom=bottom, color=colour, edgecolor="white", linewidth=0.5, zorder=2,
                       hatch="" if design == FULL or key != "compaction" else "",
                       label=name if index == 0 and design == NATIVE else None)
                if value > 0.45:
                    ax.text(x, bottom + value / 2, f"{value:.1f}", ha="center", va="center", fontsize=6,
                            color="white" if key != "flush" else INK)
                bottom += value
            ax.annotate(f"{bottom:.1f} µs", (x, bottom), xytext=(0, 1.5), textcoords="offset points", ha="center",
                        va="bottom", fontsize=6.4, fontweight="bold", color=INK)
            ticks.append(x)
            labels.append(f"{'RocksDB' if design == NATIVE else 'Ours'}")
        ax.text(index, -0.2, f"{cores} cores", transform=blended_transform_factory(ax.transData, ax.transAxes),
                ha="center", va="top", fontsize=7.2, color=INK)
    ax.set_xticks(ticks, labels, fontsize=6.6)
    ax.set_ylim(0, ax.get_ylim()[1] * 1.12)
    finish_axis(ax, "CPU per write (µs)")
    ax.legend(frameon=False, fontsize=6.6, ncol=3, loc="lower center", bbox_to_anchor=(0.5, 1.0))
    fig.tight_layout(rect=(0, 0.05, 1, 1))
    save(fig, "fig39_cpu_per_write")


def figure_settings():
    """read-tradeoff campaign: writes/s with default, frequent-compaction and frequent + ZSTD settings."""
    entries = campaigns.summarise(campaigns.load("read-tradeoff"))
    shapes = [("default", "Default\n(LZ4)"), ("aggressive", "Compact more\noften"),
              ("aggressive-zstd", "Compact more\noften, ZSTD")]
    fig, ax = plt.subplots(figsize=(DOUBLE_COLUMN * 0.62, 2.4))
    width = 0.38
    top = 0.0
    for index, (shape, _) in enumerate(shapes):
        values = {}
        for entry in entries:
            if entry["config"].get("shape") == shape:
                values[entry["design"]] = entry["ops_per_second"][0] / 1e3
        for offset, design, key in ((-width / 2, NATIVE, "native"), (width / 2, FULL, "full")):
            bar(ax, [index + offset], [values[key]], width * 0.95, design)
            ax.annotate(f"{values[key]:.0f}k", (index + offset, values[key]), xytext=(0, 1.5),
                        textcoords="offset points", ha="center", va="bottom", fontsize=6.2, color=INK)
        ax.annotate(f"{values['full'] / values['native']:.2f}×", (index, max(values.values())), xytext=(0, 12),
                    textcoords="offset points", ha="center", va="bottom", fontsize=7.2, fontweight="bold", color=INK)
        top = max(top, *values.values())
    ax.set_xticks(range(len(shapes)), [label for _, label in shapes], fontsize=6.6)
    ax.set_ylim(0, top * 1.35)
    finish_axis(ax, "Operations per second")
    fig.tight_layout(rect=(0, 0, 1, 0.86))
    design_legend(fig, (NATIVE, FULL), y=0.86, labels=LABELS)
    save(fig, "fig40_settings")


def figure_replication():
    """R6: total CPU of all machines per write, each copy compacting against compacting once."""
    from make_figures import load_rdma_replication
    runs = load_rdma_replication()
    designs = [("per_replica", "Each copy compacts", NATIVE), ("once_primary_cpu", "Compact once,\nsend the files", FULL)]
    fig, ax = plt.subplots(figsize=(DOUBLE_COLUMN * 0.62, 2.4))
    width = 0.38
    top = 0.0
    for index, (writes, title) in enumerate((("batched", "Batches of 100 writes"), ("single", "One write at a time"))):
        values = []
        for offset, (key, _, design) in zip((-width / 2, width / 2), designs):
            value = median([r["total_us"] for r in runs.get((writes, key), [])])
            values.append(value)
            bar(ax, [index + offset], [value], width * 0.95, design)
            ax.annotate(f"{value:.1f} µs", (index + offset, value), xytext=(0, 1.5), textcoords="offset points",
                        ha="center", va="bottom", fontsize=6.2, color=INK)
        ax.annotate(f"{values[0] / values[1]:.1f}× less", (index, max(values)), xytext=(0, 12),
                    textcoords="offset points", ha="center", va="bottom", fontsize=7.2, fontweight="bold", color=INK)
        top = max(top, *values)
    ax.set_xticks([0, 1], ["Batches of 100 writes", "One write at a time"], fontsize=6.8)
    ax.set_ylim(0, top * 1.3)
    finish_axis(ax, "CPU of all 3 machines per write (µs)")
    fig.tight_layout(rect=(0, 0, 1, 0.86))
    design_legend(fig, (NATIVE, FULL), y=0.86, labels={NATIVE: designs[0][1], FULL: designs[1][1].replace("\n", " ")})
    save(fig, "fig41_replication")


def main() -> int:
    figure_overview()
    figure_cpu_per_write()
    figure_settings()
    figure_replication()
    return 0


if __name__ == "__main__":
    sys.exit(main())
