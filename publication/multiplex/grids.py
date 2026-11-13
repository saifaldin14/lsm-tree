#!/usr/bin/env python3
"""Grids for the GPU multiplexing campaigns (PREREGISTRATION.md, "GPU multiplexing").

  python3 publication/multiplex/grids.py OUT_DIR --engines K

M1 (local sharing, zyra): N databases of 4 cores (N in 1, 2, 4, 6) and of 8 cores (N in 1, 2,
4), each on its own cores within one NUMA node, the same cores in every arm (4 cores of the
GPU's node are reserved for the compaction server in every arm). Arms:
  native            RocksDB, no GPU (N = 1 and the largest N)
  separate          GPU offload in each database process (own CUDA context and engines)
  mps               the same through CUDA MPS
  service-fifo      one shared compaction server, K engines, arrival order
  service-fair      the same, fair queuing by tenant
Each database uses the tuned settings of configs/system/v3-headline.json for its core count.

M3 (isolation, zyra): through the service with 1 engine, one heavy tenant (closed loop) and
three light tenants (open loop at --light-rate writes/s), 4 cores each, with each policy, and
one light tenant alone at the same rate.

M4 (burst, zyra): as M3 under fair and fifo, the heavy tenant starting 60 s after the light
tenants and running 30 s plus 60 s; the light tenants run 30 s plus 180 s.

E1 (exploratory, added after M1, not registered): service-fair with 4, 6 and 8 engines and MPS
at 8 cores x 4 databases and 4 cores x 6 databases, in the same blocks. In M1 the service's
jobs waited 550 to 750 ms for one of its 4 engines while the GPU was 64 to 69% busy.

E2 (exploratory, added after the first M3 trials, not registered): the M3 tenants with the same
placement and rates, (a) as separate processes, each with its own engines and no shared queue,
and (b) through the service with 4 engines (fair). In M3 the light tenant on the heavy tenant's
NUMA node was slowed more than the one on another node, so part of the slowdown is not the queue.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
COMMON = {"records": 50000000, "read-percent": 0, "warmup-seconds": 30, "duration-seconds": 120, "verify": True}


def tuned() -> dict:
    headline = json.loads((ROOT / "configs" / "system" / "v3-headline.json").read_text())
    return {(t["design"], t["cores"]): t for t in headline["trials"]}


def arm(name: str, cores: int, instances: int, engines: int, settings: dict) -> dict:
    design = "native" if name == "native" else "full"
    base = settings[(design, cores)]
    trial = {"design": name, "cores": cores, "instances": instances, "writes": "batched", "flags": dict(base["flags"]),
             "placement": "per_node", "reserved_cores": 4}
    if name == "native":
        trial["mode"] = "native_cpu"
    elif name in ("separate", "mps"):
        trial["mode"] = "cuda_merge"
        trial["sharing"] = name
    else:
        trial["mode"] = "remote_merge"
        trial["sharing"] = "service"
        trial["scheduler"] = name.split("-")[1]
        trial["engines"] = engines
        trial["server_cores"] = 4
        trial["connections"] = 4
    return trial


def m1(engines: int) -> dict:
    settings = tuned()
    trials = []
    for cores, counts in ((4, (1, 2, 4, 6)), (8, (1, 2, 4))):
        for n in counts:
            for name in ("separate", "mps", "service-fifo", "service-fair"):
                trials.append(arm(name, cores, n, engines, settings))
        for n in (1, counts[-1]):
            trials.append(arm("native", cores, n, engines, settings))
    return {"_comment": "M1 local GPU sharing; PREREGISTRATION.md, GPU multiplexing.", "common": COMMON, "trials": trials}


def service(cores: int, instances: int, engines: int, policy: str, settings: dict) -> dict:
    trial = arm(f"service-{policy}", cores, instances, engines, settings)
    trial["design"] = f"service-{policy}"
    return trial


def m3(light_rate: int) -> dict:
    settings = tuned()
    light = {"rate": light_rate}
    trials = []
    for policy in ("fifo", "fair", "stall"):
        trial = service(4, 4, 1, policy, settings)
        trial["instance_flags"] = [{}, light, light, light]
        trial["role"] = "heavy+3light"
        trials.append(trial)
    solo = service(4, 1, 1, "fair", settings)
    solo["instance_flags"] = [light]
    solo["role"] = "light-solo"
    trials.append(solo)
    return {"_comment": "M3 isolation; PREREGISTRATION.md, GPU multiplexing.", "common": COMMON, "trials": trials,
            "light_rate": light_rate}


def m4(light_rate: int) -> dict:
    settings = tuned()
    light = {"rate": light_rate, "duration-seconds": 180}
    trials = []
    for policy in ("fifo", "fair"):
        trial = service(4, 4, 1, policy, settings)
        trial["instance_flags"] = [{"duration-seconds": 60}, light, light, light]
        trial["instance_start_delay"] = [60, 0, 0, 0]
        trial["role"] = "burst"
        trials.append(trial)
    return {"_comment": "M4 burst; PREREGISTRATION.md, GPU multiplexing.", "common": COMMON, "trials": trials,
            "light_rate": light_rate}


def e1() -> dict:
    settings = tuned()
    trials = []
    for cores, n in ((8, 4), (4, 6)):
        for engines in (4, 6, 8):
            trial = service(cores, n, engines, "fair", settings)
            trial["design"] = f"service-fair-k{engines}"
            trials.append(trial)
        trials.append(arm("mps", cores, n, 4, settings))
    return {"_comment": "E1 exploratory engine count; PREREGISTRATION.md, Changes after data collection.",
            "common": COMMON, "trials": trials}


def e2(light_rate: int) -> dict:
    settings = tuned()
    light = {"rate": light_rate}
    trials = []
    for name, engines in (("separate", 4), ("service-fair", 4)):
        trial = arm(name, 4, 4, engines, settings)
        trial["design"] = name if name == "separate" else f"service-fair-k{engines}"
        trial["instance_flags"] = [{}, light, light, light]
        trial["role"] = "heavy+3light"
        trials.append(trial)
    return {"_comment": "E2 exploratory isolation controls; PREREGISTRATION.md, Changes after data collection.",
            "common": COMMON, "trials": trials, "light_rate": light_rate}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("out", type=Path)
    parser.add_argument("--engines", type=int, required=True)
    parser.add_argument("--light-rate", type=int, help="M3/M4: open-loop writes/s of each light tenant")
    arguments = parser.parse_args()
    arguments.out.mkdir(parents=True, exist_ok=True)
    grid = m1(arguments.engines)
    (arguments.out / "m1-grid.json").write_text(json.dumps(grid, indent=1))
    print(f"m1: {len(grid['trials'])} trials")
    grid = e1()
    (arguments.out / "e1-grid.json").write_text(json.dumps(grid, indent=1))
    print(f"e1: {len(grid['trials'])} trials")
    if arguments.light_rate:
        for name, make in (("m3", m3), ("m4", m4), ("e2", e2)):
            grid = make(arguments.light_rate)
            (arguments.out / f"{name}-grid.json").write_text(json.dumps(grid, indent=1))
            print(f"{name}: {len(grid['trials'])} trials")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
