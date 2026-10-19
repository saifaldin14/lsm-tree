"""Analysis of sealed measured runs; never treats individual requests as replicates."""

from __future__ import annotations

import csv
import datetime
import importlib
import json
import math
import plistlib
from pathlib import Path
import random
import statistics
import shutil


def is_relative_to(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


experiments = importlib.import_module(f"{__package__}.experiments" if __package__ else "experiments")


def resource_stream(path: Path, window: dict | None = None) -> dict:
    keys = ("scope", "entity", "name", "source", "unit", "kind")
    rows = {}
    previous = {}
    last_end = 0
    sequence = 0
    footer = None
    errors = 0
    with path.open() as stream:
        header = json.loads(stream.readline(8 << 20))
        if header.get("type") != "header" or header.get("schema_version") != 1:
            raise ValueError(f"invalid resource header: {path}")
        host = header["host"]
        if window and (not host.get("host_id") or not host.get("boot_id") or
                       any(host.get(key) != window["host"].get(key) for key in ("host_id", "boot_id"))):
            return {"status": "unavailable", "reason": "different host or boot; clock mapping is not verified",
                    "host": host, "role": header["role"], "metrics": []}
        lower = window["measurement_start_ns"] if window else header["start_ns"]
        upper = window["measurement_end_ns"] if window else None
        max_gap = header["interval_ms"] * 3_000_000
        if upper is not None and upper <= lower:
            raise ValueError("invalid resource measurement window")
        while line := stream.readline(8 << 20):
            if not line.endswith("\n"):
                raise ValueError("truncated or oversized resource record")
            record = json.loads(line)
            if footer is not None:
                raise ValueError("resource records after footer")
            if record.get("type") == "footer":
                footer = record
                continue
            if record.get("type") != "sample" or record["sequence"] != sequence:
                raise ValueError("missing or unordered resource sample")
            begin, end = record["sample_start_ns"], record["sample_end_ns"]
            if begin < last_end or end < begin:
                raise ValueError("nonmonotonic resource sample bounds")
            last_end = end
            sequence += 1
            errors += bool(record["errors"])
            midpoint = (begin + end) // 2
            selected = begin >= lower and (upper is None or end <= upper)
            current = {}
            for metric in record["metrics"]:
                identity = tuple(metric[key] for key in keys) + (metric.get("role", header["role"]),)
                if identity in current:
                    raise ValueError("duplicate resource metric in one sample")
                value = metric["value"]
                valid = metric["status"] == "ok" and type(value) in (int, float) and math.isfinite(value)
                if metric["kind"] == "counter" and valid and value < 0:
                    valid = False
                current[identity] = (begin, end, midpoint, value if valid else None, selected)
                row = rows.setdefault(identity, {**{key: metric[key] for key in keys},
                    "role": identity[-1], "samples": 0, "available_samples": 0, "unavailable_samples": 0,
                    "minimum": None, "maximum": None, "covered_ns": 0, "invalid_intervals": 0,
                    "observed_delta": None, "time_weighted_mean": None, "mean_rate_per_second": None,
                    "integrated_power_joules": None, "reasons": set(), "weighted_sum": 0.0,
                    "intervals": 0, "metadata": {key: metric[key] for key in (
                        "device", "interface", "loopback", "layer", "memory_model", "label", "chip", "measurement_scope",
                        "sampling_window", "overlap_policy", "max_range_joules", "includes_children", "pid", "api_stability",
                        "physical_interconnect", "device_location", "virtual_device", "virtualization") if key in metric}})
                if not selected:
                    continue
                row["samples"] += 1
                if not valid:
                    row["unavailable_samples"] += 1
                    row["reasons"].add(metric.get("reason", "invalid or nonfinite reading"))
                    continue
                row["available_samples"] += 1
                row["minimum"] = value if row["minimum"] is None else min(value, row["minimum"])
                row["maximum"] = value if row["maximum"] is None else max(value, row["maximum"])
                prior = previous.get(identity)
                if not prior or not prior[4]:
                    continue
                elapsed = midpoint - prior[2]
                if prior[3] is None or not 0 < elapsed <= max_gap:
                    row["invalid_intervals"] += 1
                    row["reasons"].add("missing reading or excessive sampling gap")
                    continue
                if metric["kind"] == "counter":
                    if value < prior[3]:
                        row["invalid_intervals"] += 1
                        row["reasons"].add("counter reset or wrap; no wrap size inferred")
                        continue
                    row["observed_delta"] = (row["observed_delta"] or 0) + value - prior[3]
                elif metric["kind"] == "gauge":
                    row["weighted_sum"] += (value + prior[3]) * 0.5 * elapsed
                else:
                    raise ValueError("unknown resource metric kind")
                row["covered_ns"] += elapsed
                row["intervals"] += 1
            if selected:
                for identity in rows.keys() - current.keys():
                    rows[identity]["unavailable_samples"] += 1
                    rows[identity]["reasons"].add("metric absent from sample")
            previous = current
    if footer and footer["samples"] != sequence:
        raise ValueError("resource footer sample count mismatch")
    duration = (upper if upper is not None else (footer or {}).get("end_ns", last_end)) - lower
    for row in rows.values():
        covered = row["covered_ns"]
        row["coverage_fraction"] = min(1.0, covered / duration) if duration > 0 else 0.0
        if covered and row["kind"] == "counter":
            row["mean_rate_per_second"] = row["observed_delta"] * 1e9 / covered
        if covered and row["kind"] == "gauge":
            row["time_weighted_mean"] = row["weighted_sum"] / covered
            if row["name"] == "power_watts" and row["unit"] == "watts" and row["minimum"] >= 0:
                row["integrated_power_joules"] = row["weighted_sum"] / 1e9
        row["status"] = "ok" if covered and not row["invalid_intervals"] and not row["unavailable_samples"] else "partial"
        if not row["available_samples"]:
            row["status"] = "unavailable"
        row["reasons"] = sorted(row["reasons"])
        del row["weighted_sum"]
    families = {
        "cpu_time": ("cpu_user_ns", "cpu_system_ns"),
        "memory": ("resident_bytes", "memory_total_bytes"),
        "process_storage_bytes": ("storage_read_bytes", "storage_write_bytes"),
        "network_bytes": ("network_rx_bytes", "network_tx_bytes"),
        "block_device_bytes": ("disk_read_bytes", "disk_write_bytes"),
        "gpu_utilization": ("gpu_utilization_percent",),
        "gpu_memory": ("gpu_memory_used_bytes",),
        "power": ("power_watts",),
        "hardware_energy": ("energy_joules",),
        "temperature": ("temperature_celsius",),
    }
    availability = {}
    for family, names in families.items():
        candidates = [row for row in rows.values() if row["name"] in names and row["scope"] != "host_virtual_block_device"]
        observed = [row for row in candidates if row["available_samples"]]
        availability[family] = {"status": "available" if observed else ("unavailable" if candidates else "not_collected"),
            "observed_entities": len({(row["scope"], row["entity"]) for row in observed}),
            "maximum_coverage_fraction": max((row["coverage_fraction"] for row in observed), default=0.0)}
    return {"status": "complete" if footer and footer["status"] == "complete" and not errors else "incomplete",
            "role": header["role"], "host": host, "includes_host": header["includes_host"],
            "window": "measurement" if window else "collector_lifetime", "duration_ns": max(0, duration),
            "metrics": list(rows.values()), "availability": availability, "collector": footer,
            "rules": ["observed deltas are not extrapolated to unsampled edges", "counter resets and long gaps are excluded",
                      "host, process, interface and device totals overlap and must not be added together",
                      "integrated_power_joules is a sampled estimate, separate from hardware energy counters"]}


def write_resource_report(streams: list[dict], output: Path) -> dict:
    report = {"schema_version": 1, "streams": streams, "aggregation": "per_entity_no_cross_scope_or_cross_host_sums"}
    experiments.write_json(output / "resources.json", report)
    columns = ("trial", "file", "window", "host_id", "role", "scope", "entity", "name", "source", "unit", "kind",
               "status", "available_samples", "unavailable_samples", "observed_delta", "mean_rate_per_second",
               "minimum", "maximum", "time_weighted_mean", "integrated_power_joules", "covered_ns", "coverage_fraction", "invalid_intervals")
    with (output / "resources.csv").open("w", newline="") as destination:
        writer = csv.DictWriter(destination, fieldnames=columns)
        writer.writeheader()
        for stream in streams:
            for row in stream.get("metrics", []):
                record = {key: row.get(key) for key in columns}
                record.update(trial=stream.get("trial"), file=stream["file"], window=stream.get("window"), host_id=stream["host"].get("host_id"))
                writer.writerow(record)
    return report


def analyze_resources(directory: Path, run: dict, output: Path) -> dict:
    streams = []
    for trial in run["trials"]:
        base = directory / "trials" / trial["id"]
        result_path = base / "load" / "result.json"
        window = json.loads(result_path.read_text()).get("resource_window") if result_path.exists() else None
        paths = [base / "host-resources.jsonl", base / "primary-resources.jsonl", base / "load" / "resources.jsonl"]
        if trial["mode"] != "native_cpu":
            paths.append(base / "worker-resources.jsonl")
        for path in paths:
            label = {"trial": trial["id"], "file": str(path.relative_to(directory))}
            if not path.exists():
                streams.append({**label, "status": "unavailable", "reason": "resource stream was not recorded", "metrics": []})
                continue
            streams.append({**label, **resource_stream(path)})
            if window:
                streams.append({**label, **resource_stream(path, window)})
    return write_resource_report(streams, output)


def powermetrics_sample(document: dict) -> dict:
    timestamp = document.get("timestamp")
    if not isinstance(timestamp, datetime.datetime):
        raise ValueError("powermetrics sample has no plist timestamp")
    duration = document.get("elapsed_ns")
    if type(duration) not in (int, float) or not math.isfinite(duration) or not 0 < duration <= 3_600_000_000_000:
        duration = None
    processor = document.get("processor", {})
    if not isinstance(processor, dict):
        raise ValueError("powermetrics processor record must be a dictionary")
    metrics = []
    for domain, field in (("cpu", "cpu_power"), ("gpu", "gpu_power"), ("ane", "ane_power"),
                          ("combined_cpu_gpu_ane", "combined_power")):
        value = processor.get(field)
        valid = type(value) in (int, float) and math.isfinite(value) and value >= 0 and not document.get("invalid", False)
        watts = value / 1000 if valid else None
        metrics.append({"domain": domain, "source": "powermetrics.plist:processor." + field,
                        "power_watts": watts, "status": "ok" if valid else "unavailable",
                        "reason": None if valid else "explicit power field absent or invalid; legacy energy-named fields are not assumed to be watts",
                        "estimated_sample_energy_joules": watts * duration / 1e9 if valid and duration else None,
                        "hardware_energy_counter": False, "measurement_kind": "OS_power_estimate"})
    return {"timestamp_utc": timestamp.replace(tzinfo=datetime.timezone.utc).isoformat(), "elapsed_ns": duration,
            "metrics": metrics, "thermal_pressure": document.get("thermal_pressure"),
            "temperature_celsius": None, "temperature_reason": "thermal pressure is not a Celsius sensor"}


def import_powermetrics(capture: Path, output: Path) -> dict:
    if output.exists() or capture.stat().st_size > 1 << 30:
        raise ValueError("power import requires a new output directory and a capture no larger than 1 GiB")
    output.mkdir()
    retained = output / "powermetrics.plist"
    shutil.copyfile(capture, retained)
    if retained.stat().st_size > 1 << 30:
        raise ValueError("power capture grew beyond the import budget; stop collection before importing")
    samples = 0
    available = 0
    previous_timestamp = None
    with retained.open("rb") as source, (output / "power-samples.jsonl").open("x") as destination:
        buffered = b""
        while chunk := source.read(65536):
            buffered += chunk
            while b"\0" in buffered:
                frame, buffered = buffered.split(b"\0", 1)
                if not frame.strip():
                    continue
                if len(frame) > 8 << 20:
                    raise ValueError("oversized powermetrics plist record")
                sample = powermetrics_sample(plistlib.loads(frame))
                if previous_timestamp and sample["timestamp_utc"] <= previous_timestamp:
                    raise ValueError("duplicate or decreasing powermetrics timestamp")
                previous_timestamp = sample["timestamp_utc"]
                available += sum(metric["status"] == "ok" for metric in sample["metrics"])
                destination.write(json.dumps(sample, allow_nan=False) + "\n")
                samples += 1
            if len(buffered) > 8 << 20:
                raise ValueError("oversized powermetrics plist record")
        if buffered.strip():
            sample = powermetrics_sample(plistlib.loads(buffered))
            if previous_timestamp and sample["timestamp_utc"] <= previous_timestamp:
                raise ValueError("duplicate or decreasing powermetrics timestamp")
            available += sum(metric["status"] == "ok" for metric in sample["metrics"])
            destination.write(json.dumps(sample, allow_nan=False) + "\n")
            samples += 1
    report = {"schema_version": 1, "status": "imported" if samples and available else "unavailable", "samples": samples,
              "available_power_readings": available, "capture_sha256": experiments.file_hash(retained),
              "clock": "capture_realtime_UTC_not_aligned_to_benchmark_monotonic_clock",
              "host_binding": "unverified_operator_provided_capture", "power_source": "OS_estimate_not_calibrated_meter",
              "limitations": ["unsupported plist fields remain unavailable", "combined and component power must not be summed",
                              "sample-energy estimates are not cumulative hardware counters", "thermal pressure is not temperature"]}
    experiments.write_json(output / "power-import.json", report)
    experiments.seal_bundle(output)
    return report


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


ENGINE_COUNTERS = {
    "user_bytes_written": ("io", "user_bytes_written"),
    "flush_bytes_written": ("io", "flush_bytes_written"),
    "compaction_bytes_read": ("io", "compaction_bytes_read"),
    "compaction_bytes_written": ("io", "compaction_bytes_written"),
    "write_stall_micros": ("write_stalls", "stall_micros"),
    "write_stops": ("write_stalls", "cf", "total-stops"),
    "write_delays": ("write_stalls", "cf", "total-delays"),
}


def engine_value(statistics_document: dict | None, path: tuple) -> float | None:
    value = statistics_document
    for key in path:
        if not isinstance(value, dict) or key not in value:
            return None
        value = value[key]
    return value if isinstance(value, (int, float)) and not isinstance(value, bool) else None


def engine_metrics(samples: list[dict], summary: dict | None = None) -> dict:
    """In-window deltas of primary write-stall and I/O counters plus whole-trial totals from primary-summary.json.

    Fields are None when the primary predates these statistics."""
    result = {}
    for name, path in ENGINE_COUNTERS.items():
        values = [engine_value(sample.get("statistics"), path) for sample in samples]
        values = [value for value in values if value is not None]
        result[f"window_{name}"] = values[-1] - values[0] if len(values) >= 2 and values[-1] >= values[0] else None
        result[f"total_{name}"] = engine_value(summary, path) if summary else None
    user = result["window_user_bytes_written"]
    written = [result["window_flush_bytes_written"], result["window_compaction_bytes_written"]]
    result["window_write_amplification"] = (sum(written) / user if user and None not in written else None)
    result["total_write_amplification"] = engine_value(summary, ("io", "write_amplification")) if summary else None
    running = [engine_value(sample.get("statistics"), ("properties", "rocksdb.num-running-compactions")) for sample in samples]
    running = [value for value in running if value is not None]
    result["window_mean_running_compactions"] = statistics.fmean(running) if running else None
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
        closed = trial.get("load_mode") == "closed_loop"
        row = {"trial": trial["id"], "repetition": trial["repetition"], "mode": trial["mode"], "rate": trial.get("rate"),
               "qualification": "invalid", "reason": trial.get("failure", "trial did not complete"),
               "throughput": None, "read_p99_ms": None, "write_p99_ms": None, "schedule_sha256": None,
               "read_slo_ms": config["read_slo_ms"], "write_slo_ms": config["write_slo_ms"],
               "remote_committed": None, "in_window_remote_committed": 0, "in_window_gpu_blocks": 0,
               "backlog_first": None, "backlog_last": None}
        if closed:
            row.update(load_mode="closed_loop", clients=trial["clients"], reads_per_second=None, writes_per_second=None,
                       read_p999_ms=None, write_p999_ms=None)
        row.update(engine_metrics([]))
        if trial.get("result"):
            path = (directory / trial["result"]).resolve(strict=True)
            if not is_relative_to(path, directory.resolve(strict=True)):
                raise ValueError("trial result escaped run bundle")
            result = json.loads(path.read_text())
            measured = result["measurement"]
            accounted = sum(measured[key] for key in ("completed", "failed", "expired_before_send", "blocked_by_indeterminate_write", "queue_rejected"))
            if accounted != measured["offered"] or (not closed and result["offered_rate"] != trial["rate"]) or \
                    (closed and result.get("load_mode") != "closed_loop"):
                raise ValueError("result identity or request accounting mismatch")
            row["throughput"] = measured["completed_in_window"] * 1e9 / result["offered_window_ns"]
            if closed:
                row["throughput"] = result["throughput"]["ops_per_second"]
                row["reads_per_second"] = result["throughput"]["reads_per_second"]
                row["writes_per_second"] = result["throughput"]["writes_per_second"]
                for kind in ("read", "write"):
                    percentile = result[f"{kind}_latency"].get("p999")
                    row[f"{kind}_p999_ms"] = None if percentile is None else percentile / 1e6
            for kind in ("read", "write"):
                percentile = result[f"{kind}_latency"]["p99"]
                row[f"{kind}_p99_ms"] = None if percentile is None else percentile / 1e6
            row["schedule_sha256"] = result.get("schedule_sha256")
            database_statistics = result.get("database_statistics") or {}
            remote = database_statistics.get("remote")
            local_transform = database_statistics.get("local_transform")
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
            summary_path = path.parent.parent / "primary-summary.json"
            primary_summary = json.loads(summary_path.read_text()) if summary_path.is_file() else None
            row.update(engine_metrics(window_samples, primary_summary))
            observed_work = window_work(window_samples)
            row["in_window_remote_committed"] = observed_work["committed"]
            local_blocks = [
                sample["statistics"]["local_transform"]["blocks"]
                for sample in window_samples
                if (sample.get("statistics") or {}).get("local_transform") is not None
                and (sample["statistics"]["local_transform"] or {}).get("blocks") is not None
            ]
            row["in_window_gpu_blocks"] = (
                local_blocks[-1] - local_blocks[0] if local_transform is not None and len(local_blocks) >= 2
                else observed_work["gpu_blocks"]
            )
            complete = trial.get("valid", False) and result["valid"] and accounted == measured["completed"]
            read_ok = config["read_percent"] == 0 or (row["read_p99_ms"] is not None and row["read_p99_ms"] <= config["read_slo_ms"])
            write_ok = config["read_percent"] == 100 or (row["write_p99_ms"] is not None and row["write_p99_ms"] <= config["write_slo_ms"])
            if not complete:
                row.update(qualification="invalid", reason="errors, overload, missing metrics, or failed offline verification")
            elif trial["mode"] not in ("native_cpu", "cuda_local") and config["read_percent"] < 100 and not observed_work["committed"]:
                row.update(qualification="unverified", reason="no remote compaction commit observed between in-window samples")
            elif trial["mode"] in ("metal_batched", "cuda_batched", "cuda_local") and config["read_percent"] < 100 and not row["in_window_gpu_blocks"]:
                row.update(qualification="unverified", reason="no useful GPU work observed between in-window samples")
            elif not read_ok or not write_ok:
                row.update(qualification="fail", reason="operation p99 exceeds declared SLO")
            elif closed:
                row.update(qualification="pass", reason="valid saturation run, SLO satisfied (debt growth is expected at saturation)")
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
        for rate in config.get("rates", []):
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
    if output.exists() or is_relative_to(output.absolute(), directory.absolute()):
        raise ValueError("analysis output must be new and outside the sealed run bundle")
    rows = measured_rows(directory, run, plan)
    modes = plan["config"]["modes"]
    repetitions = plan["config"]["repetitions"]
    if plan["config"].get("load_mode") == "closed_loop":
        return analyze_closed_loop(directory, output, plan, run, rows, plots)
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
    resources = analyze_resources(directory, run, output)
    summary["resource_streams"] = len(resources["streams"])
    summary["resource_report"] = "resources.json"
    summary["limitations"].append("sensor availability and sampling coverage are reported per entity; missing power is not zero energy")
    experiments.write_json(output / "analysis.json", summary)
    with (output / "trials.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    if plots:
        plot_rows(rows, curves, output)
    return summary


def analyze_closed_loop(directory: Path, output: Path, plan: dict, run: dict, rows: list[dict], plots: bool) -> dict:
    """Saturation runs: sustained throughput per trial and paired throughput ratios per repetition."""
    modes = plan["config"]["modes"]
    observed = [row for row in rows if row["qualification"] in ("pass", "fail")]
    saturation = [{"repetition": row["repetition"], "mode": row["mode"], "clients": row["clients"],
                   "ops_per_second": row["throughput"], "reads_per_second": row["reads_per_second"],
                   "writes_per_second": row["writes_per_second"], "qualification": row["qualification"]} for row in rows]
    comparisons = []
    for baseline, mode in comparison_modes(modes):
        pairs = []
        for repetition in range(plan["config"]["repetitions"]):
            reference = next((row for row in observed if row["repetition"] == repetition and row["mode"] == baseline), None)
            treatment = next((row for row in observed if row["repetition"] == repetition and row["mode"] == mode), None)
            if reference and treatment:
                pairs.append((reference["throughput"], treatment["throughput"]))
        comparisons.append({"baseline": baseline, "treatment": mode,
                            "throughput_ratio": paired_bootstrap(pairs, plan["config"]["seed"])})
    summary = {"schema_version": 1, "plan_sha256": plan["plan_sha256"], "topology": run["topology"], "load_mode": "closed_loop",
               "cross_host_qualified": run["cross_host_qualified"], "trial_count": len(rows),
               "saturation": saturation, "comparisons": comparisons,
               "qualification_rule": "all requests successful, offline verified, per-operation p99 SLO, useful work between in-window samples",
               "limitations": ["local-process runs do not establish cross-host speedup",
                               "closed-loop throughput depends on the client count; latency excludes queueing before send",
                               "per-request validation overhead is included"]}
    output.mkdir(parents=False)
    resources = analyze_resources(directory, run, output)
    summary["resource_streams"] = len(resources["streams"])
    summary["resource_report"] = "resources.json"
    experiments.write_json(output / "analysis.json", summary)
    with (output / "trials.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    if plots:
        plot_saturation(rows, output)
    return summary


def plot_saturation(rows: list[dict], output: Path) -> None:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plot

    modes = sorted({row["mode"] for row in rows})
    figure, axes = plot.subplots(1, 3, figsize=(12, 3.8), layout="constrained")
    fields = ("throughput", "read_p99_ms", "write_p99_ms")
    labels = ("Sustained Throughput (ops/s)", "Read p99 (ms)", "Write p99 (ms)")
    for axis, field, label in zip(axes, fields, labels):
        for index, mode in enumerate(modes):
            samples = [row[field] for row in rows if row["mode"] == mode and row[field] is not None]
            axis.scatter([index] * len(samples), samples, alpha=0.6, s=18, color=f"C{index}")
            if samples:
                axis.hlines(statistics.median(samples), index - 0.3, index + 0.3, color=f"C{index}")
        axis.set_xticks(range(len(modes)), modes, rotation=20, fontsize=8)
        axis.set(ylabel=label, ylim=(0, None))
        axis.grid(alpha=0.2)
    figure.suptitle("Closed-Loop Saturation (All Recorded Outcomes)")
    for suffix in ("png", "svg", "pdf"):
        figure.savefig(output / f"saturation.{suffix}", dpi=180)
    plot.close(figure)


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
