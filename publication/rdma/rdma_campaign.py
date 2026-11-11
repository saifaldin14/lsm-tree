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
  r7  GPU compaction over RDMA (HOSTS.json needs a "gpu" entry): isolated compaction on the
      database node, native against remote_merge to (a) the server's CPU over RDMA, (b) the
      server as a relay (--backend remote_merge) over RDMA that forwards each job over TCP to
      a cuda_merge server on the GPU host, and (c) the GPU host directly over TCP; plus, on the
      GPU host, local cuda_merge and a loopback (127.0.0.1) cuda_merge server. If the "gpu" entry
      has an "rdma_address", (d) remote-gpu-rdma sends each job over RDMA straight to a
      cuda_merge server on the GPU host (no relay). Every server is
      restarted per workload and variant so its statistics give a per-hop time breakdown.
  r8  whole database over the r7 chain: lsm_system_bench on the database node (batched writes,
      WAL off, full verification) with native compaction, remote_merge to the server's CPU over
      RDMA, and remote_merge through the relay to the GPU host
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
GPU_SERVER_ENV = GPU_ENV + " LSM_GPU_PINNED_PREALLOC_MB=1024"
# The three headline isolated workloads (PREREGISTRATION.md, X1)
GPU_WORKLOADS = [(128, 4_000_000, "lz4"), (32, 10_000_000, "lz4"), (128, 4_000_000, "zstd")]
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
        # ssh exits with 255 when the connection itself fails; retry those (commands are idempotent
        # or restart from a clean scratch directory).
        for attempt in range(6):
            result = subprocess.run(self.config["ssh"] + [host, full], capture_output=True, text=True, timeout=timeout)
            if result.returncode != 255:
                break
            time.sleep(min(60, 5 * 2 ** attempt))
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

    def statistics(self, host: str, log: str) -> dict | None:
        """The statistics JSON a stopped lsm_compaction_server printed last."""
        lines = [line for line in self.run(host, f"cat {log}", check=False).splitlines() if line.startswith("{")]
        return json.loads(lines[-1]) if lines else None

    def load(self, host: str) -> dict:
        out = self.run(host, "cat /proc/loadavg; who | wc -l", check=False).split()
        return {"load_1m": float(out[0]), "users": int(out[-1])} if out else {}

    def cpu_seconds(self, host: str, pid: int) -> float | None:
        """User + system CPU of a process and its waited-for children, from /proc. Retries an
        empty read (a dropped ssh session); None if the process cannot be read."""
        for _ in range(5):
            out = self.run(host, f"cat /proc/{pid}/stat; getconf CLK_TCK", check=False).split("\n")
            fields = out[0].rsplit(")", 1)[-1].split()
            if len(fields) >= 15 and len(out) > 1 and out[1].strip().isdigit():
                return sum(int(value) for value in fields[11:15]) / int(out[1])
            time.sleep(2)
        return None


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
                    server_cpu_before = cluster.cpu_seconds(s["host"], server[0])
                    command = (f"rm -rf {primary} && cp -a --reflink=auto {golden} {primary} && "
                               f"{GPU_ENV} LSM_REMOTE_ENDPOINT={server[1]} {pin(c)} {cluster.build}/lsm_system_bench "
                               f"--phase run --dir {primary} --records {records} --threads 4 --background-jobs 4 "
                               f"--warmup-seconds {seconds[0]} --duration-seconds {seconds[1]} "
                               f"{designs[design]} {writes[write]} --replicate-to {','.join(endpoints)}")
                    text = cluster.run(c["host"], command)
                    server_cpu_after = cluster.cpu_seconds(s["host"], server[0])
                    result = json.loads(text[text.index("{"):])
                    result.pop("timeline", None)
                    append(results, {"experiment": "r6", "repetition": repetition, "design": design, "writes": write,
                                     "server_backend": s["backend"], "load_before": load_before,
                                     "server_cpu_seconds": (server_cpu_after - server_cpu_before
                                                            if None not in (server_cpu_after, server_cpu_before) else None),
                                     "result": result})
                    print(f"r6 repetition {repetition} {design} {write}: "
                          f"{result['ingest_and_compact']['ops_per_second']:.0f} ops/s "
                          f"match={result['replication']['match']}", flush=True)
                finally:
                    for host, pid in started:
                        cluster.stop(host, pid)
    finally:
        if server:
            cluster.stop(s["host"], server[0])


def experiment_r7(cluster: Cluster, out: Path, only: list[str] | None = None) -> None:
    c, s, g = cluster.config["client"], cluster.config["server"], cluster.config["gpu"]
    gpu_build = g.get("build", cluster.build)
    scratch = f"{cluster.config['scratch']}/r7"
    (out / "r7").mkdir(exist_ok=True)
    base = "--l0-files 8 --block-bytes 4096 --repetitions 4 --allow-fallocate false"
    for value, records, codec in GPU_WORKLOADS:
        tag = f"v{value}-{codec}"
        common = f"--dir {scratch} --records {records} --value-bytes {value} --compression {codec} {base}"
        variants = ["remote-cpu-rdma", "remote-gpu-relay", "remote-gpu-tcp", "gpu-host"]
        random.Random(f"r7-{tag}").shuffle(variants)
        if "rdma_address" in g:
            variants.insert(random.Random(f"r7-direct-{tag}").randrange(len(variants) + 1), "remote-gpu-rdma")
        if only:
            variants = [v for v in variants if v in only]
        for name in variants:
            target = out / "r7" / f"{tag}-{name}.jsonl"
            if target.exists() and target.stat().st_size > 0:
                continue
            started = []  # (host, pid, log, role)
            record = {"experiment": "r7", "workload": tag, "variant": name,
                      "load_before": {"client": cluster.load(c["host"]), "server": cluster.load(s["host"]),
                                      "gpu": cluster.load(g["host"])}}
            try:
                def server(machine: dict, build: str, listen: str, backend: str, role: str, env: str) -> str:
                    log = f"/tmp/lsm-r7-{role}.log"
                    pid, endpoint = cluster.start(machine["host"], f"{env} {pin(machine)} {build}/lsm_compaction_server "
                                                                   f"--listen {listen} --backend {backend}", log)
                    started.append((machine["host"], pid, log, role))
                    return endpoint

                host, modes, env, build = c["host"], "native_cpu,remote_merge", "", cluster.build
                if name == "remote-cpu-rdma":
                    endpoint = server(s, cluster.build, f"rdma://{s['address']}:0", "cpu_merge", "server", GPU_ENV)
                    env = f"LSM_RDMA_PUSH=1 LSM_REMOTE_ENDPOINT={endpoint}"
                elif name == "remote-gpu-relay":
                    gpu = server(g, gpu_build, f"tcp://{g['address']}:0", "cuda_merge", "gpu", GPU_SERVER_ENV)
                    relay = server(s, cluster.build, f"rdma://{s['address']}:0", "remote_merge", "relay",
                                   f"LSM_REMOTE_ENDPOINT={gpu}")
                    env = f"LSM_RDMA_PUSH=1 LSM_REMOTE_ENDPOINT={relay}"
                elif name == "remote-gpu-tcp":
                    endpoint = server(g, gpu_build, f"tcp://{g['address']}:0", "cuda_merge", "gpu", GPU_SERVER_ENV)
                    env = f"LSM_REMOTE_ENDPOINT={endpoint}"
                elif name == "remote-gpu-rdma":
                    endpoint = server(g, gpu_build, f"rdma://{g['rdma_address']}:0", "cuda_merge", "gpu",
                                      GPU_SERVER_ENV)
                    env = f"LSM_RDMA_PUSH=1 LSM_REMOTE_ENDPOINT={endpoint}"
                else:
                    # On the GPU host: the database on db_cores/db_node, the loopback server on the GPU's node.
                    endpoint = server(g, gpu_build, "tcp://127.0.0.1:0", "cuda_merge", "gpu", GPU_SERVER_ENV)
                    host, build, modes = g["host"], gpu_build, "native_cpu,cuda_merge,remote_merge"
                    env = f"{GPU_SERVER_ENV} LSM_REMOTE_ENDPOINT={endpoint}"
                placement = (f"numactl --physcpubind={g['db_cores']} --membind={g['db_node']}"
                             if name == "gpu-host" else pin(c))
                before = {role: cluster.cpu_seconds(h, pid) for h, pid, _, role in started}
                text = cluster.run(host, f"rm -rf {scratch} && {GPU_ENV} {env} {placement} "
                                         f"{build}/lsm_compaction_bench --modes {modes} {common}")
                after = {role: cluster.cpu_seconds(h, pid) for h, pid, _, role in started}
                cluster.run(host, f"rm -rf {scratch}", check=False)
                record["server_cpu_seconds"] = {role: (after[role] - before[role]
                                                       if None not in (after[role], before[role]) else None)
                                                for role in before}
                record["rows"] = [json.loads(line) for line in text.splitlines() if line.startswith("{")]
            finally:
                for h, pid, log, role in reversed(started):
                    cluster.stop(h, pid)
                    record.setdefault("server_statistics", {})[role] = cluster.statistics(h, log)
            target.write_text(json.dumps(record) + "\n")
            matches = all(row.get("matches_reference") for row in record["rows"])
            print(f"r7 {tag} {name} done, matches native: {matches}", flush=True)


def experiment_r8(cluster: Cluster, out: Path, repetitions: int, records: int, seconds: tuple[int, int],
                  only: list[str] | None = None, subcompactions: int | None = None) -> None:
    c, s, g = cluster.config["client"], cluster.config["server"], cluster.config["gpu"]
    gpu_build = g.get("build", cluster.build)
    scratch = cluster.config["scratch"]
    golden = f"{scratch}/rdma-golden-{records}"
    if "done" not in cluster.run(c["host"], f"test -f {golden}.done && echo done || true"):
        cluster.run(c["host"], f"rm -rf {golden} && {cluster.build}/lsm_system_bench --phase preload --dir {golden} "
                               f"--records {records} --threads 8 > {golden}.json 2>&1 && touch {golden}.done")
    results = out / "r8-database.jsonl"
    done = {(r["repetition"], r["design"]) for r in map(json.loads, results.open())} if results.exists() else set()
    for repetition in range(1, repetitions + 1):
        order = ["native", "remote_cpu_rdma", "remote_gpu_relay"]
        random.Random(800 + repetition).shuffle(order)
        if "rdma_address" in g:
            order.insert(random.Random(f"r8-direct-{repetition}").randrange(len(order) + 1), "remote_gpu_rdma")
        if only:
            order = [d for d in order if d in only]
        for design in order:
            if (repetition, design) in done:
                continue
            started = []
            record = {"experiment": "r8", "repetition": repetition, "design": design, "subcompactions": subcompactions,
                      "load_before": {"client": cluster.load(c["host"]), "server": cluster.load(s["host"]),
                                      "gpu": cluster.load(g["host"])}}
            try:
                env, mode = "", "native_cpu"
                if design == "remote_cpu_rdma":
                    pid, endpoint = cluster.start(s["host"], f"{GPU_ENV} {pin(s)} {cluster.build}/lsm_compaction_server "
                                                             f"--listen rdma://{s['address']}:0 --backend cpu_merge",
                                                  "/tmp/lsm-r8-server.log")
                    started.append((s["host"], pid, "/tmp/lsm-r8-server.log", "server"))
                    env, mode = f"LSM_RDMA_PUSH=1 LSM_REMOTE_ENDPOINT={endpoint}", "remote_merge"
                elif design == "remote_gpu_relay":
                    pid, gpu = cluster.start(g["host"], f"{GPU_SERVER_ENV} {pin(g)} {gpu_build}/lsm_compaction_server "
                                                        f"--listen tcp://{g['address']}:0 --backend cuda_merge",
                                             "/tmp/lsm-r8-gpu.log")
                    started.append((g["host"], pid, "/tmp/lsm-r8-gpu.log", "gpu"))
                    pid, relay = cluster.start(s["host"], f"LSM_REMOTE_ENDPOINT={gpu} {pin(s)} {cluster.build}/"
                                                          f"lsm_compaction_server --listen rdma://{s['address']}:0 "
                                                          f"--backend remote_merge", "/tmp/lsm-r8-relay.log")
                    started.append((s["host"], pid, "/tmp/lsm-r8-relay.log", "relay"))
                    env, mode = f"LSM_RDMA_PUSH=1 LSM_REMOTE_ENDPOINT={relay}", "remote_merge"
                elif design == "remote_gpu_rdma":
                    pid, endpoint = cluster.start(g["host"], f"{GPU_SERVER_ENV} {pin(g)} {gpu_build}/lsm_compaction_server "
                                                             f"--listen rdma://{g['rdma_address']}:0 --backend cuda_merge",
                                                  "/tmp/lsm-r8-gpu.log")
                    started.append((g["host"], pid, "/tmp/lsm-r8-gpu.log", "gpu"))
                    env, mode = f"LSM_RDMA_PUSH=1 LSM_REMOTE_ENDPOINT={endpoint}", "remote_merge"
                primary = f"{scratch}/rdma-r8"
                tuning = f"--subcompactions {subcompactions} " if subcompactions else ""
                before = {role: cluster.cpu_seconds(h, pid) for h, pid, _, role in started}
                text = cluster.run(c["host"], f"rm -rf {primary} && cp -a {golden} {primary} && {GPU_ENV} {env} {pin(c)} "
                                              f"{cluster.build}/lsm_system_bench --phase run --mode {mode} --dir {primary} "
                                              f"--records {records} --threads 4 --background-jobs 4 --batch-size 100 "
                                              f"{tuning}--disable-wal true --warmup-seconds {seconds[0]} "
                                              f"--duration-seconds {seconds[1]}; rc=$?; rm -rf {primary}; exit $rc")
                after = {role: cluster.cpu_seconds(h, pid) for h, pid, _, role in started}
                record["server_cpu_seconds"] = {role: (after[role] - before[role]
                                                       if None not in (after[role], before[role]) else None)
                                                for role in before}
                result = json.loads(text[text.index("{"):])
                result.pop("timeline", None)
                record["result"] = result
            finally:
                for h, pid, log, role in reversed(started):
                    cluster.stop(h, pid)
                    record.setdefault("server_statistics", {})[role] = cluster.statistics(h, log)
            append(results, record)
            print(f"r8 repetition {repetition} {design}: {record['result']['throughput']['ops_per_second']:.0f} ops/s, "
                  f"verified {record['result'].get('verification', {}).get('keys')} keys", flush=True)


# r9: tuned 4-core settings (configs/system/v3-headline.json) for every database.
R9_FLAGS = {
    "native": {"threads": 4, "subcompactions": 4, "pipelined-write": False, "adaptive-yield": True,
               "background-jobs": 4, "batch-size": 100, "disable-wal": True, "allow-fallocate": False},
    "service": {"threads": 8, "subcompactions": 4, "pipelined-write": False, "adaptive-yield": False,
                "background-jobs": 4, "batch-size": 100, "disable-wal": True, "allow-fallocate": False},
}


def experiment_r9(cluster: Cluster, out: Path, repetitions: int, records: int, seconds: tuple[int, int],
                  counts: list[int], engines: int, policies: list[str]) -> None:
    """Several databases on the client machine sharing one remote GPU compaction server over RDMA
    (PREREGISTRATION.md, GPU multiplexing, M2). The client's "db_slots" lists [cores, node] for
    each database in order; the server is restarted for every trial."""
    c, g = cluster.config["client"], cluster.config["gpu"]
    gpu_build = g.get("build", cluster.build)
    scratch = cluster.config["scratch"]
    golden = f"{scratch}/rdma-golden-{records}"
    if "done" not in cluster.run(c["host"], f"test -f {golden}.done && echo done || true"):
        cluster.run(c["host"], f"rm -rf {golden} && {cluster.build}/lsm_system_bench --phase preload --dir {golden} "
                               f"--records {records} --threads 8 > {golden}.json 2>&1 && touch {golden}.done")
    results = out / "r9-sharing.jsonl"
    done = {(r["repetition"], r["design"], r["instances"]) for r in map(json.loads, results.open())} \
        if results.exists() else set()
    for repetition in range(1, repetitions + 1):
        order = [(f"service-{policy}", n) for n in counts for policy in policies]
        order += [("native", n) for n in sorted({counts[0], counts[-1]})]
        random.Random(900 + repetition).shuffle(order)
        for design, n in order:
            if (repetition, design, n) in done:
                continue
            record = {"experiment": "r9", "repetition": repetition, "design": design, "instances": n,
                      "engines": engines if design != "native" else None,
                      "load_before": {"client": cluster.load(c["host"]), "gpu": cluster.load(g["host"])}}
            server = None
            log = "/tmp/lsm-r9-server.log"
            try:
                env, mode, flags = "", "native_cpu", R9_FLAGS["native"]
                if design != "native":
                    policy = design.split("-", 1)[1]
                    server = cluster.start(g["host"], f"{GPU_SERVER_ENV} LSM_SCHEDULER_LOG=1 {pin(g)} {gpu_build}/"
                                                      f"lsm_compaction_server --listen rdma://{g['rdma_address']}:0 "
                                                      f"--backend cuda_merge --engines {engines} --scheduler {policy}", log)
                    env = f"LSM_RDMA_PUSH=1 LSM_REMOTE_ENDPOINT={server[1]} LSM_REMOTE_CONNECTIONS=4"
                    mode, flags = "remote_merge", R9_FLAGS["service"]
                    record["server_cpu_before"] = cluster.cpu_seconds(g["host"], server[0])
                options = " ".join(f"--{k} {str(v).lower() if isinstance(v, bool) else v}" for k, v in flags.items())
                lines = [f"set -u; D={scratch}/r9; rm -rf $D; mkdir -p $D"]
                for index in range(n):
                    cores, node = c["db_slots"][index]
                    lines.append(f"cp -a {golden} $D/db{index}")
                for index in range(n):
                    cores, node = c["db_slots"][index]
                    lines.append(f"(LSM_TENANT=db{index} {env} numactl --physcpubind={cores} --preferred={node} "
                                 f"{cluster.build}/lsm_system_bench --phase run --mode {mode} --dir $D/db{index} "
                                 f"--records {records} --warmup-seconds {seconds[0]} --duration-seconds {seconds[1]} "
                                 f"{options} > $D/db{index}.json 2> $D/db{index}.err; echo $? > $D/db{index}.rc) &")
                lines.append("wait")
                lines.append("for f in $D/db*.rc; do echo rc $(basename $f .rc) $(cat $f); done")
                lines.append("for f in $D/db*.json; do printf 'json %s %s\\n' \"$(basename $f .json)\" \"$(tr -d '\\n' < $f)\"; done")
                lines.append("for f in $D/db*.err; do printf 'err %s %s\\n' \"$(basename $f .err)\" \"$(tail -c 300 $f | tr '\\n' ' ')\"; done")
                lines.append("rm -rf $D")
                text = cluster.run(c["host"], "bash -c " + shlex.quote("\n".join(lines)))
                if server:
                    after = cluster.cpu_seconds(g["host"], server[0])
                    before = record.pop("server_cpu_before")
                    record["server_cpu_seconds"] = after - before if None not in (after, before) else None
                record["results"], record["exit_codes"], record["errors"] = {}, {}, {}
                for line in text.splitlines():
                    kind, _, rest = line.partition(" ")
                    name, _, value = rest.partition(" ")
                    if kind == "rc":
                        record["exit_codes"][name] = int(value or -1)
                    elif kind == "json" and value.startswith("{"):
                        result = json.loads(value)
                        result.pop("timeline", None)
                        record["results"][name] = result
                    elif kind == "err" and value.strip():
                        record["errors"][name] = value
            finally:
                if server:
                    cluster.stop(g["host"], server[0])
                    record["server_statistics"] = cluster.statistics(g["host"], log)
            append(results, record)
            ops = [r["throughput"]["ops_per_second"] for r in record["results"].values()]
            print(f"r9 repetition {repetition} {design} N={n}: per-database "
                  f"{[round(o / 1e3) for o in ops]} k ops/s, exits {record['exit_codes']}", flush=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--hosts", type=Path, required=True)
    parser.add_argument("--experiment", choices=["r1", "r2", "r6", "r7", "r8", "r9"], required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--repetitions", type=int, default=None)
    parser.add_argument("--records", type=int, default=50_000_000)
    parser.add_argument("--warmup-seconds", type=int, default=30)
    parser.add_argument("--duration-seconds", type=int, default=120)
    parser.add_argument("--variants", help="r7 and r8: comma-separated subset of variants or designs to run")
    parser.add_argument("--subcompactions", type=int, help="r8: max_subcompactions for every design")
    parser.add_argument("--counts", default="1,2,4,8", help="r9: databases sharing the GPU")
    parser.add_argument("--engines", type=int, default=4, help="r9: engines of the compaction server")
    parser.add_argument("--policies", default="fifo,fair", help="r9: scheduler policies")
    arguments = parser.parse_args()
    cluster = Cluster(json.loads(arguments.hosts.read_text()))
    arguments.out.mkdir(parents=True, exist_ok=True)
    if arguments.experiment == "r1":
        experiment_r1(cluster, arguments.out, arguments.repetitions or 3)
    elif arguments.experiment == "r2":
        experiment_r2(cluster, arguments.out)
    elif arguments.experiment == "r7":
        experiment_r7(cluster, arguments.out, arguments.variants.split(",") if arguments.variants else None)
    elif arguments.experiment == "r9":
        experiment_r9(cluster, arguments.out, arguments.repetitions or 3, arguments.records,
                      (arguments.warmup_seconds, arguments.duration_seconds),
                      [int(n) for n in arguments.counts.split(",")], arguments.engines, arguments.policies.split(","))
    elif arguments.experiment == "r8":
        experiment_r8(cluster, arguments.out, arguments.repetitions or 3, arguments.records,
                      (arguments.warmup_seconds, arguments.duration_seconds),
                      arguments.variants.split(",") if arguments.variants else None, arguments.subcompactions)
    else:
        experiment_r6(cluster, arguments.out, arguments.repetitions or 5, arguments.records,
                      (arguments.warmup_seconds, arguments.duration_seconds))
    return 0


if __name__ == "__main__":
    sys.exit(main())
