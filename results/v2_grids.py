#!/usr/bin/env python3
"""Writes the v2 campaign grids (PREREGISTRATION.md, "Methodology v2") from the frozen tuning
choice, so no setting is edited by hand after tuning.

  v2_grids.py configs/system/tuned.json

Writes configs/system/v2-headline.json (V1), v2-stream.json, v2-metadata.json and
v2-timeseries.json (V2), and v2-ssd.json (V4). Workloads keep their own write mode
(docs/workloads.md) and take the tuned concurrency settings of their design and core count.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CONFIGS = ROOT / "configs" / "system"
MODES = {"native": "native_cpu", "full": "cuda_merge"}
BATCHED = {"batch-size": 100, "disable-wal": True}
LONG = {"warmup-seconds": 120, "duration-seconds": 600}
WORKLOAD_TIME = {"warmup-seconds": 60, "duration-seconds": 300}

# docs/workloads.md
WORKLOADS = {
    "stream": {"key-bytes": 24, "value-bytes": 64, "value-bytes-max": 512, "distribution": "zipfian",
               "zipf-theta": 0.99, "rmw-percent": 70, "delete-percent": 10, "disable-wal": True},
    "metadata": {"key-bytes": 20, "key-bytes-max": 60, "value-bytes": 0, "value-bytes-max": 200,
                 "key-families": 6, "read-percent": 25, "scan-percent": 5, "scan-length": 20,
                 "delete-percent": 20, "distribution": "zipfian", "zipf-theta": 0.8},
    "timeseries": {"workload": "timeseries", "retention": 1250, "value-bytes": 32, "batch-size": 100,
                   "read-percent": 5, "scan-percent": 5, "scan-length": 60},
}


def tuned_flags(choices: dict, design: str, cores: int) -> dict:
    choice = choices[(design, cores)]
    return {k: v for k, v in choice["flags"].items() if v is not None}


def trial(design: str, cores: int, flags: dict, writes: str) -> dict:
    return {"design": design, "mode": MODES[design], "cores": cores, "writes": writes, "flags": flags}


def write(name: str, comment: str, common: dict, trials: list[dict]) -> None:
    path = CONFIGS / name
    path.write_text(json.dumps({"_comment": comment, "common": common, "trials": trials}, indent=1) + "\n")
    print(f"{path.relative_to(ROOT)}: {len(trials)} trials")


def main() -> int:
    tuned = json.loads(Path(sys.argv[1]).read_text())
    choices = {(c["design"], c["cores"]): c for c in tuned["choices"]}
    common = {"records": 50000000, "verify": True, "read-percent": 0}

    write("v2-headline.json", "V1: batched writes, LZ4, /dev/shm, tuned settings (configs/system/tuned.json).",
          {**common, **LONG},
          [trial(d, c, {**tuned_flags(choices, d, c), **BATCHED}, "batched") for d in MODES for c in (4, 8, 16, 32)])

    for name, shape in WORKLOADS.items():
        trials = []
        for d in MODES:
            for c in (8, 16):
                flags = tuned_flags(choices, d, c)
                # Single-operation workloads keep one client per core (more clients only queue).
                if shape.get("batch-size", 1) == 1:
                    flags.update({"threads": c, "adaptive-yield": True})
                trials.append(trial(d, c, {**flags, **shape}, name))
        write(f"v2-{name}.json", f"V2: {name} workload (docs/workloads.md) with tuned concurrency settings.",
              {**common, **WORKLOAD_TIME}, trials)

    write("v2-ssd.json", "V4: SATA SSD with direct I/O, batched writes, LZ4, tuned settings.",
          {**common, **LONG, "use-direct-io": True},
          [trial(d, c, {**tuned_flags(choices, d, c), **BATCHED}, "batched") for d in MODES for c in (8, 16)])
    return 0


if __name__ == "__main__":
    sys.exit(main())
