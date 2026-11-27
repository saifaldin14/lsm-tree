#!/usr/bin/env python3
"""Writes the v3 whole-database grids (PREREGISTRATION.md, "Methodology v3") from the frozen
v2 tuning choice: X3 (LZ4), X4 (LZ4 above, ZSTD at the bottom) and X5 (open loop).

  v3_grids.py configs/system/tuned.json
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CONFIGS = ROOT / "configs" / "system"
MODES = {"native": "native_cpu", "full": "cuda_merge"}
BATCHED = {"batch-size": 100, "disable-wal": True, "allow-fallocate": False}
OPEN_LOOP_RATES = [430000, 600000, 770000]
# docs/workloads.md (as v2_grids.py)
WORKLOADS = {
    "stream": {"key-bytes": 24, "value-bytes": 64, "value-bytes-max": 512, "distribution": "zipfian",
               "zipf-theta": 0.99, "rmw-percent": 70, "delete-percent": 10, "disable-wal": True},
    "metadata": {"key-bytes": 20, "key-bytes-max": 60, "value-bytes": 0, "value-bytes-max": 200,
                 "key-families": 6, "read-percent": 25, "scan-percent": 5, "scan-length": 20,
                 "delete-percent": 20, "distribution": "zipfian", "zipf-theta": 0.8},
    "timeseries": {"workload": "timeseries", "retention": 1250, "value-bytes": 32, "batch-size": 100,
                   "read-percent": 5, "scan-percent": 5, "scan-length": 60},
}


def write(name: str, comment: str, common: dict, trials: list[dict]) -> None:
    path = CONFIGS / name
    path.write_text(json.dumps({"_comment": comment, "common": common, "trials": trials}, indent=1) + "\n")
    print(f"{path.relative_to(ROOT)}: {len(trials)} trials")


def main() -> int:
    tuned = json.loads(Path(sys.argv[1]).read_text())
    choices = {(c["design"], c["cores"]): {k: v for k, v in c["flags"].items() if v is not None}
               for c in tuned["choices"]}
    common = {"records": 50000000, "verify": True, "read-percent": 0}
    trial = lambda d, c, extra, writes: {"design": d, "mode": MODES[d], "cores": c, "writes": writes,
                                         "flags": {**choices[(d, c)], **BATCHED, **extra}}
    write("v3-headline.json", "X3: LZ4, closed loop, tuned (v2 choice), fallocate off for both.",
          {**common, "warmup-seconds": 120, "duration-seconds": 300},
          [trial(d, c, {}, "batched") for d in MODES for c in (4, 8, 16)])
    write("v3-bottomzstd.json", "X4: LZ4 above, ZSTD at the bottommost level, closed loop, tuned.",
          {**common, "warmup-seconds": 120, "duration-seconds": 300, "compression": "lz4"},
          [trial(d, c, {"bottommost-compression": "zstd"}, "batched") for d in MODES for c in (4, 8)])
    for name, shape in WORKLOADS.items():
        trials = []
        for d in MODES:
            for c in (8, 16):
                flags = {**choices[(d, c)], "allow-fallocate": False}
                # Single-operation workloads keep one client per core (more clients only queue).
                if shape.get("batch-size", 1) == 1:
                    flags.update({"threads": c, "adaptive-yield": True})
                trials.append({"design": d, "mode": MODES[d], "cores": c, "writes": name, "flags": {**flags, **shape}})
        write(f"v3-{name}.json", f"X6: {name} workload (docs/workloads.md), tuned concurrency, fallocate off.",
              {**common, "warmup-seconds": 60, "duration-seconds": 300}, trials)
    write("v3-ssd.json", "X8: SATA SSD with direct I/O, batched writes, LZ4, tuned, fallocate off.",
          {**common, "warmup-seconds": 120, "duration-seconds": 300, "use-direct-io": True},
          [trial(d, c, {}, "batched") for d in MODES for c in (8, 16)])
    write("v3-openloop.json", "X5: open loop at fixed rates (keys/s), LZ4, 8 cores, tuned.",
          {**common, "warmup-seconds": 60, "duration-seconds": 180},
          [trial(d, 8, {"rate": rate}, f"open-{rate}") for d in MODES for rate in OPEN_LOOP_RATES])
    return 0


if __name__ == "__main__":
    sys.exit(main())
