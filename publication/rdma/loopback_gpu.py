#!/usr/bin/env python3
"""Remote GPU compaction without RDMA hardware (PREREGISTRATION.md, V6): the compaction server
runs on the GPU's NUMA node and the database on another node of the same machine, connected
over loopback TCP. This measures what moving compaction to a separate process and address
space costs, without a network in between.

  loopback_gpu.py OUT_DIR --golden DIR [--flags tuned.json] [--repetitions 3]

Designs, each with 4 database cores on --db-node:
  native       RocksDB compaction on the database cores
  local_gpu    cuda_merge in the database process
  remote_gpu   remote_merge to lsm_compaction_server --backend cuda_merge on 8 cores of --server-node
  remote_cpu   remote_merge to lsm_compaction_server --backend cpu_merge on the same 8 cores
Results (lsm_system_bench JSON plus the server's CPU seconds) go to OUT_DIR/results.jsonl.
"""

from __future__ import annotations

import argparse
import json
import os
import random
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from system_campaign import GPU_ENV, physical_cores  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
BUILD = Path(os.environ.get("LSM_BUILD_DIR", ROOT / "build" / "zyra-cuda"))


def cpu_seconds(pid: int) -> float:
    fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")


def start_server(backend: str, cpus: list[int], node: int, log: Path) -> tuple[subprocess.Popen, str]:
    command = ["numactl", f"--physcpubind={','.join(map(str, cpus))}", f"--membind={node}",
               str(BUILD / "lsm_compaction_server"), "--listen", "tcp://127.0.0.1:0", "--backend", backend]
    process = subprocess.Popen(command, stdout=log.open("w"), stderr=subprocess.STDOUT, env={**os.environ, **GPU_ENV})
    for _ in range(600):
        for line in log.read_text().splitlines():
            if line.startswith("listening "):
                return process, line.split()[1]
        if process.poll() is not None:
            raise RuntimeError(f"server exited: {log.read_text()}")
        time.sleep(0.1)
    process.kill()
    raise RuntimeError("server did not start")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("out", type=Path)
    parser.add_argument("--golden", type=Path, required=True)
    parser.add_argument("--scratch", type=Path, default=Path(f"/dev/shm/{os.environ.get('USER', 'lsm')}/loopback"))
    parser.add_argument("--flags", type=Path, help="tuned.json; uses the 4-core settings of each design")
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--db-node", type=int, default=2)
    parser.add_argument("--server-node", type=int, default=0)
    parser.add_argument("--warmup-seconds", type=int, default=60)
    parser.add_argument("--duration-seconds", type=int, default=300)
    arguments = parser.parse_args()
    arguments.out.mkdir(parents=True, exist_ok=True)
    results_path = arguments.out / "results.jsonl"
    done = {(r["repetition"], r["design"]) for r in map(json.loads, results_path.open())} if results_path.exists() else set()

    tuned = {}
    if arguments.flags:
        for choice in json.loads(arguments.flags.read_text())["choices"]:
            if choice["cores"] == 4:
                tuned[choice["design"]] = {k: v for k, v in choice["flags"].items() if v is not None}
    db_cpus, _ = physical_cores(arguments.db_node, 4)
    server_cpus, _ = physical_cores(arguments.server_node, 8)
    designs = {"native": ("native_cpu", None, "native"), "local_gpu": ("cuda_merge", None, "full"),
               "remote_gpu": ("remote_merge", "cuda_merge", "full"), "remote_cpu": ("remote_merge", "cpu_merge", "full")}
    database = arguments.scratch / "db"
    arguments.scratch.mkdir(parents=True, exist_ok=True)
    with results_path.open("a") as results:
        for repetition in range(1, arguments.repetitions + 1):
            order = list(designs)
            random.Random(700 + repetition).shuffle(order)
            for design in order:
                if (repetition, design) in done:
                    continue
                mode, backend, flags_from = designs[design]
                flags = {"records": 50000000, "batch-size": 100, "disable-wal": True, "threads": 4,
                         "background-jobs": 4, "warmup-seconds": arguments.warmup_seconds,
                         "duration-seconds": arguments.duration_seconds, "verify": True,
                         **tuned.get(flags_from, {})}
                server, endpoint, before = None, None, 0.0
                environment = {**os.environ, **GPU_ENV}
                try:
                    if backend:
                        server, endpoint = start_server(backend, server_cpus, arguments.server_node,
                                                        arguments.out / f"server-{design}-{repetition}.log")
                        environment["LSM_REMOTE_ENDPOINT"] = endpoint
                        before = cpu_seconds(server.pid)
                    shutil.rmtree(database, ignore_errors=True)
                    subprocess.run(["cp", "-a", str(arguments.golden), str(database)], check=True)
                    command = ["numactl", f"--physcpubind={','.join(map(str, db_cpus))}", f"--membind={arguments.db_node}",
                               str(BUILD / "lsm_system_bench"), "--phase", "run", "--mode", mode, "--dir", str(database)]
                    for name, value in flags.items():
                        command += [f"--{name}", str(value).lower() if isinstance(value, bool) else str(value)]
                    run = subprocess.run(command, capture_output=True, text=True, env=environment)
                    server_cpu = cpu_seconds(server.pid) - before if server else 0.0
                    record = {"repetition": repetition, "design": design, "endpoint": endpoint, "db_cpus": db_cpus,
                              "server_cpus": server_cpus if backend else None,
                              "exit_code": run.returncode, "server_cpu_seconds": server_cpu, "flags": flags}
                    if run.returncode == 0:
                        result = json.loads(run.stdout[run.stdout.index("{"):])
                        record["result"] = result
                        print(f"rep {repetition} {design}: {result['throughput']['ops_per_second']:.0f} ops/s, "
                              f"server CPU {server_cpu:.0f} s", flush=True)
                    else:
                        record["error"] = run.stderr[-2000:]
                        print(f"rep {repetition} {design}: failed {run.stderr[-300:]}", flush=True)
                    results.write(json.dumps(record) + "\n")
                    results.flush()
                finally:
                    if server:
                        server.terminate()
                        try:
                            server.wait(timeout=30)
                        except subprocess.TimeoutExpired:
                            server.kill()
                    shutil.rmtree(database, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
