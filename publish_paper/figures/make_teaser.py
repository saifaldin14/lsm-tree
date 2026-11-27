"""Figure 1: compaction CPU per GiB for each stage placement, by codec.

Reads only archived, reference-checked runs in results/data/prior-compaction
(128-byte values, one full compaction, RTX A6000). Run from any directory:
    python publish_paper/figures/make_teaser.py
"""
import json
import statistics
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = Path(__file__).resolve().parent
DATA = HERE.parents[1] / "results" / "data" / "prior-compaction"

CODECS = [("none", "No compression"), ("lz4", "LZ4"), ("zstd", "ZSTD")]
DESIGNS = [
    ("native", "RocksDB (CPU)", "#4d4d4d", ""),
    ("sortmerge", "GPU sort/merge", "#009E73", "xxx"),
    ("pipeline", "GPU pipeline, CPU compression", "#CC79A7", "\\\\\\"),
    ("ours", "Ours: full per-byte offload", "#D55E00", ""),
]


def load(codec):
    """Median CPU-s per GiB of input for each design; repetition 0 is warm-up."""
    samples = {}
    for suffix in ("s3", "sortmerge", "pipeline"):
        for line in (DATA / f"v128-{codec}-{suffix}.jsonl").read_text().splitlines():
            run = json.loads(line)
            if not run["matches_reference"]:
                raise SystemExit(f"{codec}-{suffix}: run differs from native RocksDB")
            if run["repetition"] == 0:
                continue
            if suffix == "s3":
                design = {"native_cpu": "native", "cuda_merge": "ours"}.get(run["mode"])
            else:
                design = suffix if run["mode"] != "native_cpu" else None
            if design is None:
                continue
            gib = run["input_bytes"] / 2**30
            samples.setdefault(design, []).append(run["process_cpu_seconds"] / gib)
    return {d: statistics.median(v) for d, v in samples.items()}


def main():
    values = {codec: load(codec) for codec, _ in CODECS}
    for codec, by in values.items():
        print(codec, {d: round(v, 2) for d, v in by.items()})

    plt.rcParams.update({"font.family": "sans-serif", "font.size": 7.5,
                         "axes.spines.top": False, "axes.spines.right": False,
                         "axes.linewidth": 0.6, "hatch.linewidth": 0.5})
    fig, ax = plt.subplots(figsize=(3.33, 1.85))
    width = 0.2
    for i, (key, label, color, hatch) in enumerate(DESIGNS):
        xs = [c + (i - 1.5) * width for c in range(len(CODECS))]
        ys = [values[codec].get(key, float("nan")) for codec, _ in CODECS]
        ax.bar(xs, ys, width, label=label, color=color, hatch=hatch,
               edgecolor="white", linewidth=0.4)
        if key == "ours":
            for x, y in zip(xs, ys):
                ax.text(x, y + 0.3, f"{y:.1f}", ha="center", va="bottom", fontsize=6.5)
    for c, (codec, _) in enumerate(CODECS):
        ratio = values[codec]["pipeline"] / values[codec]["ours"]
        top = max(values[codec].values())
        ax.text(c + 0.1, top + 1.2, f"{ratio:.1f}\u00d7", ha="center",
                fontsize=7.5, fontweight="bold", color="#1a1a1a")
    ax.set_xticks(range(len(CODECS)), [label for _, label in CODECS])
    ax.set_ylabel("Compaction CPU (s per GiB)")
    ax.set_ylim(0, max(max(v.values()) for v in values.values()) + 3.5)
    ax.yaxis.grid(True, color="#e6e6e6", linewidth=0.5)
    ax.set_axisbelow(True)
    ax.legend(fontsize=6.2, frameon=False, ncol=2, loc="upper left",
              bbox_to_anchor=(0, 1.32), handlelength=1.4, columnspacing=0.8)
    fig.savefig(HERE / "fig1_teaser.pdf", bbox_inches="tight")


if __name__ == "__main__":
    main()
