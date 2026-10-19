#!/usr/bin/env python3
"""Randomized revision campaign: CUDA sync mode x primary NUMA placement, an
equal-resource CPU baseline, and the previously excluded write-heavy point.

Runs on the experiment host (Python 3.8+). By default the GPU input path is
disabled (LSM_DISABLE_GPU_INPUT=1), matching the published configuration, so
only compaction output compression and checksums are offloaded. Each block contains every
(workload, condition) cell once, in a seeded random order; every cell is a
one-repetition paired plan (native_cpu and cuda_local, order randomized by
labctl). CPU package/DRAM energy is sampled with `perf stat` (RAPL) for the
whole campaign; its timestamps are on CLOCK_MONOTONIC, the same clock as the
per-trial resource windows.
"""

from __future__ import annotations

import argparse
import json
import os
import random
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LABCTL = ROOT / "tools" / "labctl.py"

BASE_SPEC = {
    "schema_version": 1,
    "dataset": {"records": 16000, "seed": 1, "key_bytes": 16, "value_bytes": 1024, "overwrite_rounds": 1},
    "engine": {"compression": "lz4", "write_buffer_bytes": 262144, "target_file_bytes": 8388608,
               "block_cache_bytes": 67108864, "block_bytes": 131072, "background_jobs": 1, "levels": 4},
    "limits": {"logical_write_bytes": 4294967296, "minimum_free_disk_bytes": 1073741824},
}

WORKLOADS = {
    "stress-5000": {"rates": [5000], "read_percent": 0, "threads": 8, "queue_capacity": 16384,
                    "max_logical_write_bytes": 4294967296, "max_artifact_bytes": 4294967296},
    "balanced-8000": {"rates": [8000], "read_percent": 50, "threads": 64, "queue_capacity": 8192,
                      "max_logical_write_bytes": 8589934592, "max_artifact_bytes": 8589934592},
    "write-heavy-5556": {"rates": [5556], "read_percent": 10, "threads": 64, "queue_capacity": 8192,
                         "max_logical_write_bytes": 8589934592, "max_artifact_bytes": 8589934592},
}

# name: (CUDA sync mode, primary NUMA node or "", RocksDB max_background_jobs)
CONDITIONS = {
    "auto-unpinned": ("auto", "", 1),
    "blocking-unpinned": ("blocking", "", 1),
    "auto-node0": ("auto", "0", 1),
    "blocking-node0": ("blocking", "0", 1),
    "bg3-auto-unpinned": ("auto", "", 3),
}

CELLS = [(w, c) for w in ("stress-5000", "balanced-8000") for c in CONDITIONS] + [("write-heavy-5556", "auto-unpinned")]


def write_json(path: Path, value) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def schedule(blocks: int, seed: int) -> list:
    generator = random.Random(seed)
    runs = []
    for block in range(blocks):
        cells = list(CELLS)
        generator.shuffle(cells)
        for workload, condition in cells:
            runs.append({"block": block, "workload": workload, "condition": condition,
                         "seed": seed * 1000 + block * 100 + CELLS.index((workload, condition)),
                         "id": f"b{block}-{workload}-{condition}"})
    return runs


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--blocks", type=int, default=6)
    parser.add_argument("--seed", type=int, default=2026)
    parser.add_argument("--python", default=sys.executable)
    parser.add_argument("--gpu-input", action="store_true",
                        help="also offload compaction input decompression (the published runs disabled it)")
    parser.add_argument("--label", default="revision")
    args = parser.parse_args()

    output = args.output.resolve()
    output.mkdir(parents=False, exist_ok=False)
    (output / "configs").mkdir()
    (output / "runs").mkdir()
    for jobs in sorted({c[2] for c in CONDITIONS.values()}):
        spec = json.loads(json.dumps(BASE_SPEC))
        spec["engine"]["background_jobs"] = jobs
        write_json(output / "configs" / f"dataset-bg{jobs}.json", spec)

    runs = schedule(args.blocks, args.seed)
    campaign = {"schema_version": 1, "label": args.label, "gpu_input": args.gpu_input,
                "seed": args.seed, "blocks": args.blocks, "conditions": CONDITIONS,
                "workloads": WORKLOADS, "runs": runs, "state": "running"}
    write_json(output / "campaign.json", campaign)

    energy = output / "rapl.csv"
    perf_monotonic_ns = time.monotonic_ns()
    perf = subprocess.Popen(["perf", "stat", "-a", "-e", "power/energy-pkg/,power/energy-ram/", "-I", "1000",
                             "-x", ",", "-o", str(energy)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    campaign["rapl"] = {"file": energy.name, "start_monotonic_ns": perf_monotonic_ns,
                        "interval_ms": 1000, "scope": "all sockets (package) and DRAM, whole host"}
    try:
        for run in runs:
            sync, node, jobs = CONDITIONS[run["condition"]]
            config = {"schema_version": 1, "name": f"rev-{run['id']}",
                      "dataset_spec": str(output / "configs" / f"dataset-bg{jobs}.json"),
                      "modes": ["native_cpu", "cuda_local"], "repetitions": 1, "seed": run["seed"],
                      "warmup_seconds": 2, "measurement_seconds": 8, "deadline_ms": 5000,
                      "read_slo_ms": 100, "write_slo_ms": 100, "max_wall_seconds": 300,
                      **WORKLOADS[run["workload"]]}
            config_path = output / "configs" / f"{run['id']}.json"
            plan_path = output / "configs" / f"{run['id']}-plan.json"
            write_json(config_path, config)
            env = dict(os.environ, LSM_CUDA_SYNC=sync, LSM_LAB_PRIMARY_NUMA_NODE=node)
            env.pop("LSM_DISABLE_GPU_INPUT", None)
            if not args.gpu_input:
                env["LSM_DISABLE_GPU_INPUT"] = "1"
            run["start_monotonic_ns"] = time.monotonic_ns()
            planned = subprocess.run([args.python, str(LABCTL), "plan", str(config_path), str(plan_path)],
                                     cwd=ROOT, env=env, capture_output=True, text=True)
            executed = None
            if planned.returncode == 0:
                executed = subprocess.run([args.python, str(LABCTL), "run", str(plan_path), str(output / "runs" / run["id"])],
                                          cwd=ROOT, env=env, capture_output=True, text=True)
            run["end_monotonic_ns"] = time.monotonic_ns()
            run["plan_exit"] = planned.returncode
            run["run_exit"] = None if executed is None else executed.returncode
            if planned.returncode != 0 or executed is None or executed.returncode != 0:
                run["error"] = (planned.stderr if planned.returncode else executed.stderr)[-2000:]
            write_json(output / "campaign.json", campaign)
            print(f"{run['id']}: plan={run['plan_exit']} run={run['run_exit']}", flush=True)
        campaign["state"] = "complete"
    finally:
        perf.terminate()
        perf.wait(timeout=30)
        write_json(output / "campaign.json", campaign)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
