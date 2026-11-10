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
import math
import statistics
import sys
from pathlib import Path

import matplotlib.pyplot as plt
from matplotlib.colors import to_rgba
from matplotlib.lines import Line2D
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
    fig, axes = plt.subplots(1, 2, figsize=(DOUBLE_COLUMN, 3.0), gridspec_kw={"width_ratios": [1, 1.25]})
    for ax, letter, (metric, ylabel, fmt) in zip(axes, "ab", [("ops", "Throughput (M writes/s)", lambda v: f"{v:.3f}"),
                                                            ("cpu", "CPU per write (µs)", lambda v: f"{v:.1f}")]):
        top = grouped_bars(ax, groups[metric], (NATIVE, FULL), None, fmt, size=6.4)
        ax.tick_params(axis="x", labelsize=6.4)
        finish_axis(ax, ylabel)
        panel_label(ax, letter)
        if metric == "ops":
            native, full = REPORTED_NVME_OPS["native"], REPORTED_NVME_OPS["full"]
            bracket_y = full * 1.23
            ax.plot([-0.2, -0.2, 0.2, 0.2], [native * 1.22, bracket_y, bracket_y, full * 1.1],
                    color=INK, linewidth=0.8)
            ax.text(0, bracket_y * 1.02, f"{full / native:.2f}× more writes", ha="center", va="bottom",
                    fontsize=7.5, fontweight="bold", color=INK)
            ax.set_ylim(0, bracket_y * 1.22)
    fig.tight_layout(w_pad=2.5, rect=(0, 0, 1, 0.9))
    design_legend(fig, (NATIVE, FULL), y=0.9, labels=LABELS)
    save(fig, "fig25_nvme")


DISK_METRICS = ("read_iops", "read_mib_s", "write_iops", "write_mib_s", "busy_percent")


def iostat_series(path, device="nvme0n1", *, since_boot=False):
    """Return (read IOPS, read MiB/s, write IOPS, write MiB/s, busy %) per report.

    The campaign uses -y, so every report is an interval by default. For logs
    without -y, since_boot=True explicitly discards the initial boot average.
    Sysstat labels binary units kB/MB: divide rkB/s and wkB/s by 1024.
    Malformed or missing device reports raise rather than shifting later bins.
    """
    path = Path(path)
    try:
        text = path.read_text()
    except (OSError, UnicodeError) as error:
        raise ValueError(f"{path}: missing or unreadable iostat recording") from error
    rows, header, columns, bandwidth = [], None, {}, []
    found_in_report = False
    for line_number, line in enumerate(text.splitlines(), 1):
        parts = line.split()
        if not parts:
            continue
        location = f"{path}:{line_number}"
        if parts[0].rstrip(":") == "Device":
            if header is not None and not found_in_report:
                raise ValueError(f"{location}: device {device} absent from preceding report")
            header = parts
            if len(set(header)) != len(header) or not {"r/s", "w/s", "%util"}.issubset(header):
                raise ValueError(f"{location}: invalid iostat header (need unique r/s, w/s, %util columns)")
            columns = {name: index for index, name in enumerate(header)}
            bandwidth = []
            for prefix in ("r", "w"):
                choices = [(name, scale) for name, scale in ((f"{prefix}MB/s", 1.0), (f"{prefix}kB/s", 1 / 1024))
                           if name in columns]
                if len(choices) != 1:
                    raise ValueError(f"{location}: header needs exactly one of {prefix}MB/s or {prefix}kB/s")
                bandwidth.append(choices[0])
            found_in_report = False
        elif parts[0] == device:
            if header is None:
                raise ValueError(f"{location}: missing Device header before {device}")
            try:
                if len(parts) != len(header):
                    raise ValueError("column count differs from header")
                values = [float(value) for value in parts[1:]]
                if any(not math.isfinite(value) or value < 0 for value in values):
                    raise ValueError("nonfinite or negative value")
                read, write = bandwidth
                rows.append((values[columns["r/s"] - 1], values[columns[read[0]] - 1] * read[1],
                             values[columns["w/s"] - 1], values[columns[write[0]] - 1] * write[1],
                             values[columns["%util"] - 1]))
            except ValueError as error:
                raise ValueError(f"{location}: malformed {device} row: {error}") from error
            found_in_report = True
    if header is None:
        raise ValueError(f"{path}: missing Device header")
    if not found_in_report:
        raise ValueError(f"{path}: device {device} absent from final report")
    if since_boot:
        rows = rows[1:]
    if not rows:
        raise ValueError(f"{path}: no interval samples for {device}")
    return rows


def _telemetry_number(value, field):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value < 0:
        raise ValueError(f"{field}: expected a finite nonnegative number")
    return float(value)


def _verification_reason(result):
    if not isinstance(result, dict):
        return "missing result"
    verification = result.get("verification")
    keys = verification.get("keys") if isinstance(verification, dict) else None
    if type(keys) is not int or keys < 0:
        return "missing or invalid result.verification.keys"
    return None


def iostat_window(rows, record):
    """Select full 1 s bins [started_unix + i, started_unix + i + 1].

    No per-report timestamps were saved. Alignment is approximate at 1 s
    resolution, anchored to monitor launch, with no inferred I/O start.
    """
    trial = f"trial {record.get('index', '?')}"
    try:
        monitor = _telemetry_number(record["started_unix"], "record.started_unix timestamp")
        window = record["result"]["window"]
        start = _telemetry_number(window["start_unix"], "result.window.start_unix timestamp")
        end = _telemetry_number(window["end_unix"], "result.window.end_unix timestamp")
    except (KeyError, TypeError) as error:
        raise ValueError(f"{trial}: missing record.started_unix or result.window.start_unix/end_unix") from error
    if end <= start:
        raise ValueError(f"{trial}: measurement window must end after it starts")
    if not rows or start < monitor or end > monitor + len(rows):
        raise ValueError(f"{trial}: insufficient iostat recording [{monitor}, {monitor + len(rows)}] "
                         f"for measurement window [{start}, {end}]")
    first, stop = math.ceil(start - monitor), math.floor(end - monitor)
    if first >= stop:
        raise ValueError(f"{trial}: no full measurement bins inside [{start}, {end}]")
    return {"samples": rows[first:stop], "relative_edges_seconds": [monitor + i - start for i in range(first, stop + 1)],
            "sample_count": stop - first, "recorded_samples": len(rows), "first_bin_index": first,
            "stop_bin_index": stop, "monitor_start_unix": monitor, "window_start_unix": start, "window_end_unix": end}


def load_disk_runs(root, device="nvme0n1"):
    """Load verified N6 runs; reject incomplete recordings and report exclusions."""
    root = Path(root)
    path = root / "results.jsonl"
    try:
        text = path.read_text()
    except OSError as error:
        raise ValueError(f"{path}: missing or unreadable results recording") from error
    series, excluded, seen, indices = {}, [], set(), set()
    for line_number, line in enumerate(text.splitlines(), 1):
        if not line.strip():
            continue
        try:
            record = json.loads(line)
        except ValueError as error:
            raise ValueError(f"{path}:{line_number}: malformed JSON record") from error
        if not isinstance(record, dict):
            raise ValueError(f"{path}:{line_number}: expected a JSON record object")
        code = record.get("exit_code")
        reason = (f"exit_code={code!r} (not a successful run)" if type(code) is not int or code != 0
                  else _verification_reason(record.get("result")))
        if reason:
            excluded.append({"index": record.get("index"), "reason": reason})
            print(f"{path}: excluded trial {record.get('index', '?')}: {reason}", file=sys.stderr)
            continue
        try:
            index, block = record["index"], record["trial"]["block"]
            design = DESIGN[record["trial"]["design"]]
        except (KeyError, TypeError) as error:
            raise ValueError(f"{path}:{line_number}: missing or invalid trial index/design/block") from error
        if any(type(value) is not int or value < 0 for value in (index, block)):
            raise ValueError(f"{path}:{line_number}: trial index/block must be nonnegative integers")
        if index in indices or (design, block) in seen:
            raise ValueError(f"{path}: duplicate trial index or design/block: {index}, {design}, {block}")
        indices.add(index)
        seen.add((design, block))
        selected = iostat_window(iostat_series(root / f"trial-{index:04d}" / "iostat.txt", device), record)
        means = {metric: statistics.mean(row[i] for row in selected["samples"]) for i, metric in enumerate(DISK_METRICS)}
        series.setdefault(design, []).append({"index": index, "block": block, **selected, "means": means})
    for runs in series.values():
        runs.sort(key=lambda run: (run["block"], run["index"]))
    return series, excluded


def disk_summary(series, excluded, device="nvme0n1"):
    """JSON-ready medians of per-run measurement-bin means, including zero reads."""
    designs = {}
    for design in (NATIVE, FULL):
        runs = series.get(design, [])
        if not runs:
            continue
        details = []
        for run in runs:
            details.append({**{key: value for key, value in run.items() if key not in ("samples", "relative_edges_seconds")},
                            "first_bin_start_seconds": run["relative_edges_seconds"][0],
                            "last_bin_end_seconds": run["relative_edges_seconds"][-1]})
        designs[design] = {"n": len(runs), "sample_counts": [r["sample_count"] for r in runs], "runs": details,
                           "median_per_run_mean": {key: median([r["means"][key] for r in runs]) for key in DISK_METRICS}}
    counts = "; ".join(f"{LABELS[d]} n={v['n']}, full bins/run={'/'.join(map(str, v['sample_counts']))}"
                       for d, v in designs.items())
    return {"device": device, "sample_interval_seconds": 1,
            "source_fields": ["record.started_unix", "result.window.start_unix", "result.window.end_unix"],
            "alignment": "Approximate 1 s resolution: bin i is [monitor start + i, monitor start + i + 1]; full bins only.",
            "bandwidth_unit": "MiB/s: iostat rMB/s/wMB/s unchanged; rkB/s/wkB/s divided by 1024.",
            "aggregation": "Median across runs of each run's mean over full measurement bins, including zeros.",
            "metadata": f"{counts}. Excluded runs: {len(excluded)}. Thin ticks show individual run means.",
            "limits": "Device counters include other I/O and omit cache-served reads. Busy % alone does not prove saturation.",
            "designs": designs, "excluded": excluded}


def figure_disk():
    """N6: first paired block 0 traces and medians of measurement-bin run means."""
    series, excluded = load_disk_runs(HERE / "data" / "malphite-nvme" / "followup" / "n6")
    summary = disk_summary(series, excluded)
    pair = {}
    for design in (NATIVE, FULL):
        candidates = [run for run in series.get(design, []) if run["block"] == 0]
        if len(candidates) != 1:
            raise ValueError(f"N6: need one verified block 0 trial with complete recording for {LABELS[design]}")
        pair[design] = candidates[0]
    summary["trace_pair"] = {"block": 0, "trials": {d: r["index"] for d, r in pair.items()}}
    panels = [("Bandwidth", "Bandwidth (MiB/s)", [("Read", 1, "--"), ("Write", 3, "-")]),
              ("IOPS", "Read / write IOPS", [("Read", 0, "--"), ("Write", 2, "-")]),
              ("Device busy", "Device busy (%)", [("Busy", 4, "-")])]
    fig, axes = plt.subplots(2, 3, figsize=(DOUBLE_COLUMN * 1.25, 5.3))
    width = 0.36
    for column, (title, ylabel, metrics) in enumerate(panels):
        ax = axes[0, column]
        for design in (NATIVE, FULL):
            run = pair[design]
            for label, i, linestyle in metrics:
                ax.stairs([row[i] for row in run["samples"]], run["relative_edges_seconds"], baseline=None,
                          color=STYLE[design]["color"], linestyle=linestyle, linewidth=0.8,
                          label=f"{LABELS[design]} {label.lower()}")
        ax.set_xlim(0, max(run["window_end_unix"] - run["window_start_unix"] for run in pair.values()))
        ax.set_ylim(0, max(100 if column == 2 else 1, ax.get_ylim()[1] * 1.05))
        ax.set_xlabel("Seconds from measurement start", fontsize=7)
        ax.set_title(f"{title}, block 0", fontsize=8, pad=9)
        finish_axis(ax, ylabel)
        panel_label(ax, "abc"[column])
        ax = axes[1, column]
        high = 0.0
        for group, (label, i, _) in enumerate(metrics):
            for offset, design in enumerate((NATIVE, FULL)):
                x = offset if len(metrics) == 1 else group + (offset - 0.5) * width
                values = [run["means"][DISK_METRICS[i]] for run in series[design]]
                value = summary["designs"][design]["median_per_run_mean"][DISK_METRICS[i]]
                color = STYLE[design]["color"]
                ax.bar(x, value, width * 0.95, facecolor=to_rgba(color, 0.3 if label == "Read" else 1),
                       edgecolor=color, hatch="///" if label == "Read" else "", linewidth=0.5, zorder=2)
                spacing = min(0.06, 0.22 / max(1, len(values) - 1))
                xs = [x + (j - (len(values) - 1) / 2) * spacing for j in range(len(values))]
                ax.scatter(xs, values, marker="|", s=28, color=INK, linewidths=0.65, zorder=4, clip_on=False)
                if 0 < value < 0.001:
                    value_label = f"{value:.2e}"
                else:
                    value_label = f"{value:.3g}" if value < 1 else f"{value:.1f}"
                ax.annotate(value_label, (x, max(values)), xytext=(0, 3),
                            textcoords="offset points", ha="center", va="bottom", fontsize=6.3, color=INK)
                high = max(high, *values)
        labels = [LABELS[NATIVE], LABELS[FULL]] if len(metrics) == 1 else [label for label, _, _ in metrics]
        ax.set_xticks(range(len(labels)), labels)
        ax.set_ylim(0, max(100 if column == 2 else 1, high * 1.22))
        ax.set_title("Median of per-run means", fontsize=8, pad=9)
        finish_axis(ax, ylabel)
        panel_label(ax, "def"[column])
    handles = [Patch(facecolor=STYLE[d]["color"], label=LABELS[d]) for d in (NATIVE, FULL)]
    handles += [Line2D([], [], color=INK, linestyle="--", label="Read (dashed; hatched bars)"),
                Line2D([], [], color=INK, label="Write (solid)")]
    fig.legend(handles=handles, loc="upper center", ncol=4, bbox_to_anchor=(0.5, 1.0), fontsize=7)
    fig.text(0.5, 0.01, summary["metadata"] + "\nAssumed 1 s bins from monitor start; approximate 1 s alignment. "
             "Only full bins inside each recorded measurement window.\n" + summary["limits"],
             ha="center", va="bottom", fontsize=6.6, color="#555555")
    fig.tight_layout(w_pad=2.0, h_pad=2.5, rect=(0, 0.14, 1, 0.93))
    save(fig, "fig27_nvme_disk")
    return summary


CPU_ROLES = [("foreground", "Accepting writes", 1.0, ""), ("flush", "Flushing", 0.72, "///"),
             ("compaction", "Compaction", 0.48, "..."), ("other", "Other (attributed)", 0.3, "xx"),
             ("unattributed", "Unattributed", 0.15, "\\\\")]
CPU_ROUNDING_TOLERANCE = 1e-6  # Absolute cores, not a percentage of the budget.


def cpu_run_components(result, cores):
    """Reconcile partial thread roles to process CPU for one measurement window."""
    budget = _telemetry_number(cores, "core budget")
    if budget == 0:
        raise ValueError("core budget must be positive")
    try:
        used = _telemetry_number(result["process_cpu"]["cores_used"], "result.process_cpu.cores_used")
        raw_roles = result["accounting"]["window"]["cores_by_role"]
    except (KeyError, TypeError) as error:
        raise ValueError("missing result.process_cpu.cores_used or accounting.window.cores_by_role") from error
    if not isinstance(raw_roles, dict):
        raise ValueError("accounting.window.cores_by_role must be an object")
    roles = {key: _telemetry_number(value, f"accounting.window.cores_by_role.{key}") for key, value in raw_roles.items()}
    attributed = math.fsum(roles.values())
    if attributed - used > CPU_ROUNDING_TOLERANCE:
        raise ValueError(f"accounting.window roles total {attributed:.9g} exceeds process_cpu.cores_used {used:.9g}")
    if used - budget > CPU_ROUNDING_TOLERANCE:
        raise ValueError(f"process_cpu.cores_used {used:.9g} exceeds core budget {budget:g}")
    main = ("foreground", "flush", "compaction")
    components = {key: roles.get(key, 0.0) for key in main}
    components["other"] = math.fsum(value for key, value in roles.items() if key not in main)
    correction = max(attributed - used, 0.0)
    if correction:
        largest = max(components, key=lambda key: components[key])
        components[largest] = max(0.0, components[largest] - correction)
    components["unattributed"] = max(used - attributed, 0.0)
    return {"process_cores_used": used, "components": components, "unused_core_budget": max(budget - used, 0.0),
            "rounding_adjustment_cores": correction}


def cpu_use_summary(headline):
    """JSON-ready arithmetic means; headline is the result mapping from load_campaign."""
    groups, excluded = [], []
    cores = sorted({c for d, c in headline if d in (NATIVE, FULL) and c in (4, 8, 16)})
    for c in cores:
        for design in (NATIVE, FULL):
            runs = []
            for index, result in enumerate(headline.get((design, c), [])):
                reason = _verification_reason(result)
                if reason:
                    excluded.append({"design": design, "cores": c, "run_index": index, "reason": reason})
                    print(f"CPU {LABELS[design]} {c} cores: excluded run {index}: {reason}", file=sys.stderr)
                    continue
                try:
                    runs.append(cpu_run_components(result, c))
                except ValueError as error:
                    raise ValueError(f"CPU {LABELS[design]} {c} cores, run {index}: {error}") from error
            if not runs:
                raise ValueError(f"CPU: no verified runs for {LABELS[design]} at {c} cores")
            groups.append({"design": design, "cores": c, "n": len(runs),
                           "process_cores_used": statistics.mean(r["process_cores_used"] for r in runs),
                           "components": {key: statistics.mean(r["components"][key] for r in runs) for key, *_ in CPU_ROLES},
                           "unused_core_budget": statistics.mean(r["unused_core_budget"] for r in runs),
                           "rounding_adjustment_cores": statistics.mean(r["rounding_adjustment_cores"] for r in runs),
                           "runs": runs})
    counts = "; ".join(f"{c} cores n=" + "/".join(str(g["n"]) for g in groups if g["cores"] == c) for c in cores)
    return {"groups": groups, "excluded": excluded,
            "source_fields": ["result.process_cpu.cores_used", "result.accounting.window.cores_by_role"],
            "aggregation": "Arithmetic means over runs of process CPU, each attributed role, and per-run residuals.",
            "rounding_tolerance_cores": CPU_ROUNDING_TOLERANCE,
            "metadata": f"Means over runs (RocksDB/Ours): {counts}. Excluded results: {len(excluded)}.",
            "limits": "Thread roles are partial: exited threads can disappear. Other includes all remaining attributed roles. "
                      "Unattributed is max(process CPU minus all roles, 0) per run. Unused core budget is not machine idle."}


def figure_cpu_use(headline):
    """Paired process-CPU stacks and unused process core budgets at 4, 8 and 16 cores."""
    summary = cpu_use_summary(headline)
    groups = summary["groups"]
    if not groups:
        return
    cores = sorted({group["cores"] for group in groups})
    fig, ax = plt.subplots(figsize=(DOUBLE_COLUMN * 1.1, 4.1))
    width = 0.36
    for group in groups:
        design, c = group["design"], group["cores"]
        x = cores.index(c) + ((0 if design == NATIVE else 1) - 0.5) * width
        color, bottom = STYLE[design]["color"], 0.0
        for key, _, alpha, hatch in CPU_ROLES:
            value = group["components"][key]
            ax.bar(x, value, width * 0.95, bottom=bottom, facecolor=to_rgba(color, alpha),
                   edgecolor=color, hatch=hatch, linewidth=0.4, zorder=2)
            bottom += value
        ax.bar(x, group["unused_core_budget"], width * 0.95, bottom=group["process_cores_used"], facecolor="white",
               edgecolor=color, linewidth=0.7, linestyle="--", zorder=2)
        ax.annotate(f"{group['process_cores_used']:.2f}", (x, group["process_cores_used"]), xytext=(0, 3),
                    textcoords="offset points", ha="center", va="bottom", fontsize=7, color=INK, bbox=LABEL_BOX)
    ax.set_xticks(range(len(cores)), [f"{c} cores" for c in cores])
    ax.set_ylim(0, max(cores) * 1.12)
    finish_axis(ax, "Process CPU use (cores) / unused core budget")
    handles = [Patch(facecolor=to_rgba("#777777", alpha), edgecolor="#777777", hatch=hatch, label=label)
               for _, label, alpha, hatch in CPU_ROLES]
    handles.append(Patch(facecolor="white", edgecolor="#777777", linestyle="--", label="Unused core budget"))
    fig.legend(handles=handles, frameon=False, fontsize=7, loc="lower center", ncol=3, bbox_to_anchor=(0.5, 0.78))
    fig.text(0.5, 0.01, summary["metadata"] + "\nRole accounting is partial; Other includes all remaining attributed roles. "
             "Unattributed is the per-run process residual.\nLabels give process cores used. "
             "The upper outline is unused process core budget, not machine idle.",
             ha="center", va="bottom", fontsize=6.5, color="#555555")
    fig.tight_layout(rect=(0, 0.14, 1, 0.77))
    design_legend(fig, (NATIVE, FULL), y=0.93, labels=LABELS)
    save(fig, "fig28_cpu_use")
    return summary


def figure_nvme_regimes():
    """H31: measured NVMe throughput, process CPU and write amplification for all cells."""
    path = HERE / "data" / "malphite-nvme" / "h31" / "results.jsonl"
    if not path.exists():
        return
    runs = {}
    for line in path.read_text().splitlines():
        if not line.strip():
            continue
        record = json.loads(line)
        result = record.get("result", {})
        if record.get("exit_code") != 0 or _verification_reason(result):
            raise ValueError(f"H31: failed or unverified trial {record.get('index')}")
        trial = record["trial"]
        runs.setdefault((trial["cell"], DESIGN[trial["design"]]), []).append(result)
    cells = [("C4", "4 cores\nLZ4"), ("BZ", "8 cores\nZSTD\nbottom level"),
             ("TR", "8 cores\nL0: 8 / 12")]
    metrics = [("Throughput (M writes/s)", lambda r: window_ops(r) / 1e6, "{:.3f}"),
               ("Process CPU per write (µs)", lambda r: r["process_cpu"]["cpu_microseconds_per_op"], "{:.2f}"),
               ("Write amplification (bytes / user byte)", lambda r: r["write_amplification"], "{:.2f}")]
    fig, axes = plt.subplots(1, 3, figsize=(DOUBLE_COLUMN * 1.25, 3.3))
    width = 0.36
    for ax, letter, (ylabel, metric, fmt) in zip(axes, "abc", metrics):
        for index, (cell, _) in enumerate(cells):
            for offset, design in enumerate((NATIVE, FULL)):
                values = [metric(r) for r in runs.get((cell, design), [])]
                if not values:
                    raise ValueError(f"H31: missing {cell} {design}")
                x = index + (offset - 0.5) * width
                bar(ax, [x], [median(values)], width * 0.95, design)
                ax.scatter([x] * len(values), values, marker="_", s=30, color=INK, linewidths=0.8, zorder=4)
                ax.annotate(fmt.format(median(values)), (x, max(values)), xytext=(0, 4), textcoords="offset points",
                            ha="center", va="bottom", fontsize=6.2, color=INK)
        ax.set_xticks(range(len(cells)), [label for _, label in cells], fontsize=6)
        ax.set_ylim(0, ax.get_ylim()[1] * 1.15)
        finish_axis(ax, ylabel)
        panel_label(ax, letter)
    fig.tight_layout(w_pad=2.0, rect=(0, 0.08, 1, 0.86))
    design_legend(fig, (NATIVE, FULL), y=0.86, labels=LABELS)
    counts = "; ".join(f"{cell}: " + "/".join(str(len(runs[(cell, d)])) for d in (NATIVE, FULL)) for cell, _ in cells)
    fig.text(0.5, 0.005, f"Runs (RocksDB/Ours): {counts}. Bars: medians; ticks: individual runs. Buffered I/O, WAL off.",
             ha="center", va="bottom", fontsize=6.4, color="#555555")
    save(fig, "fig29_nvme_regimes")


def figure_nvme_fixed_rates():
    path = HERE / "data" / "malphite-nvme" / "followup" / "n5" / "results.jsonl"
    if path.exists():
        runs = {}
        load_into(runs, "N5", path)
        figure_tail(runs, output="fig30_nvme_latency")


def figure_tail(openloop, output="fig20_v3_tail"):
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
    save(fig, output)


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
    figure_nvme_regimes()
    figure_nvme_fixed_rates()
    figure_tail(openloop)
    figure_workloads()
    print((V3 / "summary.md").read_text())
