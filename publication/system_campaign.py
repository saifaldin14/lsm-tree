#!/usr/bin/env python3
"""In-process system campaign for GPU compaction offload (runs on the GPU host).

Every trial copies a preloaded golden database, then runs `lsm_system_bench
--phase run` confined to a core budget with `numactl`, optionally with
`lsm_burn` background load on the same cores. Package+DRAM energy (RAPL via
`perf stat -a`) and GPU power (`nvidia-smi`) are sampled at 1 Hz for the whole
trial and integrated over the benchmark's measurement window. Within each
repetition block the trial order is randomized.

Usage:
  system_campaign.py OUT --golden DIR --grid grid.json [--blocks N] [--seed S]

grid.json: {"common": {bench flags...}, "trials": [{"mode": ..., "cores": 4,
"burn_percent": 0, "flags": {...}}, ...]}. Results go to OUT/results.jsonl.
"""

from __future__ import annotations

import argparse
import json
import os
import random
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("LSM_BUILD_DIR", ROOT / "build" / "dev"))
BENCH = BUILD / "lsm_system_bench"
BURN = BUILD / "lsm_burn"
GPU_ENV = {"LSM_DISABLE_GPU_INPUT": "1", "LSM_CUDA_SYNC": "blocking", "LSM_GPU_OUTPUT_MIN_BLOCK_BYTES": "1024",
           "LSM_MERGE_READ_THREADS": "4"}


def physical_cores(node: int, count: int) -> list[int]:
    """Lowest hardware thread of the first `count` physical cores on a NUMA node."""
    cpus = []
    seen = set()
    node_cpus = Path(f"/sys/devices/system/node/node{node}/cpulist").read_text().strip()
    candidates = []
    for part in node_cpus.split(","):
        if "-" in part:
            low, high = part.split("-")
            candidates.extend(range(int(low), int(high) + 1))
        else:
            candidates.append(int(part))
    for cpu in sorted(candidates):
        topology = Path(f"/sys/devices/system/cpu/cpu{cpu}/topology")
        core = (topology / "core_id").read_text().strip(), (topology / "physical_package_id").read_text().strip()
        if core in seen:
            continue
        seen.add(core)
        cpus.append(cpu)
        if len(cpus) == count:
            return cpus
    raise ValueError(f"node {node} has fewer than {count} physical cores")


def integrate(samples: list[tuple[float, float]], start: float, end: float) -> float:
    """Sum per-interval energy samples (timestamp_end, joules) falling inside [start, end]."""
    return sum(joules for stamp, joules in samples if start < stamp <= end + 0.5)


def parse_perf(path: Path, started: float) -> dict[str, list[tuple[float, float]]]:
    series: dict[str, list[tuple[float, float]]] = {}
    if not path.exists():
        return series
    for line in path.read_text().splitlines():
        fields = line.split(",")
        if len(fields) < 4 or line.startswith("#"):
            continue
        try:
            offset = float(fields[0])
            joules = float(fields[1])
        except ValueError:
            continue
        event = fields[3]
        series.setdefault(event, []).append((started + offset, joules))
    return series


def parse_gpu(path: Path) -> list[tuple[float, float]]:
    """nvidia-smi samples -> (unix time, watts)."""
    samples = []
    if not path.exists():
        return samples
    for line in path.read_text().splitlines():
        fields = [field.strip() for field in line.split(",")]
        if len(fields) != 2:
            continue
        try:
            samples.append((float(fields[0]), float(fields[1])))
        except ValueError:
            continue
    return samples


def gpu_energy(samples: list[tuple[float, float]], start: float, end: float) -> float:
    inside = [(stamp, watts) for stamp, watts in samples if start <= stamp <= end]
    if len(inside) < 2:
        return 0.0
    return sum((b[0] - a[0]) * (a[1] + b[1]) / 2 for a, b in zip(inside, inside[1:])) * (end - start) / (inside[-1][0] - inside[0][0])


def run_trial(out: Path, golden: Path, scratch: Path, trial: dict, common: dict, index: int) -> dict:
    trial_dir = out / f"trial-{index:04d}"
    trial_dir.mkdir(parents=True)
    database = scratch / "trial-db"
    shutil.rmtree(database, ignore_errors=True)
    copy_started = time.time()
    shutil.copytree(golden, database)
    copy_seconds = time.time() - copy_started
    cpus = physical_cores(trial.get("node", 0), trial["cores"])
    cpu_list = ",".join(map(str, cpus))
    flags = {**common, **trial.get("flags", {})}
    arguments = ["numactl", f"--physcpubind={cpu_list}", f"--membind={trial.get('node', 0)}", str(BENCH),
                 "--phase", "run", "--mode", trial["mode"], "--dir", str(database)]
    for name, value in flags.items():
        arguments += [f"--{name}", str(value).lower() if isinstance(value, bool) else str(value)]
    environment = {**os.environ, **GPU_ENV}

    perf_path = trial_dir / "rapl.csv"
    gpu_path = trial_dir / "gpu-power.csv"
    started = time.time()
    perf = subprocess.Popen(["perf", "stat", "-a", "-x", ",", "-I", "1000", "-e", "power/energy-pkg/,power/energy-ram/",
                             "-o", str(perf_path)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    gpu = subprocess.Popen(["nvidia-smi", "--query-gpu=timestamp,power.draw", "--format=csv,noheader,nounits", "-lms", "500"],
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    burner = None
    if trial.get("burn_percent", 0) > 0:
        burner = subprocess.Popen([str(BURN), "--cpus", cpu_list, "--percent", str(trial["burn_percent"])],
                                  stdout=(trial_dir / "burn.json").open("w"), stderr=subprocess.STDOUT)
    with (trial_dir / "bench.json").open("w") as stdout, (trial_dir / "bench.err").open("w") as stderr:
        code = subprocess.call(arguments, stdout=stdout, stderr=stderr, env=environment)
    if burner:
        burner.send_signal(signal.SIGTERM)
        burner.wait()
    perf.send_signal(signal.SIGINT)
    perf.wait()
    gpu.send_signal(signal.SIGTERM)
    gpu_lines, _ = gpu.communicate()
    converted = []
    for line in gpu_lines.splitlines():
        stamp, _, watts = line.partition(",")
        try:
            converted.append(f"{time.mktime(time.strptime(stamp.strip().split('.')[0], '%Y/%m/%d %H:%M:%S')) + float('0.' + stamp.strip().split('.')[1])},{float(watts)}")
        except (ValueError, IndexError):
            continue
    gpu_path.write_text("\n".join(converted) + "\n")
    shutil.rmtree(database, ignore_errors=True)

    record = {"index": index, "trial": trial, "flags": flags, "cpus": cpus, "exit_code": code, "copy_seconds": copy_seconds,
              "started_unix": started, "command": arguments, "environment": GPU_ENV}
    if code == 0:
        result = json.loads((trial_dir / "bench.json").read_text())
        window = result["window"]
        rapl = parse_perf(perf_path, started)
        energy = {event: integrate(samples, window["start_unix"], window["end_unix"]) for event, samples in rapl.items()}
        energy["gpu"] = gpu_energy(parse_gpu(gpu_path), window["start_unix"], window["end_unix"])
        record["energy_joules"] = energy
        record["result"] = result
    else:
        record["error"] = (trial_dir / "bench.err").read_text()[-2000:]
    return record


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("out", type=Path)
    parser.add_argument("--golden", type=Path, required=True)
    parser.add_argument("--grid", type=Path, required=True)
    parser.add_argument("--scratch", type=Path, default=Path(f"/dev/shm/{os.environ.get('USER', 'lsm')}/sys"))
    parser.add_argument("--blocks", type=int, default=1)
    parser.add_argument("--seed", type=int, default=1)
    arguments = parser.parse_args()
    grid = json.loads(arguments.grid.read_text())
    arguments.out.mkdir(parents=True, exist_ok=False)
    shutil.copy(arguments.grid, arguments.out / "grid.json")
    golden_meta = arguments.golden.parent / (arguments.golden.name + ".json")
    if golden_meta.exists():
        shutil.copy(golden_meta, arguments.out / "golden.json")
    generator = random.Random(arguments.seed)
    schedule = []
    for block in range(arguments.blocks):
        order = list(grid["trials"])
        generator.shuffle(order)
        schedule.extend({**trial, "block": block} for trial in order)
    (arguments.out / "schedule.json").write_text(json.dumps(schedule, indent=1))
    with (arguments.out / "results.jsonl").open("a") as results:
        for index, trial in enumerate(schedule):
            record = run_trial(arguments.out, arguments.golden, arguments.scratch, trial, grid.get("common", {}), index)
            results.write(json.dumps(record) + "\n")
            results.flush()
            summary = record.get("result", {}).get("throughput", {}).get("ops_per_second")
            print(f"[{index + 1}/{len(schedule)}] {trial} exit={record['exit_code']} ops/s={summary}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
