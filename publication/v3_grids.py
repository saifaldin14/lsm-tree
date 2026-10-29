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
    write("v3-openloop.json", "X5: open loop at fixed rates (keys/s), LZ4, 8 cores, tuned.",
          {**common, "warmup-seconds": 60, "duration-seconds": 180},
          [trial(d, 8, {"rate": rate}, f"open-{rate}") for d in MODES for rate in OPEN_LOOP_RATES])
    return 0


if __name__ == "__main__":
    sys.exit(main())
