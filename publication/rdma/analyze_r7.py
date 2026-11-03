#!/usr/bin/env python3
"""GPU compaction over RDMA (rdma_campaign.py r7): per-hop time breakdown, database-node CPU,
and figure fig23_rdma_gpu.

  python3 publication/rdma/analyze_r7.py [DATA_DIR]

DATA_DIR defaults to publication/data/rdma/r7. Each record is one workload and variant; its
rows are lsm_compaction_bench results (native first, then the offloaded mode) for 1 cold and 3
warm repetitions. Only warm repetitions are used, and any run whose output differs from native
RocksDB is refused.

Hops of one remote compaction, from the client's counters (sums over the jobs of one run):
  remote_ns            client's call (send request, wait, receive reply)
  server_ns            first server's handler time
  upstream_remote_ns   that server's own call to the next server (relay only)
  upstream_server_ns   the next server's handler time (relay only)
Time on the database node outside the call (reading inputs, installing outputs, writing files)
is wall - remote_ns. The GPU+RDMA estimate removes the measured relay and 1 GbE hops from the
measured end-to-end time: wall - (server_ns - upstream_server_ns).
"""

from __future__ import annotations

import json
import statistics
import sys
from pathlib import Path

import matplotlib.pyplot as plt
from matplotlib.patches import Patch

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
from make_figures import DOUBLE_COLUMN, INK, finish_axis, panel_label, save  # noqa: E402

WORKLOADS = [("v128-lz4", "128 B values, LZ4"), ("v32-lz4", "32 B values, LZ4"), ("v128-zstd", "128 B values, ZSTD")]
SEGMENTS = {  # name: (label, colour, hatch)
    "db": ("Database node (read inputs, write outputs)", "#8c8c8c", ""),
    "native": ("Compaction on the database node's CPU", "#8c8c8c", ""),
    "rdma": ("RDMA (100 GbE)", "#E69F00", ""),
    "relay": ("Relay", "#F0E442", ""),
    "gbe": ("TCP over 1 GbE", "#56B4E9", "///"),
    "cpu_server": ("Compaction server, CPU", "#0072B2", ""),
    "gpu": ("Compaction server, GPU", "#D55E00", ""),
    "loopback": ("Loopback TCP", "#CC79A7", ""),
}


def median(values):
    return statistics.median(values) if values else float("nan")


def delta(run, key):
    after, before = run.get("transform") or {}, run.get("transform_before") or {}
    return (after.get(key, 0) - before.get(key, 0)) / 1e9


def load(root: Path) -> dict:
    """{workload: {variant: record}}"""
    data = {}
    for path in sorted(root.glob("*.jsonl")):
        record = json.loads(path.read_text())
        for run in record["rows"]:
            if not run["matches_reference"]:
                raise SystemExit(f"{path.name}: {run['mode']} repetition {run['repetition']} differs from native")
        data.setdefault(record["workload"], {})[record["variant"]] = record
    return data


def warm(record, mode):
    return [run for run in record["rows"] if run["mode"] == mode and run["repetition"] > 0]


def segments(run, variant):
    """Seconds per hop of one remote run."""
    wall, remote, server = run["wall_seconds"], delta(run, "merge_remote_ns"), delta(run, "merge_server_ns")
    parts = {"db": wall - remote}
    if variant == "remote-gpu-relay":
        up_remote, up_server = delta(run, "merge_upstream_remote_ns"), delta(run, "merge_upstream_server_ns")
        parts.update(rdma=remote - server, relay=server - up_remote, gbe=up_remote - up_server, gpu=up_server)
    elif variant == "remote-cpu-rdma":
        parts.update(rdma=remote - server, cpu_server=server)
    elif variant == "remote-gpu-tcp":
        parts.update(gbe=remote - server, gpu=server)
    else:  # loopback on the GPU host
        parts.update(loopback=remote - server, gpu=server)
    return parts


def summarize(data):
    """{workload: {design: {"wall", "cpu_gib", "parts"}}} with medians over warm repetitions."""
    out = {}
    for workload, variants in data.items():
        designs = {}
        nasus = [r for v, r in variants.items() if v != "gpu-host"]
        native = [run for record in nasus for run in warm(record, "native_cpu")]
        if native:
            designs["native"] = {"wall": median([r["wall_seconds"] for r in native]),
                                 "cpu_gib": median([r["process_cpu_seconds_per_gib"] for r in native]),
                                 "parts": {"native": median([r["wall_seconds"] for r in native])}}
        for variant in ("remote-cpu-rdma", "remote-gpu-relay", "remote-gpu-tcp"):
            if variant not in variants:
                continue
            runs = warm(variants[variant], "remote_merge")
            per_run = [segments(r, variant) for r in runs]
            designs[variant] = {"wall": median([r["wall_seconds"] for r in runs]),
                                "cpu_gib": median([r["process_cpu_seconds_per_gib"] for r in runs]),
                                "parts": {k: median([p[k] for p in per_run]) for k in per_run[0]},
                                "gib": median([r["input_bytes"] / 2**30 for r in runs]),
                                "server_cpu": variants[variant].get("server_cpu_seconds"),
                                "server_statistics": variants[variant].get("server_statistics")}
            if variant == "remote-gpu-relay":
                estimate = [{"db": p["db"], "rdma": p["rdma"], "gpu": p["gpu"]} for p in per_run]
                designs["estimate"] = {"wall": median([sum(p.values()) for p in estimate]),
                                       "parts": {k: median([p[k] for p in estimate]) for k in estimate[0]}}
        if "gpu-host" in variants:
            record = variants["gpu-host"]
            for mode, name in (("native_cpu", "gpu-host-native"), ("cuda_merge", "gpu-host-local"),
                               ("remote_merge", "gpu-host-loopback")):
                runs = warm(record, mode)
                if runs:
                    designs[name] = {"wall": median([r["wall_seconds"] for r in runs]),
                                     "cpu_gib": median([r["process_cpu_seconds_per_gib"] for r in runs])}
                    if mode == "remote_merge":
                        per_run = [segments(r, "loopback") for r in runs]
                        designs[name]["parts"] = {k: median([p[k] for p in per_run]) for k in per_run[0]}
        out[workload] = designs
    return out


BARS = [("native", "Native RocksDB"), ("remote-cpu-rdma", "Remote CPU server, RDMA"),
        ("remote-gpu-relay", "Remote GPU, RDMA + relay (measured)"),
        ("estimate", "Remote GPU, RDMA only (estimate)")]
CPU_BARS = [("native", "Native RocksDB", "#8c8c8c", ""), ("remote-cpu-rdma", "Remote CPU server, RDMA", "#0072B2", ""),
            ("remote-gpu-relay", "Remote GPU, RDMA", "#D55E00", ""),
            ("remote-gpu-tcp", "Remote GPU, TCP", "#56B4E9", "///")]


def figure(summary, workloads):
    fig, axes = plt.subplots(1, 2, figsize=(DOUBLE_COLUMN, 3.1), gridspec_kw={"width_ratios": [1.7, 1]})
    ax = axes[0]
    labels, y = [], 0.0
    ticks = []
    for index, (workload, title) in enumerate(workloads):
        designs = summary[workload]
        for key, label in BARS:
            if key not in designs:
                continue
            left = 0.0
            for name, value in designs[key]["parts"].items():
                _, colour, hatch = SEGMENTS[name]
                ax.barh(y, value, 0.72, left=left, color=colour, hatch=hatch, edgecolor="white", linewidth=0.4,
                        zorder=2, alpha=0.55 if key == "estimate" else 1.0)
                left += value
            ax.annotate(f"{left:.1f} s", (left, y), xytext=(2, 0), textcoords="offset points", va="center",
                        fontsize=5.8, color=INK)
            ticks.append(y)
            labels.append(label)
            y += 1
        ax.text(0, y - len(BARS) - 0.75, title, fontsize=7, fontweight="bold", color=INK, va="bottom",
                transform=ax.transData)
        y += 1.0
    ax.set_yticks(ticks, labels, fontsize=6.3)
    ax.invert_yaxis()
    ax.set_xlabel("Time of one compaction (s)")
    ax.xaxis.grid(True, color="#e6e6e6", linewidth=0.6, zorder=0)
    ax.set_axisbelow(True)
    ax.tick_params(axis="y", length=0)
    panel_label(ax, "a")

    ax = axes[1]
    width = 0.2
    for index, (workload, _) in enumerate(workloads):
        for offset, (key, label, colour, hatch) in enumerate(CPU_BARS):
            value = summary[workload].get(key, {}).get("cpu_gib")
            if value is None:
                continue
            x = index + (offset - 1.5) * width
            ax.bar(x, value, width * 0.92, color=colour, hatch=hatch, edgecolor="white", linewidth=0.4, zorder=2)
            ax.annotate(f"{value:.1f}", (x, value), xytext=(0, 1.2), textcoords="offset points", ha="center",
                        va="bottom", fontsize=5.4, color=INK)
    ax.set_xticks(range(len(workloads)), [title.replace(" values, ", "\n") for _, title in workloads])
    finish_axis(ax, "Database-node CPU (s per GiB of input)")
    panel_label(ax, "b")

    used = []
    for designs in (summary[w] for w, _ in workloads):
        for key, _ in BARS:
            for name in designs.get(key, {}).get("parts", {}):
                if name not in used:
                    used.append(name)
    handles = [Patch(facecolor=SEGMENTS[n][1], hatch=SEGMENTS[n][2], edgecolor="white", label=SEGMENTS[n][0])
               for n in used]
    handles += [Patch(facecolor=c, hatch=h, edgecolor="white", label=f"(b) {l}") for _, l, c, h in CPU_BARS]
    fig.tight_layout(w_pad=2.0, rect=(0, 0, 1, 0.80))
    fig.legend(handles=handles, loc="lower center", ncol=3, bbox_to_anchor=(0.5, 0.80), fontsize=6.4,
               handlelength=1.4, columnspacing=1.0)
    save(fig, "fig23_rdma_gpu")


def table(summary, workloads) -> str:
    lines = ["| Workload | Design | Compaction time (s) | Database-node CPU (s/GiB) | Hops (s) |",
             "|---|---|---:|---:|---|"]
    names = dict(BARS) | {"remote-gpu-tcp": "Remote GPU, TCP 1 GbE (no relay)",
                          "gpu-host-native": "Native RocksDB on the GPU host",
                          "gpu-host-local": "GPU in the database process (GPU host)",
                          "gpu-host-loopback": "Remote GPU over loopback TCP (GPU host)"}
    for workload, title in workloads:
        for key, label in names.items():
            entry = summary[workload].get(key)
            if not entry:
                continue
            parts = ", ".join(f"{name} {value:.2f}" for name, value in entry.get("parts", {}).items())
            cpu = f"{entry['cpu_gib']:.2f}" if "cpu_gib" in entry else "-"
            lines.append(f"| {title} | {label} | {entry['wall']:.2f} | {cpu} | {parts} |")
    return "\n".join(lines)


def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE.parent / "data" / "rdma" / "r7"
    data = load(root)
    summary = summarize(data)
    workloads = [(w, t) for w, t in WORKLOADS if w in summary and "remote-gpu-relay" in summary[w]]
    if not workloads:
        raise SystemExit("no complete workloads")
    figure(summary, workloads)
    text = table(summary, workloads)
    (root / "summary.md").write_text("# GPU compaction over RDMA (r7)\n\nMedians of 3 warm repetitions; "
                                     "every run matched native RocksDB.\n\n" + text + "\n")
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
