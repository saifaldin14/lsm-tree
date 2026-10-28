#!/usr/bin/env python3
"""Multi-host experiments for remote compaction and replication over RDMA
(PREREGISTRATION.md, "Remote compaction and replication over RDMA").

  python3 publication/rdma/rdma_campaign.py --hosts HOSTS.json --experiment r1 --out OUT_DIR

HOSTS.json describes the database node (client), the compaction server and the backups
(see hosts.example.json; keep the filled-in file outside the repository). Every remote
command runs over ssh; servers are started in the background and stopped by PID.

Experiments:
  r1  transport: lsm_rpc_bench over RDMA push, RDMA pull and TCP, plus ib_write_bw and
      ib_read_bw, for 64 KiB to 256 MiB, 3 repetitions
  r2  isolated compaction on the database node: native, local cpu_merge, and remote_merge
      to the server's backend over RDMA push, RDMA pull and TCP (isolated-sweep workloads)
  r6  replication with 2 backups: per-replica compaction (logical), compact once on the
      primary's CPU (physical) and compact once on the compaction server (physical +
      remote_merge), for batched writes (WAL off) and single writes (WAL on)
Results are JSON lines in OUT_DIR; every run is resumable (finished files are skipped).
"""

from __future__ import annotations

import argparse
import json
import random
import shlex
import subprocess
import sys
import time
from pathlib import Path

GPU_ENV = "LSM_DISABLE_GPU_INPUT=1 LSM_CUDA_SYNC=blocking LSM_GPU_OUTPUT_MIN_BLOCK_BYTES=1024 " \
          "LSM_MERGE_READ_THREADS=4 LSM_GPU_MERGE_STREAMS=2"
SIZES = ["64K", "1M", "16M", "128M", "256M"]
BYTES = {"64K": 1 << 16, "1M": 1 << 20, "16M": 1 << 24, "128M": 1 << 27, "256M": 1 << 28}
# value bytes, records, compression, delete fraction (as publication/compaction_sweep.sh)
WORKLOADS = [(32, 10_000_000, "lz4", 0), (128, 4_000_000, "lz4", 0), (1024, 1_000_000, "lz4", 0),
             (128, 4_000_000, "none", 0), (128, 4_000_000, "zstd", 0), (128, 4_000_000, "lz4", 0.1)]


class Cluster:
    def __init__(self, config: dict):
        self.config = config
        self.repo = config["repo"]
        self.build = config["build"]

    def run(self, host: str, command: str, check: bool = True, timeout: int | None = None) -> str:
        # Not "cd && command": with a trailing "&" that would background the whole list in a
        # subshell that keeps ssh's output open.
        full = f"cd {self.repo} || exit 1; {command}"
        result = subprocess.run(self.config["ssh"] + [host, full], capture_output=True, text=True, timeout=timeout)
        if check and result.returncode != 0:
            raise RuntimeError(f"{host}: {command}\n{result.stdout[-2000:]}\n{result.stderr[-2000:]}")
        return result.stdout

    def start(self, host: str, command: str, log: str) -> tuple[int, str]:
        """Starts a server in the background; returns its PID and the endpoint it reports."""
        wrapped = f"nohup setsid bash -c {shlex.quote(command)} > {log} 2>&1 < /dev/null & echo $!"
        pid = int(self.run(host, wrapped).strip().splitlines()[-1])
        for _ in range(300):
            out = self.run(host, f"cat {log} 2>/dev/null || true")
            for line in out.splitlines():
                if line.startswith("listening "):
                    return pid, line.split()[1]
            time.sleep(0.2)
        raise RuntimeError(f"{host}: server did not start: {self.run(host, f'cat {log}', check=False)}")

    def stop(self, host: str, pid: int) -> str:
        """SIGTERM, then returns the process's final output (statistics)."""
        self.run(host, f"kill {pid} 2>/dev/null; for i in $(seq 1 50); do kill -0 {pid} 2>/dev/null || exit 0; "
                       f"sleep 0.2; done; kill -9 {pid} 2>/dev/null; true", check=False)
        return ""

    def load(self, host: str) -> dict:
        out = self.run(host, "cat /proc/loadavg; who | wc -l", check=False).split()
        return {"load_1m": float(out[0]), "users": int(out[-1])} if out else {}


def pin(machine: dict) -> str:
    return f"numactl --physcpubind={machine['cores']} --membind={machine['node']}"


def append(path: Path, record: dict) -> None:
    with path.open("a") as out:
        out.write(json.dumps(record) + "\n")


def experiment_r1(cluster: Cluster, out: Path, repetitions: int) -> None:
    c, s = cluster.config["client"], cluster.config["server"]
    results = out / "r1-transport.jsonl"
    done = {(r["repetition"], r["variant"]) for r in map(json.loads, results.open())} if results.exists() else set()
    for repetition in range(1, repetitions + 1):
        variants = ["rdma_push", "rdma_pull", "tcp", "ib_write_bw", "ib_read_bw"]
        random.Random(repetition).shuffle(variants)
        for variant in variants:
            if (repetition, variant) in done:
                continue
            record = {"experiment": "r1", "repetition": repetition, "variant": variant,
                      "load_before": {"client": cluster.load(c["host"]), "server": cluster.load(s["host"])}}
            if variant.startswith("ib_"):
                rows = []
                for size in SIZES:
                    server = f"timeout 120 {variant} -R -d {s['device']} -s {BYTES[size]} -n 100 --report_gbits -q 1"
                    log = f"/tmp/lsm-{variant}.log"
                    cluster.run(s["host"], f"nohup setsid {server} > {log} 2>&1 < /dev/null &")
                    time.sleep(1.5)
                    text = cluster.run(c["host"], f"timeout 120 {variant} -R -d {c['device']} -s {BYTES[size]} -n 100 "
                                                  f"--report_gbits -q 1 {s['address']}", check=False)
                    numbers = [line.split() for line in text.splitlines() if line.strip().startswith(str(BYTES[size]))]
                    rows.append({"bytes": BYTES[size], "gbit_per_s": float(numbers[-1][3]) if numbers else None})
                record["rows"] = rows
            else:
                scheme = "tcp" if variant == "tcp" else "rdma"
                pid, endpoint = cluster.start(s["host"], f"{pin(s)} {cluster.build}/lsm_compaction_server "
                                                          f"--listen {scheme}://{s['address']}:0", "/tmp/lsm-r1-server.log")
                try:
                    env = "LSM_RDMA_PUSH=0" if variant == "rdma_pull" else "LSM_RDMA_PUSH=1"
                    text = cluster.run(c["host"], f"{env} {pin(c)} {cluster.build}/lsm_rpc_bench --endpoint {endpoint} "
                                                  f"--sizes {','.join(SIZES)} --iterations 20")
                    record["rows"] = [json.loads(line) for line in text.splitlines() if line.startswith("{")]
                finally:
                    cluster.stop(s["host"], pid)
            append(results, record)
            print(f"r1 repetition {repetition} {variant} done", flush=True)


def experiment_r2(cluster: Cluster, out: Path) -> None:
    c, s = cluster.config["client"], cluster.config["server"]
    scratch = f"{cluster.config['scratch']}/r2"
    (out / "r2").mkdir(exist_ok=True)
    servers = {}
    try:
        for scheme in ("rdma", "tcp"):
            servers[scheme] = cluster.start(
                s["host"], f"{GPU_ENV} {pin(s)} {cluster.build}/lsm_compaction_server --listen {scheme}://{s['address']}:0 "
                           f"--backend {s['backend']}", f"/tmp/lsm-r2-server-{scheme}.log")
        for value, records, codec, deletes in WORKLOADS:
            tag = f"v{value}-{codec}" + (f"-del{deletes}" if deletes else "")
            common = (f"--dir {scratch} --records {records} --value-bytes {value} --compression {codec} --l0-files 8 "
                      f"--block-bytes 4096 --delete-fraction {deletes} --repetitions 4")
            variants = [("local", "", "native_cpu,cpu_merge"),
                        ("remote-rdma-push", f"LSM_RDMA_PUSH=1 LSM_REMOTE_ENDPOINT={servers['rdma'][1]}", "remote_merge"),
                        ("remote-rdma-pull", f"LSM_RDMA_PUSH=0 LSM_REMOTE_ENDPOINT={servers['rdma'][1]}", "remote_merge"),
                        ("remote-tcp", f"LSM_REMOTE_ENDPOINT={servers['tcp'][1]}", "remote_merge")]
            for name, env, modes in variants:
                target = out / "r2" / f"{tag}-{name}.jsonl"
                if target.exists() and target.stat().st_size > 0:
                    continue
                text = cluster.run(c["host"], f"rm -rf {scratch} && {GPU_ENV} {env} {pin(c)} "
                                              f"{cluster.build}/lsm_compaction_bench --modes {modes} {common}")
                rows = []
                for line in text.splitlines():
                    if line.startswith("{"):
                        row = json.loads(line)
                        row["variant"] = name
                        row["server_backend"] = s["backend"] if name != "local" else None
                        rows.append(json.dumps(row))
                target.write_text("\n".join(rows) + "\n")
                print(f"r2 {tag} {name} done", flush=True)
        cluster.run(c["host"], f"rm -rf {scratch}", check=False)
    finally:
        for pid, _ in servers.values():
            cluster.stop(s["host"], pid)


def experiment_r6(cluster: Cluster, out: Path, repetitions: int, records: int, seconds: tuple[int, int]) -> None:
    c, s, backups = cluster.config["client"], cluster.config["server"], cluster.config["backups"]
    scratch = cluster.config["scratch"]
    golden = f"{scratch}/rdma-golden-{records}"
    if "done" not in cluster.run(c["host"], f"test -f {golden}.done && echo done || true"):
        cluster.run(c["host"], f"rm -rf {golden} && {cluster.build}/lsm_system_bench --phase preload --dir {golden} "
                               f"--records {records} --threads 8 > {golden}.json 2>&1 && touch {golden}.done")
    results = out / "r6-replication.jsonl"
    done = {(r["repetition"], r["design"], r["writes"]) for r in map(json.loads, results.open())} \
        if results.exists() else set()
    designs = {
        "per_replica": "--mode native_cpu --replication logical --replica-backend native_cpu",
        "once_primary_cpu": "--mode native_cpu --replication physical",
        "once_server": "--mode remote_merge --replication physical",
    }
    writes = {"batched": "--batch-size 100 --disable-wal true", "single": "--batch-size 1"}
    server = None
    try:
        server = cluster.start(s["host"], f"{GPU_ENV} {pin(s)} {cluster.build}/lsm_compaction_server "
                                          f"--listen rdma://{s['address']}:0 --backend {s['backend']}",
                               "/tmp/lsm-r6-server.log")
        for repetition in range(1, repetitions + 1):
            order = [(d, w) for d in designs for w in writes]
            random.Random(600 + repetition).shuffle(order)
            for design, write in order:
                if (repetition, design, write) in done:
                    continue
                started = []
                try:
                    endpoints = []
                    for index, backup in enumerate(backups):
                        directory = f"{scratch}/rdma-backup-{index}"
                        cluster.run(backup["host"], f"rm -rf {directory} && mkdir -p {directory}")
                        pid, endpoint = cluster.start(
                            backup["host"], f"{GPU_ENV} {pin(backup)} {cluster.build}/lsm_replica_server "
                                            f"--listen rdma://{backup['address']}:0 --dir {directory}",
                            f"/tmp/lsm-r6-backup-{index}.log")
                        started.append((backup["host"], pid))
                        endpoints.append(endpoint)
                    primary = f"{scratch}/rdma-primary"
                    load_before = {m: cluster.load(h) for m, h in
                                   [("client", c["host"]), ("server", s["host"])] + [(f"backup{i}", b["host"]) for i, b in enumerate(backups)]}
                    command = (f"rm -rf {primary} && cp -a --reflink=auto {golden} {primary} && "
                               f"{GPU_ENV} LSM_REMOTE_ENDPOINT={server[1]} {pin(c)} {cluster.build}/lsm_system_bench "
                               f"--phase run --dir {primary} --records {records} --threads 4 --background-jobs 4 "
                               f"--warmup-seconds {seconds[0]} --duration-seconds {seconds[1]} "
                               f"{designs[design]} {writes[write]} --replicate-to {','.join(endpoints)}")
                    text = cluster.run(c["host"], command)
                    result = json.loads(text[text.index("{"):])
                    result.pop("timeline", None)
                    append(results, {"experiment": "r6", "repetition": repetition, "design": design, "writes": write,
                                     "server_backend": s["backend"], "load_before": load_before, "result": result})
                    print(f"r6 repetition {repetition} {design} {write}: "
                          f"{result['ingest_and_compact']['ops_per_second']:.0f} ops/s "
                          f"match={result['replication']['match']}", flush=True)
                finally:
                    for host, pid in started:
                        cluster.stop(host, pid)
    finally:
        if server:
            cluster.stop(s["host"], server[0])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--hosts", type=Path, required=True)
    parser.add_argument("--experiment", choices=["r1", "r2", "r6"], required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--repetitions", type=int, default=None)
    parser.add_argument("--records", type=int, default=50_000_000)
    parser.add_argument("--warmup-seconds", type=int, default=30)
    parser.add_argument("--duration-seconds", type=int, default=120)
    arguments = parser.parse_args()
    cluster = Cluster(json.loads(arguments.hosts.read_text()))
    arguments.out.mkdir(parents=True, exist_ok=True)
    if arguments.experiment == "r1":
        experiment_r1(cluster, arguments.out, arguments.repetitions or 3)
    elif arguments.experiment == "r2":
        experiment_r2(cluster, arguments.out)
    else:
        experiment_r6(cluster, arguments.out, arguments.repetitions or 5, arguments.records,
                      (arguments.warmup_seconds, arguments.duration_seconds))
    return 0


if __name__ == "__main__":
    sys.exit(main())
