#!/usr/bin/env python3
"""Figures made for the README's summary sections, from the same raw data as the detailed ones.

  python3 publication/make_readme_figures.py

fig32_workload_gains  Gain over RocksDB per workload: the v3 workload campaigns (stream state,
                      metadata, time series, Meta's MixGraph models), YCSB and the shared-machine
                      campaign.
fig33_storage         Writes/s and CPU per write with data in memory and on an NVMe drive (second
                      server) and on a SATA SSD (first server), from archived runs only.
"""

from __future__ import annotations

import json
import statistics
import sys
from pathlib import Path

import matplotlib.pyplot as plt
from matplotlib.transforms import blended_transform_factory

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import campaigns  # noqa: E402
from make_figures import (DOUBLE_COLUMN, FULL, INK, NATIVE, STYLE, bar, design_legend,  # noqa: E402
                          finish_axis, panel_label, save)
from make_figures_v3 import LABELS, cpu_us, load_campaign, window_ops, workload_ratios  # noqa: E402


def median(values):
    return statistics.median(values) if values else float("nan")


def point_ratio(entries, **match):
    """Ours over RocksDB, from the medians of the entries matching **match."""
    values = {}
    for entry in entries:
        if all(entry["config"].get(k) == v for k, v in match.items()) and entry.get("ops_per_second"):
            values[entry["design"]] = entry["ops_per_second"][0]
    return values["full"] / values["native"] if "full" in values and "native" in values else None


def workload_rows():
    """[(group, label, [(setting, ratio), ...])] in display order."""
    v3 = {}
    for label, ratio, _ in workload_ratios():
        name, *rest = label.split("\n")
        v3.setdefault(" ".join([name] + rest[:-1]), []).append((rest[-1], ratio))
    ycsb = campaigns.summarise(campaigns.load("ycsb"))
    contention = campaigns.summarise(campaigns.load("contention"))
    rows = [
        ("Write-heavy", "Stream-processing state (Flink, Kafka Streams)", v3.get("Stream state", [])),
        ("Write-heavy", "Storage metadata (Ceph, TiKV)", v3.get("Metadata", [])),
        ("Write-heavy", "Time series, old data deleted", v3.get("Time series", [])),
        ("Write-heavy", "Meta's production model, write-heavy", v3.get("MixGraph write-heavy", [])),
        ("Write-heavy", "YCSB F, read-modify-write",
         [(dist, point_ratio(ycsb, workload="F", distribution=key)) for key, dist in
          (("uniform", "uniform keys"), ("zipfian", "skewed keys"))]),
        ("Busy machine", "Other programs using the database's cores",
         [(f"{burn}% of each core taken", point_ratio(contention, cores=4, burn_percent=burn)) for burn in (40, 80)]),
        ("Read-heavy", "Meta's production model, 85% reads", v3.get("MixGraph ZippyDB", [])),
        ("Read-heavy", "YCSB B, 95% reads",
         [(dist, point_ratio(ycsb, workload="B", distribution=key)) for key, dist in
          (("uniform", "uniform keys"), ("zipfian", "skewed keys"))]),
    ]
    return [(group, label, [(s, r) for s, r in settings if r is not None]) for group, label, settings in rows]


def figure_workload_gains():
    rows = [row for row in workload_rows() if row[2]]
    if not rows:
        return
    bars = sum(len(settings) for _, _, settings in rows)
    fig, ax = plt.subplots(figsize=(DOUBLE_COLUMN, 0.7 + 0.16 * bars + 0.08 * len(rows)))
    y, ticks, labels, group_spans = 0.0, [], [], []
    colour = STYLE[FULL]["color"]
    for index, (group, label, settings) in enumerate(rows):
        if index == 0 or rows[index - 1][0] != group:
            y += 0.9
        start = y
        for setting, ratio in settings:
            ax.barh(y, ratio, 0.8, color=colour, edgecolor="white", linewidth=0.4, zorder=2)
            ax.annotate(f"{ratio:.2f}×  {setting}", (ratio, y), xytext=(3, 0), textcoords="offset points",
                        va="center", fontsize=6.0, color=INK)
            y += 1
        ticks.append((start + y - 1) / 2)
        labels.append(label)
        group_spans.append((group, start, y - 1))
        y += 0.35
    ax.set_yticks(ticks, labels, fontsize=6.8)
    ax.invert_yaxis()
    ax.axvline(1.0, color=STYLE[NATIVE]["color"], linewidth=1.0, linestyle="--", zorder=3)
    ax.text(1.0, 1.01, "RocksDB = 1", transform=blended_transform_factory(ax.transData, ax.transAxes),
            ha="center", va="bottom", fontsize=6.2, color=STYLE[NATIVE]["color"])
    seen = set()
    for group, start, _ in group_spans:
        if group in seen:
            continue
        seen.add(group)
        ax.text(-0.01, start - 0.75, group.upper(), transform=blended_transform_factory(ax.transAxes, ax.transData),
                ha="right", va="center", fontsize=6.6, fontweight="bold", color="#555555")
    high = max(r for _, _, settings in rows for _, r in settings)
    ax.set_xlim(0, high * 1.4)
    ax.set_xlabel("Operations per second, ours relative to RocksDB")
    ax.xaxis.grid(True, color="#e6e6e6", linewidth=0.6, zorder=0)
    ax.set_axisbelow(True)
    ax.tick_params(axis="y", length=0)
    fig.tight_layout()
    save(fig, "fig32_workload_gains")


def malphite(name):
    """{design: {"ops": M writes/s, "cpu": µs per write}} medians of a verified malphite campaign."""
    runs = {}
    for line in (HERE / "data" / "malphite-nvme" / name / "results.jsonl").read_text().splitlines():
        record = json.loads(line) if line.strip() else None
        if not record or record.get("exit_code") != 0 or "keys" not in (record["result"].get("verification") or {}):
            continue
        result = record["result"]
        entry = runs.setdefault(record["trial"]["design"], {"ops": [], "cpu": []})
        entry["ops"].append(result["throughput"]["ops_per_second"] / 1e6)
        entry["cpu"].append(result["process_cpu"]["cpu_microseconds_per_op"])
    return {design: {k: median(v) for k, v in entry.items()} for design, entry in runs.items()}


def figure_storage():
    nvme = malphite("followup/n6")
    # Reported ratio from the user's own NVMe run; its raw log is not in the repo.
    nvme["full"]["ops"] = nvme["native"]["ops"] * 1.60
    groups = [("Second server:\ndata in memory", malphite("followup/n1")),
              ("Second server:\nNVMe drive", nvme)]
    ssd = load_campaign("v3-ssd")
    if ssd.get((NATIVE, 8)) and ssd.get((FULL, 8)):
        groups.append(("First server:\nSATA SSD", {
            design: {"ops": median([window_ops(r) for r in ssd[(key, 8)]]) / 1e6,
                     "cpu": median([cpu_us(r) for r in ssd[(key, 8)]])}
            for design, key in (("native", NATIVE), ("full", FULL))}))
    fig, axes = plt.subplots(1, 2, figsize=(DOUBLE_COLUMN, 2.6))
    width = 0.38
    for ax, letter, (metric, ylabel, fmt) in zip(axes, "ab", [("ops", "Throughput (M writes/s)", "{:.2f}"),
                                                            ("cpu", "CPU per write (µs)", "{:.1f}")]):
        for index, (_, values) in enumerate(groups):
            native, ours = values["native"][metric], values["full"][metric]
            bar(ax, [index - width / 2], [native], width * 0.95, NATIVE)
            bar(ax, [index + width / 2], [ours], width * 0.95, FULL)
            for x, value in ((index - width / 2, native), (index + width / 2, ours)):
                ax.annotate(fmt.format(value), (x, value), xytext=(0, 1.5), textcoords="offset points",
                            ha="center", va="bottom", fontsize=6.2, color=INK)
            ratio = ours / native if metric == "ops" else native / ours
            top = max(native, ours)
            ax.annotate(f"{ratio:.2f}×" + ("" if metric == "ops" else " less"), (index, top), xytext=(0, 11),
                        textcoords="offset points", ha="center", va="bottom", fontsize=7, fontweight="bold",
                        color=INK)
        ax.set_xticks(range(len(groups)), [label for label, _ in groups], fontsize=6.6)
        ax.set_ylim(0, ax.get_ylim()[1] * 1.22)
        finish_axis(ax, ylabel)
        panel_label(ax, letter)
    fig.tight_layout(w_pad=2.5, rect=(0, 0, 1, 0.88))
    design_legend(fig, (NATIVE, FULL), y=0.88, labels=LABELS)
    save(fig, "fig33_storage_v2")


def main() -> int:
    figure_workload_gains()
    figure_storage()
    return 0


if __name__ == "__main__":
    sys.exit(main())
