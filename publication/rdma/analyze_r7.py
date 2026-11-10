#!/usr/bin/env python3
"""GPU compaction over RDMA (rdma_campaign.py r7 and r8): per-hop time breakdown,
database-node CPU, figure fig23_rdma_gpu, and the whole-database runs of r8.

  python3 publication/rdma/analyze_r7.py [DATA_DIR]

DATA_DIR defaults to publication/data/rdma/r7; r8 is read from DATA_DIR/../r8-database.jsonl. Each record is one workload and variant; its
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
from make_figures import (DOUBLE_COLUMN, FULL, INK, NATIVE, bar, design_legend, finish_axis,  # noqa: E402
                          panel_label, save)

WORKLOADS = [("v128-lz4", "128 B values, LZ4"), ("v32-lz4", "32 B values, LZ4"), ("v128-zstd", "128 B values, ZSTD")]
SEGMENTS = {  # name: (label, colour, hatch)
    "db": ("Database node (read inputs, write outputs)", "#4d4d4d", ""),
    "native": ("Compaction on the database node's CPU", "#4d4d4d", ""),
    "rdma": ("RDMA (100 GbE)", "#E69F00", ""),
    "relay": ("Relay", "#9E7B00", ""),
    "gbe": ("TCP over 1 GbE", "#3399CC", "///"),
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
    elif variant == "remote-gpu-rdma":
        parts.update(rdma=remote - server, gpu=server)
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


def summarize_direct(data, summary):
    """Adds the direct testbed (database on pyke, GPU server on malphite, no relay) to summary."""
    for workload, variants in data.items():
        record = variants.get("remote-gpu-rdma")
        if not record or workload not in summary:
            continue
        native, remote = warm(record, "native_cpu"), warm(record, "remote_merge")
        per_run = [segments(r, "remote-gpu-rdma") for r in remote]
        summary[workload]["direct-native"] = {
            "wall": median([r["wall_seconds"] for r in native]),
            "cpu_gib": median([r["process_cpu_seconds_per_gib"] for r in native]),
            "parts": {"native": median([r["wall_seconds"] for r in native])}}
        summary[workload]["remote-gpu-rdma"] = {
            "wall": median([r["wall_seconds"] for r in remote]),
            "cpu_gib": median([r["process_cpu_seconds_per_gib"] for r in remote]),
            "parts": {k: median([p[k] for p in per_run]) for k in per_run[0]}}
        cpu_record = variants.get("remote-cpu-rdma")
        if cpu_record:
            runs = warm(cpu_record, "remote_merge")
            per_run = [segments(r, "remote-cpu-rdma") for r in runs]
            summary[workload]["direct-cpu-rdma"] = {
                "wall": median([r["wall_seconds"] for r in runs]),
                "cpu_gib": median([r["process_cpu_seconds_per_gib"] for r in runs]),
                "parts": {k: median([p[k] for p in per_run]) for k in per_run[0]}}


ROWS = [("native", "Native RocksDB"), ("remote-cpu-rdma", "Remote CPU server, RDMA"),
        ("remote-gpu-tcp", "Remote GPU, TCP (1 GbE)"),
        ("remote-gpu-relay", "Remote GPU, RDMA + relay"),
        ("direct-native", "Native RocksDB, direct testbed"),
        ("remote-gpu-rdma", "Remote GPU, direct RDMA")]
# Figure-only names; the summary table keeps ROWS.
FIGURE_LABELS = {"native": "RocksDB", "direct-native": "RocksDB (direct link)",
                 "remote-gpu-rdma": "Remote GPU, direct RDMA"}
LEGEND = ["db", "rdma", "relay", "gbe", "cpu_server", "gpu"]
# The direct testbed alone (README section 6, FULL-RESULTS.md section 10).
DIRECT_ROWS = [("direct-native", "RocksDB on the database machine"),
               ("direct-cpu-rdma", "Remote CPU server over RDMA"),
               ("remote-gpu-rdma", "Remote GPU over RDMA (ours)")]
DIRECT_LEGEND = ["db", "rdma", "cpu_server", "gpu"]


def figure_direct(summary, workloads):
    """README figure: RocksDB on the database machine against the remote GPU over RDMA,
    (a) time of one compaction and (b) the database machine's CPU per GiB, with the gain."""
    labels = {NATIVE: "RocksDB on the database machine", FULL: "Ours: compaction sent to a GPU in another machine"}
    fig, axes = plt.subplots(1, 2, figsize=(DOUBLE_COLUMN, 2.6))
    panels = [("wall", "Time of one compaction (s)", "{:.2f} s", "faster"),
              ("cpu_gib", "Database machine's CPU\nper GiB compacted (s)", "{:.2f}", "less CPU")]
    width = 0.38
    for ax, letter, (metric, ylabel, fmt, word) in zip(axes, "ab", panels):
        top = 0.0
        for index, (workload, _) in enumerate(workloads):
            native = summary[workload]["direct-native"][metric]
            ours = summary[workload]["remote-gpu-rdma"][metric]
            for x, value, design in ((index - width / 2, native, NATIVE), (index + width / 2, ours, FULL)):
                bar(ax, [x], [value], width * 0.95, design)
                ax.annotate(fmt.format(value), (x, value), xytext=(0, 1.5), textcoords="offset points",
                            ha="center", va="bottom", fontsize=6.2, color=INK)
            ax.annotate(f"{native / ours:.1f}× {word}", (index, native), xytext=(0, 12), textcoords="offset points",
                        ha="center", va="bottom", fontsize=7.2, fontweight="bold", color=INK)
            top = max(top, native)
        ax.set_xticks(range(len(workloads)), [title.replace(" values, ", " values\n") for _, title in workloads])
        ax.set_ylim(0, top * 1.35)
        finish_axis(ax, ylabel)
        panel_label(ax, letter)
    fig.tight_layout(w_pad=2.5, rect=(0, 0, 1, 0.88))
    design_legend(fig, (NATIVE, FULL), y=0.88, labels=labels)
    save(fig, "fig31_rdma_direct")


def figure(summary, workloads, rows_spec=ROWS, legend=LEGEND, out_name="fig23_rdma_gpu", divider=3.5):
    """Per workload: (a) where the time of one compaction goes, (b) database-node CPU per GiB."""
    ROWS = rows_spec  # noqa: N806
    rows = len(ROWS)
    height = 0.35 + 0.22 * rows
    fig, axes = plt.subplots(len(workloads), 2, figsize=(DOUBLE_COLUMN, 0.95 + height * len(workloads)),
                             sharey="row", squeeze=False, gridspec_kw={"width_ratios": [2.3, 1]})
    time_max = max(summary[w][k]["wall"] for w, _ in workloads for k, _ in ROWS if k in summary[w])
    cpu_max = max(summary[w][k].get("cpu_gib", 0) for w, _ in workloads for k, _ in ROWS if k in summary[w])
    for index, (workload, title) in enumerate(workloads):
        designs = summary[workload]
        left_ax, right_ax = axes[index]
        for y, (key, _) in enumerate(ROWS):
            entry = designs.get(key)
            if not entry:
                continue
            left = 0.0
            for name, value in entry["parts"].items():
                _, colour, hatch = SEGMENTS["db" if name == "native" else name]
                left_ax.barh(y, value, 0.74, left=left, color=colour, hatch=hatch, edgecolor="white",
                             linewidth=0.4, zorder=2)
                left += value
            left_ax.annotate(f"{entry['wall']:.2f} s", (left, y), xytext=(2, 0), textcoords="offset points", va="center",
                             fontsize=5.8, color=INK)
            colour = SEGMENTS["gpu"][1] if "gpu" in key else SEGMENTS["cpu_server"][1] if "cpu" in key else SEGMENTS["db"][1]
            if key in ("native", "direct-native"):
                colour = SEGMENTS["db"][1]
            hatch = SEGMENTS["gbe"][2] if key == "remote-gpu-tcp" else ""
            value = entry["cpu_gib"]
            right_ax.barh(y, value, 0.74, color=colour, hatch=hatch, edgecolor="white", linewidth=0.4, zorder=2)
            right_ax.annotate(f"{value:.2f}", (value, y), xytext=(2, 0), textcoords="offset points", va="center",
                              fontsize=5.8, color=INK)
        labels = [FIGURE_LABELS.get(key, label) if rows_spec is ROWS_ALL else label for key, label in ROWS]
        left_ax.set_yticks(range(rows), labels, fontsize=6.4)
        left_ax.set_ylim(rows - 0.45, -0.55)
        left_ax.set_xlim(0, time_max * 1.13)
        right_ax.set_xlim(0, cpu_max * 1.22)
        left_ax.set_title(title, loc="left", fontsize=7.5, fontweight="bold", color=INK, pad=3)
        for ax in (left_ax, right_ax):
            if divider is not None:
                # Rows below the line come from the second testbed (different machines).
                ax.axhline(divider, color="#999999", linewidth=0.6, linestyle="--", zorder=1)
            ax.xaxis.grid(True, color="#e6e6e6", linewidth=0.6, zorder=0)
            ax.set_axisbelow(True)
            ax.tick_params(axis="y", length=0)
            if index < len(workloads) - 1:
                ax.tick_params(axis="x", labelbottom=False)
    axes[-1][0].set_xlabel("Time of one compaction (s)")
    axes[-1][1].set_xlabel("Database-node CPU\n(s per GiB of input)")
    panel_label(axes[0][0], "a")
    panel_label(axes[0][1], "b")
    handles = [Patch(facecolor=SEGMENTS[n][1], hatch=SEGMENTS[n][2], edgecolor="white", label=SEGMENTS[n][0])
               for n in legend]
    handles[0] = Patch(facecolor=SEGMENTS["db"][1], edgecolor="white", label="On the database machine")
    top = 1 - 0.55 / (0.95 + height * len(workloads))
    fig.tight_layout(h_pad=0.6, w_pad=1.0, rect=(0, 0, 1, top))
    fig.legend(handles=handles, loc="lower center", ncol=len(handles), bbox_to_anchor=(0.5, top), fontsize=6.4,
               handlelength=1.4, columnspacing=1.0)
    save(fig, out_name)


ROWS_ALL = ROWS


def table(summary, workloads) -> str:
    lines = ["| Workload | Design | Compaction time (s) | Database-node CPU (s/GiB) | Hops (s) |",
             "|---|---|---:|---:|---|"]
    names = dict(ROWS) | {"gpu-host-native": "Native RocksDB on the GPU host",
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


R8_NAMES = {"native": "Native RocksDB", "remote_cpu_rdma": "Remote CPU server, RDMA",
            "remote_gpu_relay": "Remote GPU, RDMA + relay"}


def r8_table(path: Path) -> str:
    """Whole database (r8): medians over repetitions; every run verified all keys and values."""
    runs = {}
    for line in path.read_text().splitlines():
        record = json.loads(line)
        result = record["result"]
        if "verification" not in result:
            raise SystemExit(f"r8 {record['design']} repetition {record['repetition']}: not verified")
        runs.setdefault(record["design"], []).append(record)
    lines = ["| Design | Writes/s | Writes stalled | Database-node cores used | Database-node CPU per write (µs) "
             "| Remote jobs | Fallbacks | Runs |",
             "|---|---:|---:|---:|---:|---:|---:|---:|"]
    for design, label in R8_NAMES.items():
        records = runs.get(design, [])
        if not records:
            continue
        results = [r["result"] for r in records]
        ops = median([r["throughput"]["ops_per_second"] for r in results])
        cpu = median([r["accounting"]["window"]["cores"] * 1e6 / r["throughput"]["ops_per_second"] for r in results])
        stalled = median([r["stall_fraction"] for r in results])
        cores = median([r["accounting"]["window"]["cores"] for r in results])
        jobs = median([(r.get("transform") or {}).get("merge_jobs", 0) for r in results])
        fallbacks = max((r.get("transform") or {}).get("merge_fallbacks", 0) for r in results)
        lines.append(f"| {label} | {ops / 1e3:.0f}k | {stalled:.0%} | {cores:.2f} | {cpu:.2f} | {jobs:.0f} | {fallbacks} "
                     f"| {len(records)} |")
    return "\n".join(lines)


def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE.parent / "data" / "rdma" / "r7"
    data = load(root)
    summary = summarize(data)
    direct = root.parent / "r7-direct" / "out" / "r7"
    if direct.exists():
        summarize_direct(load(direct), summary)
    workloads = [(w, t) for w, t in WORKLOADS if w in summary and "remote-gpu-relay" in summary[w]]
    if not workloads:
        raise SystemExit("no complete workloads")
    figure(summary, workloads)
    direct_workloads = [(w, t) for w, t in workloads if "remote-gpu-rdma" in summary[w]]
    if direct_workloads:
        figure_direct(summary, direct_workloads)
        figure(summary, direct_workloads, DIRECT_ROWS, DIRECT_LEGEND, "fig34_rdma_direct_breakdown", None)
    text = table(summary, workloads)
    r8 = root.parent / "r8-database.jsonl"
    if r8.exists():
        text += ("\n\n## Whole database (r8)\n\nBatched writes (100 per batch, WAL off), 4 client threads and "
                 "4 background jobs on the database node; medians over repetitions; every run checked all keys "
                 "and values.\n\n" + r8_table(r8))
    (root / "summary.md").write_text("# GPU compaction over RDMA (r7)\n\nMedians of 3 warm repetitions; "
                                     "every run matched native RocksDB.\n\n" + text + "\n")
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
