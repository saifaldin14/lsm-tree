#!/usr/bin/env python3
"""Figures for the GPU compaction results (Stage 3).

Numbers are medians of repetitions 1-3 (repetition 0 is a cold start) from
lsm_compaction_bench on Zyra (Xeon Gold 5220, RTX A6000, /dev/shm), and
single runs of lsm_system_bench (4 cores, 50 M keys, 128-byte values).
Run: python3 publication/make_figures.py  ->  publication/figures/*.png|pdf
"""

from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

OUT = Path(__file__).resolve().parent / "figures"

GREY = "#9aa0a6"
LIGHT = "#a8c8e8"
MID = "#4a90c8"
ACCENT = "#d9822b"
INK = "#202124"

plt.rcParams.update({
    "font.family": "DejaVu Sans",
    "font.size": 10,
    "axes.edgecolor": "#5f6368",
    "axes.labelcolor": INK,
    "axes.titlesize": 12,
    "axes.titleweight": "bold",
    "axes.spines.top": False,
    "axes.spines.right": False,
    "xtick.color": INK,
    "ytick.color": INK,
    "legend.frameon": False,
    "savefig.bbox": "tight",
    "savefig.dpi": 200,
})

# Isolated compaction (one full CompactRange; identical output in every mode).
WORKLOADS = ["32 B values", "128 B values", "1 KB values", "128 B values,\nno compression"]
CPU_PER_GIB = {  # process CPU seconds per GiB of compaction input
    "Native RocksDB": [16.65, 10.51, 5.55, 5.09],
    "GPU compression only": [15.34, 9.37, 5.75, 7.94],
    "GPU merge": [9.69, 5.85, 3.78, 5.05],
    "GPU merge + block building": [2.29, 2.09, 1.94, 1.77],
}
SPEEDUP = {  # wall-time speedup over native
    "GPU compression only": [1.12, 1.22, 1.09, 0.99],
    "GPU merge": [1.74, 1.97, 1.76, 1.54],
    "GPU merge + block building": [6.06, 4.63, 2.70, 2.32],
}
COLORS = {
    "Native RocksDB": GREY,
    "GPU compression only": LIGHT,
    "GPU merge": MID,
    "GPU merge + block building": ACCENT,
}

# Whole database, batched writes (100 puts per batch, WAL off), 4 cores.
# "End to end" divides all operations by the time to write them and then
# finish every compaction they caused.
BATCHED = {
    "Native RocksDB": {"window": 468607, "end_to_end": 349389, "p99_ms": 7.08, "cpu_us": 9.06, "stall": 7, "drain": 68.5},
    "GPU compression only": {"window": 215720, "end_to_end": 162752, "p99_ms": 24.1, "cpu_us": 9.65, "stall": 17, "drain": 111.8},
    "GPU merge + block building": {"window": 613751, "end_to_end": 602465, "p99_ms": 1.11, "cpu_us": 5.59, "stall": 0, "drain": 3.4},
}
SINGLE = {
    "Native RocksDB": {"window": 210432, "end_to_end": 120733, "p99_us": 74, "cpu_us": 22.08, "drain": 114.1},
    "GPU merge + block building": {"window": 237358, "end_to_end": 231862, "p99_us": 55, "cpu_us": 16.23, "drain": 3.5},
}


def label_bars(ax, bars, fmt, pad=3, size=8.5):
    for bar in bars:
        height = bar.get_height()
        ax.annotate(fmt(height), (bar.get_x() + bar.get_width() / 2, height), xytext=(0, pad),
                    textcoords="offset points", ha="center", va="bottom", fontsize=size, color=INK)


def save(fig, name):
    OUT.mkdir(exist_ok=True)
    fig.savefig(OUT / f"{name}.png")
    fig.savefig(OUT / f"{name}.pdf")
    plt.close(fig)


def figure_compaction_cpu():
    fig, ax = plt.subplots(figsize=(8.6, 4.2))
    width = 0.2
    for offset, (name, values) in enumerate(CPU_PER_GIB.items()):
        xs = [index + (offset - 1.5) * width for index in range(len(WORKLOADS))]
        bars = ax.bar(xs, values, width, label=name, color=COLORS[name])
        label_bars(ax, bars, lambda v: f"{v:.1f}", size=8)
    ax.set_xticks(range(len(WORKLOADS)), WORKLOADS)
    ax.set_ylabel("CPU seconds per GiB compacted\n(lower is better)")
    ax.set_title("CPU cost of compaction")
    ax.set_ylim(0, 19)
    ax.legend(ncol=2, loc="upper right")
    ax.yaxis.grid(True, color="#e8eaed")
    ax.set_axisbelow(True)
    save(fig, "1_compaction_cpu")


def figure_compaction_speedup():
    fig, ax = plt.subplots(figsize=(8.6, 4.2))
    width = 0.26
    for offset, (name, values) in enumerate(SPEEDUP.items()):
        xs = [index + (offset - 1) * width for index in range(len(WORKLOADS))]
        bars = ax.bar(xs, values, width, label=name, color=COLORS[name])
        label_bars(ax, bars, lambda v: f"{v:.1f}×", size=8.5)
    ax.axhline(1.0, color=GREY, linestyle="--", linewidth=1, label="Native RocksDB (1×)")
    ax.set_xticks(range(len(WORKLOADS)), WORKLOADS)
    ax.set_ylabel("Compaction speed-up over native\n(higher is better)")
    ax.set_title("Compaction speed")
    ax.set_ylim(0, 7)
    ax.legend(loc="upper right")
    ax.yaxis.grid(True, color="#e8eaed")
    ax.set_axisbelow(True)
    save(fig, "2_compaction_speedup")


def figure_database():
    names = list(BATCHED)
    colors = [COLORS[name] for name in names]
    short = ["Native\nRocksDB", "GPU compression\nonly", "GPU merge +\nblock building"]
    fig, axes = plt.subplots(1, 3, figsize=(11.5, 3.9))
    panels = [
        ("Throughput\n(thousand ops/s, higher is better)", [BATCHED[n]["end_to_end"] / 1000 for n in names], lambda v: f"{v:.0f}k"),
        ("p99 write latency\n(ms, lower is better)", [BATCHED[n]["p99_ms"] for n in names], lambda v: f"{v:.1f}"),
        ("CPU per operation\n(µs, lower is better)", [BATCHED[n]["cpu_us"] for n in names], lambda v: f"{v:.1f}"),
    ]
    for ax, (title, values, fmt) in zip(axes, panels):
        bars = ax.bar(range(len(names)), values, 0.62, color=colors)
        label_bars(ax, bars, fmt)
        ax.set_xticks(range(len(names)), short, fontsize=8.5)
        ax.set_title(title, fontsize=10.5)
        ax.set_ylim(0, max(values) * 1.18)
        ax.yaxis.grid(True, color="#e8eaed")
        ax.set_axisbelow(True)
    fig.suptitle("Whole database: batched writes on 4 CPU cores", fontweight="bold", fontsize=12, y=1.03)
    fig.text(0.5, -0.05, "Throughput counts the time to finish all compaction the writes caused. "
             "50 M keys, 128-byte values, 100 writes per batch. Single runs.",
             ha="center", fontsize=8.5, color="#5f6368")
    fig.tight_layout()
    save(fig, "3_database_batched")


def figure_deferred_compaction():
    groups = [("Batched writes", BATCHED), ("Single writes", SINGLE)]
    fig, axes = plt.subplots(1, 2, figsize=(10, 4.0), sharey=False)
    for ax, (title, data) in zip(axes, groups):
        names = [n for n in ["Native RocksDB", "GPU merge + block building"] if n in data]
        width = 0.36
        for index, name in enumerate(names):
            window = data[name]["window"] / 1000
            total = data[name]["end_to_end"] / 1000
            bars = ax.bar([index - width / 2], [window], width, color=COLORS[name], alpha=0.35,
                          label="During the run only" if index == 0 else None)
            label_bars(ax, bars, lambda v: f"{v:.0f}k", size=8.5)
            bars = ax.bar([index + width / 2], [total], width, color=COLORS[name],
                          label="Including catch-up compaction" if index == 0 else None)
            label_bars(ax, bars, lambda v: f"{v:.0f}k", size=8.5)
        ax.set_xticks(range(len(names)), [f"{label}\n(catch-up {data[name]['drain']:.0f} s)" for label, name in
                                          zip(["Native RocksDB", "GPU merge + block building"], names)])
        ax.set_title(title, fontsize=11)
        ax.set_ylabel("Thousand ops/s")
        ax.set_ylim(0, max(d["window"] for d in data.values()) / 1000 * 1.2)
        ax.yaxis.grid(True, color="#e8eaed")
        ax.set_axisbelow(True)
    axes[0].legend(loc="upper left", fontsize=8.5)
    fig.suptitle("Short runs overstate native RocksDB: it postpones compaction", fontweight="bold", fontsize=12, y=1.02)
    fig.tight_layout()
    save(fig, "4_deferred_compaction")


if __name__ == "__main__":
    figure_compaction_cpu()
    figure_compaction_speedup()
    figure_database()
    figure_deferred_compaction()
    print("wrote", sorted(p.name for p in OUT.glob("*.png")))
