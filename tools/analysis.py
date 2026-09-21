"""Analysis of sealed measured runs; never treats individual requests as replicates."""

import csv
import importlib
import json
import math
from pathlib import Path
import random
import statistics


experiments = importlib.import_module(f"{__package__}.experiments" if __package__ else "experiments")


def capacity_bracket(rows: list[dict]) -> dict:
    ordered = sorted(rows, key=lambda row: row["rate"])
    lower = 0
    upper = None
    for row in ordered:
        if row["qualification"] not in ("pass", "fail"):
            return {"lower": lower, "upper": None, "censoring": "unverified"}
        if row["qualification"] != "pass":
            upper = row["rate"]
            break
        lower = row["rate"]
    return {"lower": lower, "upper": upper,
            "censoring": "right" if upper is None else ("left" if lower == 0 else "interval")}


def paired_bootstrap(pairs: list[tuple[float, float]], seed: int, samples: int = 2000) -> dict:
    if len(pairs) < 2 or any(left <= 0 or right <= 0 for left, right in pairs):
        return {"estimate": None, "ci95": None, "pairs": len(pairs), "reason": "at least two positive complete pairs required"}
    logs = [math.log(right / left) for left, right in pairs]
    generator = random.Random(seed)
    bootstrap = sorted(math.exp(statistics.fmean(generator.choices(logs, k=len(logs)))) for _ in range(samples))
    return {"estimate": math.exp(statistics.fmean(logs)), "ci95": [bootstrap[int(samples * 0.025)], bootstrap[int(samples * 0.975)]],
            "pairs": len(pairs), "resampling_unit": "paired repetition", "statistic": "geometric mean ratio"}


def window_work(samples: list[dict]) -> dict:
    result = {"committed": 0, "gpu_blocks": 0}
    if len(samples) < 2:
        return result
    for field in result:
        values = [(sample["statistics"].get("remote") or {}).get(field, 0) for sample in samples]
        if any(later < earlier for earlier, later in zip(values, values[1:])):
            raise ValueError("remote counters decreased during the measurement window")
        result[field] = values[-1] - values[0]
    return result


def comparison_modes(modes: list[str]) -> list[tuple[str, str]]:
    cpu_modes = ("native_cpu", "cpu_native", "cpu_batched")
    pairs = []
    for index, baseline in enumerate(cpu_modes):
        if baseline in modes:
            pairs.extend((baseline, mode) for mode in modes if mode not in cpu_modes[:index + 1])
    return pairs


def measured_rows(directory: Path, run: dict, plan: dict) -> list[dict]:
    config = plan["config"]
    rows = []
    for trial in run["trials"]:
        row = {"trial": trial["id"], "repetition": trial["repetition"], "mode": trial["mode"], "rate": trial["rate"],
               "qualification": "invalid", "reason": trial.get("failure", "trial did not complete"),
               "throughput": None, "read_p99_ms": None, "write_p99_ms": None, "schedule_sha256": None,
               "read_slo_ms": config["read_slo_ms"], "write_slo_ms": config["write_slo_ms"],
               "remote_committed": None, "in_window_remote_committed": 0, "in_window_gpu_blocks": 0,
               "backlog_first": None, "backlog_last": None}
        if trial.get("result"):
            path = (directory / trial["result"]).resolve(strict=True)
            if not path.is_relative_to(directory.resolve(strict=True)):
                raise ValueError("trial result escaped run bundle")
            result = json.loads(path.read_text())
            measured = result["measurement"]
            accounted = sum(measured[key] for key in ("completed", "failed", "expired_before_send", "blocked_by_indeterminate_write", "queue_rejected"))
            if accounted != measured["offered"] or result["offered_rate"] != trial["rate"]:
                raise ValueError("result identity or request accounting mismatch")
            row["throughput"] = measured["completed_in_window"] * 1e9 / result["offered_window_ns"]
            for kind in ("read", "write"):
                percentile = result[f"{kind}_latency"]["p99"]
                row[f"{kind}_p99_ms"] = None if percentile is None else percentile / 1e6
            row["schedule_sha256"] = result["schedule_sha256"]
            remote = (result.get("database_statistics") or {}).get("remote")
            row["remote_committed"] = remote.get("committed", 0) if remote else 0
            metrics = []
            window_samples = []
            metrics_path = path.parent / "primary-metrics.jsonl"
            for line in metrics_path.read_text().splitlines():
                sample = json.loads(line)
                sample_start = sample.get("load_sample_start_ns", sample["load_elapsed_ns"])
                if sample["status"] == "ok" and result["warmup_ns"] <= sample_start <= sample["load_elapsed_ns"] < result["warmup_ns"] + result["offered_window_ns"]:
                    window_samples.append(sample)
                    pending = sample["statistics"]["properties"].get("rocksdb.estimate-pending-compaction-bytes")
                    if pending is not None:
                        metrics.append(pending)
            if metrics:
                row["backlog_first"], row["backlog_last"] = metrics[0], metrics[-1]
            observed_work = window_work(window_samples)
            row["in_window_remote_committed"] = observed_work["committed"]
            row["in_window_gpu_blocks"] = observed_work["gpu_blocks"]
            complete = trial.get("valid", False) and result["valid"] and accounted == measured["completed"]
            read_ok = config["read_percent"] == 0 or (row["read_p99_ms"] is not None and row["read_p99_ms"] <= config["read_slo_ms"])
            write_ok = config["read_percent"] == 100 or (row["write_p99_ms"] is not None and row["write_p99_ms"] <= config["write_slo_ms"])
            if not complete:
                row.update(qualification="invalid", reason="errors, overload, missing metrics, or failed offline verification")
            elif trial["mode"] != "native_cpu" and config["read_percent"] < 100 and not observed_work["committed"]:
                row.update(qualification="unverified", reason="no remote compaction commit observed between in-window samples")
            elif trial["mode"] in ("metal_batched", "cuda_batched") and config["read_percent"] < 100 and not observed_work["gpu_blocks"]:
                row.update(qualification="unverified", reason="no useful GPU work observed between in-window samples")
            elif not read_ok or not write_ok:
                row.update(qualification="fail", reason="operation p99 exceeds declared SLO")
            elif len(metrics) < 3:
                row.update(qualification="unverified", reason="fewer than three in-window compaction-debt samples")
            elif metrics[-1] > metrics[0]:
                row.update(qualification="fail", reason="pending compaction bytes increased during measurement")
            else:
                row.update(qualification="pass", reason="valid run, SLO satisfied, observed compaction debt did not increase")
        rows.append(row)
    expected = {trial["id"] for trial in plan["trials"]}
    if {row["trial"] for row in rows} != expected or len(rows) != len(expected):
        raise ValueError("run is missing planned trials or contains duplicate trials")
    for repetition in range(config["repetitions"]):
        for rate in config["rates"]:
            hashes = {row["schedule_sha256"] for row in rows if row["repetition"] == repetition and row["rate"] == rate and row["schedule_sha256"]}
            if len(hashes) > 1:
                raise ValueError("paired treatments did not use identical workload schedules")
    return rows


def analyze_run(directory: Path, output: Path, plots: bool = True) -> dict:
    experiments.verify_bundle(directory)
    plan = experiments.load_plan(directory / "plan.json")
    run = json.loads((directory / "run.json").read_text())
    if run["state"] != "complete" or run["plan_sha256"] != plan["plan_sha256"]:
        raise ValueError("cannot qualify an incomplete run or mismatched plan")
    if output.exists() or output.absolute().is_relative_to(directory.absolute()):
        raise ValueError("analysis output must be new and outside the sealed run bundle")
    rows = measured_rows(directory, run, plan)
    modes = plan["config"]["modes"]
    repetitions = plan["config"]["repetitions"]
    curves = []
    for repetition in range(repetitions):
        for mode in modes:
            selected = [row for row in rows if row["repetition"] == repetition and row["mode"] == mode]
            bracket = capacity_bracket(selected)
            curves.append({"repetition": repetition, "mode": mode, **bracket,
                           "fully_observed": all(row["qualification"] in ("pass", "fail") for row in selected)})
    comparisons = []
    for baseline, mode in comparison_modes(modes):
        bounds = []
        lower_pairs = []
        for repetition in range(repetitions):
            reference = next(curve for curve in curves if curve["repetition"] == repetition and curve["mode"] == baseline)
            treatment = next(curve for curve in curves if curve["repetition"] == repetition and curve["mode"] == mode)
            if reference["fully_observed"] and treatment["fully_observed"]:
                lower = treatment["lower"] / reference["upper"] if reference["upper"] else None
                upper = treatment["upper"] / reference["lower"] if treatment["upper"] is not None and reference["lower"] else None
                bounds.append({"repetition": repetition, "lower": lower, "upper": upper})
                if reference["upper"] is not None and treatment["upper"] is not None:
                    lower_pairs.append((reference["lower"], treatment["lower"]))
        comparisons.append({"baseline": baseline, "treatment": mode, "capacity_ratio_bounds": bounds,
                            "qualifying_lower_bound_ratio": paired_bootstrap(lower_pairs, plan["config"]["seed"]),
                            "note": "lower-bound ratios compare tested rates, not exact capacities; null bounds remain censored"})
    summary = {"schema_version": 1, "plan_sha256": plan["plan_sha256"], "topology": run["topology"],
               "cross_host_qualified": run["cross_host_qualified"], "trial_count": len(rows),
               "capacity_curves": curves, "comparisons": comparisons,
               "qualification_rule": "all requests successful, offline verified, per-operation p99 SLO, at least three debt samples, no debt growth, useful work between in-window samples",
               "limitations": ["local-process runs do not establish cross-host speedup", "debt trend check is a pilot gate, not proof of long-run steady state",
                               "per-request validation and raw logging overhead is included", "censored capacity values are not replaced by point estimates"]}
    output.mkdir(parents=False)
    experiments.write_json(output / "analysis.json", summary)
    with (output / "trials.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    if plots:
        plot_rows(rows, curves, output)
    return summary


def plot_rows(rows: list[dict], curves: list[dict], output: Path) -> None:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plot

    figure, axes = plot.subplots(1, 3, figsize=(12, 3.8), layout="constrained")
    modes = sorted({row["mode"] for row in rows})
    colors = {mode: f"C{index}" for index, mode in enumerate(modes)}
    fields = ("throughput", "read_p99_ms", "write_p99_ms")
    labels = ("Completed Within Window (ops/s)", "Read p99 (ms)", "Write p99 (ms)")
    for mode in modes:
        rates = sorted({row["rate"] for row in rows if row["mode"] == mode})
        for axis, field in zip(axes, fields):
            values = []
            for rate in rates:
                samples = [row[field] for row in rows if row["mode"] == mode and row["rate"] == rate and row[field] is not None]
                values.append(statistics.median(samples) if samples else float("nan"))
                axis.scatter([rate] * len(samples), samples, alpha=0.45, s=15, color=colors[mode])
            axis.plot(rates, values, marker="o", label=mode, color=colors[mode])
    for axis, field, label in zip(axes, fields, labels):
        upper = max((row[field] for row in rows if row[field] is not None), default=1.0)
        if field.endswith("_p99_ms"):
            upper = max(upper, rows[0][field.replace("_p99_ms", "_slo_ms")])
        axis.set(xlabel="Offered Load (ops/s)", ylabel=label, ylim=(0, max(1.0, upper) * 1.08))
        axis.grid(alpha=0.2)
    for axis, slo in zip(axes[1:], ("read_slo_ms", "write_slo_ms")):
        axis.axhline(rows[0][slo], color="black", linestyle="--", linewidth=1)
    axes[0].legend(fontsize=8)
    figure.suptitle("Measured Local-Process Pilot (All Recorded Outcomes)")
    for suffix in ("png", "svg", "pdf"):
        figure.savefig(output / f"load-curves.{suffix}", dpi=180)
    plot.close(figure)
    figure, axis = plot.subplots(figsize=(8, max(3, len(curves) * 0.28)), layout="constrained")
    for index, curve in enumerate(curves):
        axis.scatter(curve["lower"], index, marker="o" if curve["fully_observed"] else "x")
        if curve["upper"] is not None:
            axis.plot([curve["lower"], curve["upper"]], [index, index], linewidth=2)
        axis.text(curve["lower"], index + 0.12, curve["censoring"] if curve["fully_observed"] else "unverified", fontsize=7)
    axis.set_yticks(range(len(curves)), [f"{curve['mode']} / pair {curve['repetition']}" for curve in curves])
    axis.set_xlabel("Capacity Bracket (offered ops/s)")
    for suffix in ("png", "svg", "pdf"):
        figure.savefig(output / f"capacity-brackets.{suffix}", dpi=180)
    plot.close(figure)