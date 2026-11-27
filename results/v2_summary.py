#!/usr/bin/env python3
"""Summaries for the v2 campaigns (PREREGISTRATION.md, "Methodology v2").

  v2_summary.py CAMPAIGN_DIR [CAMPAIGN_DIR ...] [--exclude CAMPAIGN:N,...] [--select OUT.json]

For every trial in CAMPAIGN_DIR/results.jsonl it prints throughput over the measurement
window, steady-state throughput (median of per-minute rates over the last 8 minutes, when
the window is at least 9 minutes long), the per-minute trend, seconds throttled below 25% of
the mean rate, stall fraction, CPU (cores and microseconds per operation, by thread role)
and verification.

--select writes the tuning choice: per design and core count, the highest-throughput
setting with stall below 5% and passing verification (trials listed in --exclude are
skipped, e.g. ones that overlapped other work on the machine).
"""

from __future__ import annotations

import argparse
import json
import statistics
import sys
from pathlib import Path

TUNED_FLAGS = ["threads", "subcompactions", "pipelined-write", "adaptive-yield", "background-jobs"]


def per_minute(timeline: list[dict]) -> list[float]:
    seconds = [t["ops"] for t in timeline if t["phase"] == "measurement"]
    return [sum(seconds[i:i + 60]) / 60 for i in range(0, len(seconds) - 59, 60)]


def summarize(record: dict) -> dict:
    trial, result = record["trial"], record.get("result")
    row = {"index": record["index"], "design": trial["design"], "cores": trial["cores"],
           "flags": {k: record["flags"].get(k) for k in TUNED_FLAGS}, "ok": False}
    if record.get("exit_code") != 0 or result is None:
        row["error"] = (record.get("error") or "")[-200:]
        return row
    measured = [t for t in result["timeline"] if t["phase"] == "measurement"]
    ops = result["throughput"]["ops_per_second"]
    minutes = per_minute(result["timeline"])
    window = result["accounting"]["window"]
    steady = statistics.median(minutes[-8:]) if len(minutes) >= 9 else None
    trend = None
    if len(minutes) >= 3:
        tail = minutes[-8:]
        trend = (tail[-1] - tail[0]) / max(statistics.mean(tail), 1)
    fallbacks = (result.get("transform") or {}).get("cpu_fallback_batches", 0)
    row.update({
        "ok": "keys" in (result.get("verification") or {}),
        "ops_per_second": ops,
        "steady_ops_per_second": steady,
        "trend": trend,
        "throttled_seconds": sum(1 for t in measured if t["ops"] < 0.25 * ops),
        "measured_seconds": len(measured),
        "stall_fraction": result["stall_fraction"],
        "cores_used": window["cores"],
        "cores_by_role": window["cores_by_role"],
        "cpu_us_per_op": window["cores"] * 1e6 / ops if ops else None,
        "device": result["accounting"].get("device"),
        "device_window": window.get("device"),
        "gpu_fallback_batches": fallbacks,
        "write_p99_ms": result["write_latency"].get("p99_ns", 0) / 1e6 if result.get("write_latency") else None,
    })
    return row


def load(directories: list[Path], excluded: set[str]) -> list[dict]:
    rows = []
    for directory in directories:
        for line in (directory / "results.jsonl").read_text().splitlines():
            record = json.loads(line)
            if f"{directory.name}:{record['index']}" in excluded:
                continue
            row = summarize(record)
            row["campaign"] = directory.name
            rows.append(row)
    return rows


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("campaigns", type=Path, nargs="+")
    parser.add_argument("--exclude", default="", help="comma-separated CAMPAIGN:INDEX trials to skip")
    parser.add_argument("--select", type=Path, help="write the tuning choice here")
    arguments = parser.parse_args()
    excluded = {x for x in arguments.exclude.split(",") if x}
    rows = load(arguments.campaigns, excluded)
    rows.sort(key=lambda r: (r["design"], r["cores"], -(r.get("ops_per_second") or 0)))
    print(f"{'design':8} {'cores':>5} {'thr':>4} {'sub':>4} {'pipe':>5} {'ops/s':>10} {'steady':>10} {'trend':>6} "
          f"{'throt':>5} {'stall':>6} {'used':>6} {'us/op':>6} ok  roles")
    for r in rows:
        if "ops_per_second" not in r:
            print(f"{r['design']:8} {r['cores']:>5} failed: {r.get('error', '')}")
            continue
        f = r["flags"]
        steady = f"{r['steady_ops_per_second']:10.0f}" if r["steady_ops_per_second"] else f"{'-':>10}"
        trend = f"{r['trend']:6.2f}" if r["trend"] is not None else f"{'-':>6}"
        roles = " ".join(f"{k[:4]}={v:.1f}" for k, v in sorted(r["cores_by_role"].items()) if v >= 0.05)
        print(f"{r['design']:8} {r['cores']:>5} {f['threads']:>4} {f['subcompactions']:>4} {str(f['pipelined-write'])[:5]:>5} "
              f"{r['ops_per_second']:10.0f} {steady} {trend} {r['throttled_seconds']:>5} {r['stall_fraction']:6.3f} "
              f"{r['cores_used']:6.2f} {r['cpu_us_per_op']:6.2f} {'y' if r['ok'] else 'N':>2}  {roles}")
    if arguments.select:
        chosen = {}
        for r in rows:
            if not r["ok"] or r["stall_fraction"] >= 0.05:
                continue
            key = f"{r['design']}/{r['cores']}"
            if key not in chosen or r["ops_per_second"] > chosen[key]["ops_per_second"]:
                chosen[key] = {"design": r["design"], "cores": r["cores"], "flags": r["flags"],
                               "ops_per_second": r["ops_per_second"], "trial_index": r["index"],
                               "campaign": r["campaign"]}
        arguments.select.write_text(json.dumps({
            "_comment": "Frozen tuning choice (PREREGISTRATION.md, Methodology v2): per design and core count, the "
                        "highest-throughput setting with stall < 5% and passing verification.",
            "excluded_trials": sorted(excluded), "choices": [chosen[k] for k in sorted(chosen)]}, indent=1) + "\n")
        print(f"wrote {arguments.select}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
