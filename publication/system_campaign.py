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
           "LSM_MERGE_READ_THREADS": "4", "LSM_GPU_MERGE_STREAMS": "2"}


def node_cpus(node: int) -> list[int]:
    cpus = []
    for part in Path(f"/sys/devices/system/node/node{node}/cpulist").read_text().strip().split(","):
        if "-" in part:
            low, high = part.split("-")
            cpus.extend(range(int(low), int(high) + 1))
        elif part:
            cpus.append(int(part))
    return sorted(cpus)


def physical_cores(node: int, count: int) -> tuple[list[int], list[int]]:
    """Lowest hardware thread of `count` physical cores, starting on `node` (the GPU's node)
    and spilling to the next NUMA nodes when one node has too few. Returns (cpus, nodes used)."""
    nodes = sorted(int(path.name[4:]) for path in Path("/sys/devices/system/node").glob("node[0-9]*"))
    order = [node] + [other for other in nodes if other != node]
    cpus, used, seen = [], [], set()
    for current in order:
        for cpu in node_cpus(current):
            topology = Path(f"/sys/devices/system/cpu/cpu{cpu}/topology")
            core = (topology / "core_id").read_text().strip(), (topology / "physical_package_id").read_text().strip()
            if core in seen:
                continue
            seen.add(core)
            cpus.append(cpu)
            if current not in used:
                used.append(current)
            if len(cpus) == count:
                return cpus, used
    raise ValueError(f"only {len(cpus)} physical cores available, {count} requested")


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


def idle_power(seconds: int) -> dict:
    """Average host (RAPL) and GPU power with nothing of ours running."""
    path = Path(f"/tmp/lsm-idle-{os.getpid()}.csv")
    started = time.time()
    perf = subprocess.Popen(["perf", "stat", "-a", "-x", ",", "-I", "1000", "-e", "power/energy-pkg/,power/energy-ram/",
                             "-o", str(path)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    gpu = subprocess.Popen(["nvidia-smi", "--query-gpu=power.draw", "--format=csv,noheader,nounits", "-lms", "500"],
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    time.sleep(seconds)
    perf.send_signal(signal.SIGINT)
    perf.wait()
    gpu.send_signal(signal.SIGTERM)
    gpu_lines, _ = gpu.communicate()
    elapsed = time.time() - started
    host = sum(joules for series in parse_perf(path, started).values() for _, joules in series)
    path.unlink(missing_ok=True)
    watts = [float(line) for line in gpu_lines.split() if line.replace(".", "", 1).isdigit()]
    return {"seconds": elapsed, "host_watts": host / elapsed if elapsed > 0 else 0.0,
            "gpu_watts": sum(watts) / len(watts) if watts else 0.0}


def foreign_load() -> dict:
    """CPU use (percent of one core) by other users' processes, and the 1-minute load average."""
    user = os.environ.get("USER", "")
    total = 0.0
    try:
        lines = subprocess.run(["ps", "-eo", "user:32,pcpu", "--no-headers"], capture_output=True, text=True,
                               timeout=10).stdout.splitlines()
        for line in lines:
            fields = line.split()
            if len(fields) == 2 and fields[0] != user and fields[0] != "root":
                total += float(fields[1])
    except (OSError, ValueError, subprocess.SubprocessError):
        total = -1.0
    return {"other_users_cpu_percent": total, "loadavg_1m": os.getloadavg()[0]}


def run_trial(out: Path, golden: Path, scratch: Path, trial: dict, common: dict, index: int, idle_seconds: int) -> dict:
    """One trial. With "instances": N, N independent databases run at once, each on its own
    `cores` physical cores and its own copy of the golden database, sharing the GPU."""
    trial_dir = out / f"trial-{index:04d}"
    if trial_dir.exists():
        shutil.rmtree(trial_dir)
    trial_dir.mkdir(parents=True)
    load_before = foreign_load()
    idle = idle_power(idle_seconds) if idle_seconds > 0 else None
    instances = trial.get("instances", 1)
    all_cpus, nodes = physical_cores(trial.get("node", 0), trial["cores"] * instances)
    flags = {**common, **trial.get("flags", {})}
    # A trial may add environment settings, e.g. LSM_DISABLE_GPU_BUILD=1 for the merge-only design.
    environment = {**os.environ, **GPU_ENV, **trial.get("env", {})}
    databases, commands = [], []
    copy_started = time.time()
    for instance in range(instances):
        database = Path(trial.get("scratch", scratch)) / f"trial-db-{instance}"
        database.parent.mkdir(parents=True, exist_ok=True)
        shutil.rmtree(database, ignore_errors=True)
        # Reflink copies are instant on btrfs/XFS; tmpfs falls back to a full copy.
        subprocess.run(["cp", "-a", "--reflink=auto", str(golden), str(database)], check=True)
        cpus = all_cpus[instance * trial["cores"]:(instance + 1) * trial["cores"]]
        # Several databases (or a trial marked "memory": "preferred") may not fit in one node's memory:
        # prefer the GPU's node but allow spilling.
        memory = (f"--preferred={trial.get('node', 0)}" if instances > 1 or trial.get("memory") == "preferred"
                  else f"--membind={','.join(map(str, nodes))}")
        command = ["numactl", f"--physcpubind={','.join(map(str, cpus))}", memory,
                   str(BENCH), "--phase", "run", "--mode", trial["mode"], "--dir", str(database)]
        for name, value in flags.items():
            command += [f"--{name}", str(value).lower() if isinstance(value, bool) else str(value)]
        databases.append(database)
        commands.append(command)
    copy_seconds = time.time() - copy_started
    cpu_list = ",".join(map(str, all_cpus))

    perf_path = trial_dir / "rapl.csv"
    gpu_path = trial_dir / "gpu-power.csv"
    started = time.time()
    perf = subprocess.Popen(["perf", "stat", "-a", "-x", ",", "-I", "1000", "-e", "power/energy-pkg/,power/energy-ram/",
                             "-o", str(perf_path)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    gpu = subprocess.Popen(["nvidia-smi", "--query-gpu=timestamp,power.draw,utilization.gpu", "--format=csv,noheader,nounits",
                            "-lms", "500"], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    iostat = None
    if shutil.which("iostat"):
        iostat = subprocess.Popen(["iostat", "-dxm", "-y", "1"], stdout=(trial_dir / "iostat.txt").open("w"),
                                  stderr=subprocess.DEVNULL)
    burner = None
    if trial.get("burn_percent", 0) > 0:
        burner = subprocess.Popen([str(BURN), "--cpus", cpu_list, "--percent", str(trial["burn_percent"])],
                                  stdout=(trial_dir / "burn.json").open("w"), stderr=subprocess.STDOUT)
    processes = []
    # The monitors are stopped even if the driver is interrupted, so none outlive the trial.
    try:
        for instance, command in enumerate(commands):
            suffix = "" if instances == 1 else f"-{instance}"
            processes.append(subprocess.Popen(command, stdout=(trial_dir / f"bench{suffix}.json").open("w"),
                                              stderr=(trial_dir / f"bench{suffix}.err").open("w"), env=environment))
        codes = [process.wait() for process in processes]
    finally:
        for process in processes:
            if process.poll() is None:
                process.kill()
                process.wait()
        if burner:
            burner.send_signal(signal.SIGTERM)
            burner.wait()
        perf.send_signal(signal.SIGINT)
        perf.wait()
        if iostat:
            iostat.send_signal(signal.SIGTERM)
            iostat.wait()
        gpu.send_signal(signal.SIGTERM)
        gpu_lines, _ = gpu.communicate()
    converted, utilization = [], []
    for line in gpu_lines.splitlines():
        fields = [field.strip() for field in line.split(",")]
        try:
            stamp = fields[0]
            converted.append(f"{time.mktime(time.strptime(stamp.split('.')[0], '%Y/%m/%d %H:%M:%S')) + float('0.' + stamp.split('.')[1])},{float(fields[1])}")
            utilization.append(float(fields[2]))
        except (ValueError, IndexError):
            continue
    gpu_path.write_text("\n".join(converted) + "\n")
    for database in databases:
        shutil.rmtree(database, ignore_errors=True)

    code = next((c for c in codes if c != 0), 0)
    record = {"index": index, "trial": trial, "flags": flags, "cpus": all_cpus, "exit_code": code, "exit_codes": codes,
              "copy_seconds": copy_seconds, "started_unix": started, "command": commands[0],
              "environment": {**GPU_ENV, **trial.get("env", {})}, "idle_power": idle, "numa_nodes": nodes,
              "foreign_load": {"before": load_before, "after": foreign_load()},
              "gpu_utilization_mean": sum(utilization) / len(utilization) if utilization else None}
    if code != 0:
        errors = [(trial_dir / f"bench{'' if instances == 1 else f'-{i}'}.err").read_text()[-1000:] for i in range(instances)]
        record["error"] = "\n".join(errors)
        return record
    results = [json.loads((trial_dir / f"bench{'' if instances == 1 else f'-{i}'}.json").read_text()) for i in range(instances)]
    rapl = parse_perf(perf_path, started)
    gpu_samples = parse_gpu(gpu_path)
    window = (min(r["window"]["start_unix"] for r in results), max(r["window"]["end_unix"] for r in results))
    energy = {event: integrate(samples, *window) for event, samples in rapl.items()}
    energy["gpu"] = gpu_energy(gpu_samples, *window)
    record["energy_joules"] = energy
    # Whole run, from the first operation to the end of catch-up compaction (of the last instance).
    span = (min(r["ingest_and_compact"]["start_unix"] for r in results),
            max(r["ingest_and_compact"]["end_unix"] for r in results))
    total = {event: integrate(samples, *span) for event, samples in rapl.items()}
    total["gpu"] = gpu_energy(gpu_samples, *span)
    total["seconds"] = span[1] - span[0]
    record["energy_joules_end_to_end"] = total
    if instances == 1:
        record["result"] = results[0]
    else:
        record["results"] = results
        record["aggregate"] = {
            "ops": sum(r["ingest_and_compact"]["ops"] for r in results),
            "ops_per_second": sum(r["ingest_and_compact"]["ops_per_second"] for r in results),
            "window_ops_per_second": sum(r["throughput"]["ops_per_second"] for r in results)}
    return record


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("out", type=Path)
    parser.add_argument("--golden", type=Path, required=True)
    parser.add_argument("--grid", type=Path, required=True)
    parser.add_argument("--scratch", type=Path, default=Path(f"/dev/shm/{os.environ.get('USER', 'lsm')}/sys"))
    parser.add_argument("--blocks", type=int, default=1)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--idle-seconds", type=int, default=10, help="idle power sample before each trial (0 disables)")
    parser.add_argument("--resume", action="store_true", help="continue an interrupted campaign in OUT")
    arguments = parser.parse_args()
    grid = json.loads(arguments.grid.read_text())
    done = set()
    if arguments.resume:
        if not (arguments.out / "schedule.json").exists():
            raise SystemExit("--resume needs an existing OUT with schedule.json")
        results_path = arguments.out / "results.jsonl"
        if results_path.exists():
            done = {json.loads(line)["index"] for line in results_path.read_text().splitlines() if line.strip()}
    else:
        arguments.out.mkdir(parents=True, exist_ok=False)
        shutil.copy(arguments.grid, arguments.out / "grid.json")
    golden_meta = arguments.golden.parent / (arguments.golden.name + ".json")
    if golden_meta.exists():
        shutil.copy(golden_meta, arguments.out / "golden.json")
    if arguments.resume:
        schedule = json.loads((arguments.out / "schedule.json").read_text())
    else:
        generator = random.Random(arguments.seed)
        schedule = []
        for block in range(arguments.blocks):
            order = list(grid["trials"])
            generator.shuffle(order)
            schedule.extend({**trial, "block": block} for trial in order)
        (arguments.out / "schedule.json").write_text(json.dumps(schedule, indent=1))
    with (arguments.out / "results.jsonl").open("a") as results:
        for index, trial in enumerate(schedule):
            if index in done:
                continue
            # A trial may name its own golden database (e.g. a larger one on SSD).
            golden = Path(trial.get("golden", arguments.golden))
            record = run_trial(arguments.out, golden, arguments.scratch, trial, grid.get("common", {}), index,
                               arguments.idle_seconds)
            results.write(json.dumps(record) + "\n")
            results.flush()
            summary = (record.get("result", {}).get("ingest_and_compact", {}).get("ops_per_second")
                       or record.get("aggregate", {}).get("ops_per_second"))
            print(f"[{index + 1}/{len(schedule)}] {trial} exit={record['exit_code']} ops/s={summary}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
