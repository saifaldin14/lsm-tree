#!/usr/bin/env python3
"""Multi-host experiments for remote compaction and replication
(PREREGISTRATION.md, "Remote compaction and replication over RDMA").

  python3 results/rdma/rdma_campaign.py --hosts HOSTS.json --experiment r1 --out OUT_DIR

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
  r9  several databases on the client sharing one GPU compaction server (M2, M5)
  r10 failure of the shared service (E4): N databases use the GPU service; in "service-kill"
      trials the server is killed with SIGKILL partway through the measurement and restarted
      on the same endpoint later; per-second timelines are kept
  r11 CPU pool against the GPU (E5): N databases use the server on the GPU host with either
      its GPU or a CPU pool (cpu_merge); RDMA or TCP transport, and an optional separate CPU host
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
# value bytes, records, compression, delete fraction (as results/compaction_sweep.sh)
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

    def busy_threads(self, host: str, seconds: int = 5) -> float | None:
        """Hardware threads busy on the host over a few seconds, from /proc/stat (all users)."""
        out = self.run(host, f"head -1 /proc/stat; sleep {seconds}; head -1 /proc/stat; nproc", check=False).split("\n")
        try:
            first, second = ([int(v) for v in line.split()[1:]] for line in out[:2])
            threads = int(out[2])
        except (ValueError, IndexError):
            return None
        total = sum(second) - sum(first)
        idle = (second[3] + second[4]) - (first[3] + first[4])
        return threads * (total - idle) / total if total > 0 else None

    def wait_for_free_threads(self, host: str, needed: int, limit_seconds: int = 6 * 3600) -> dict:
        """Waits until at most nproc - needed threads are busy (nothing of ours runs between trials),
        polling every minute for at most limit_seconds; returns what was seen."""
        threads = int(self.run(host, "nproc").strip())
        waited = 0
        while True:
            busy = self.busy_threads(host)
            if busy is not None and busy <= threads - needed:
                return {"busy_threads": busy, "waited_seconds": waited, "met": True}
            if waited >= limit_seconds:
                return {"busy_threads": busy, "waited_seconds": waited, "met": False}
            time.sleep(60)
            waited += 60

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

    def start_gpu_sampler(self, host: str, log: str) -> int:
        """Samples the GPU's utilisation and memory about once a second, each row prefixed with the
        host's Unix time so it can be aligned with the databases' measurement windows; returns its PID."""
        loop = ("while true; do printf '%s, ' \"$(date +%s.%N)\"; "
                "nvidia-smi --query-gpu=utilization.gpu,memory.used,memory.total --format=csv,noheader,nounits; "
                "sleep 1; done")
        wrapped = f"nohup setsid bash -c {shlex.quote(loop)} > {log} 2>&1 < /dev/null & echo $!"
        return int(self.run(host, wrapped).strip().splitlines()[-1])

    def gpu_samples(self, host: str, log: str) -> dict | None:
        """The stopped sampler's rows as [unix time, utilisation %, memory used MiB], plus the GPU's
        total memory (MiB)."""
        rows, total = [], None
        for line in self.run(host, f"cat {log}", check=False).splitlines():
            fields = [field.strip() for field in line.split(",")]
            try:
                stamp, utilisation, used, total = float(fields[0]), int(fields[1]), int(fields[2]), int(fields[3])
            except (ValueError, IndexError):
                continue
            rows.append([stamp, utilisation, used])
        return {"samples": rows, "memory_total_mib": total} if rows else None


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


def database_copy_lines(scratch: str, golden: str, n: int) -> list[str]:
    """Shell lines that make N fresh copies of the golden database on the client."""
    golden_current = shlex.quote(f"{golden}/CURRENT")
    missing_golden = shlex.quote(f"golden database missing CURRENT: {golden}")
    lines = [
        f"set -u; D={shlex.quote(f'{scratch}/r9')}; "
        f"if ! test -f {golden_current}; then echo {missing_golden} >&2; exit 1; fi; "
        'rm -rf "$D" && mkdir -p "$D" || exit 1'
    ]
    for i in range(n):
        destination = f"$D/db{i}"
        lines.append(f"cp -a {shlex.quote(golden)} \"{destination}\" || exit 1; "
                     f"if ! test -f \"{destination}/CURRENT\"; then "
                     f"echo 'copied database missing CURRENT: db{i}' >&2; exit 1; fi")
    return lines


def database_run_lines(cluster: Cluster, scratch: str, n: int, env: str, mode: str, options: str, records: int,
                       seconds: tuple[int, int]) -> list[str]:
    """Shell lines that run N databases at once on the client's db_slots, wait for them, print each one's
    exit code, JSON result and last error output, and delete the copies."""
    slots = cluster.config["client"]["db_slots"]
    lines = [f"set -u; D={shlex.quote(f'{scratch}/r9')}"]
    for index in range(n):
        cores, node = slots[index]
        lines.append(f"(LSM_TENANT=db{index} {env} numactl --physcpubind={cores} --preferred={node} "
                     f"{cluster.build}/lsm_system_bench --phase run --mode {mode} --dir $D/db{index} "
                     f"--records {records} --warmup-seconds {seconds[0]} --duration-seconds {seconds[1]} "
                     f"{options} > $D/db{index}.json 2> $D/db{index}.err; echo $? > $D/db{index}.rc) &")
    lines.append("wait")
    lines.append("for f in $D/db*.rc; do echo rc $(basename $f .rc) $(cat $f); done")
    lines.append("for f in $D/db*.json; do printf 'json %s %s\\n' \"$(basename $f .json)\" \"$(tr -d '\\n' < $f)\"; done")
    lines.append("for f in $D/db*.err; do printf 'err %s %s\\n' \"$(basename $f .err)\" \"$(tail -c 300 $f | tr '\\n' ' ')\"; done")
    lines.append('rm -rf "$D"')
    return lines


def parse_database_output(text: str, record: dict, keep_timeline: bool = False) -> None:
    record["results"], record["exit_codes"], record["errors"] = {}, {}, {}
    for line in text.splitlines():
        kind, _, rest = line.partition(" ")
        name, _, value = rest.partition(" ")
        if kind == "rc":
            record["exit_codes"][name] = int(value or -1)
        elif kind == "json" and value.startswith("{"):
            result = json.loads(value)
            if not keep_timeline:
                result.pop("timeline", None)
            record["results"][name] = result
        elif kind == "err" and value.strip():
            record["errors"][name] = value


def experiment_r9(cluster: Cluster, out: Path, repetitions: int, records: int, seconds: tuple[int, int],
                  counts: list[int], engines: int, policies: list[str], seed_base: int = 900,
                  gpu_telemetry: bool = False, results_name: str = "r9-sharing.jsonl",
                  native_counts: list[int] | None = None, server_env: str = "", client_env: str = "") -> None:
    """Several databases on the client machine sharing one remote GPU compaction server over RDMA
    (PREREGISTRATION.md, GPU multiplexing, M2; capacity curve, M5). The client's "db_slots" lists
    [cores, node] for each database in order; the server is restarted for every trial. With
    gpu_telemetry, nvidia-smi samples the GPU host once a second during every service trial."""
    c, g = cluster.config["client"], cluster.config["gpu"]
    gpu_build = g.get("build", cluster.build)
    scratch = cluster.config["scratch"]
    golden = f"{scratch}/rdma-golden-{records}"
    if "done" not in cluster.run(c["host"], f"test -f {golden}.done && echo done || true"):
        cluster.run(c["host"], f"mkdir -p {scratch} && rm -rf {golden} && {cluster.build}/lsm_system_bench --phase preload "
                               f"--dir {golden} --records {records} --threads 8 > {golden}.json 2>&1 && touch {golden}.done")
    results = out / results_name
    done = {(r["repetition"], r["design"], r["instances"]) for r in map(json.loads, results.open())} \
        if results.exists() else set()
    for repetition in range(1, repetitions + 1):
        order = [(f"service-{policy}", n) for n in counts for policy in policies]
        order += [("native", n) for n in (native_counts or sorted({counts[0], counts[-1]}))]
        random.Random(seed_base + repetition).shuffle(order)
        for design, n in order:
            if (repetition, design, n) in done:
                continue
            record = {"experiment": "r9", "repetition": repetition, "design": design, "instances": n,
                      "engines": engines if design != "native" else None,
                      "server_env": server_env if design != "native" else None,
                      "client_env": client_env if design != "native" else None,
                      "load_before": {"client": cluster.load(c["host"]), "gpu": cluster.load(g["host"])}}
            server = None
            sampler = None
            log = "/tmp/lsm-r9-server.log"
            sampler_log = "/tmp/lsm-r9-gpu.csv"
            try:
                env, mode, flags = "", "native_cpu", R9_FLAGS["native"]
                if design != "native":
                    policy = design.split("-", 1)[1]
                    server = cluster.start(g["host"], f"{GPU_SERVER_ENV} {server_env} LSM_SCHEDULER_LOG=1 {pin(g)} {gpu_build}/"
                                                      f"lsm_compaction_server --listen rdma://{g['rdma_address']}:0 "
                                                      f"--backend cuda_merge --engines {engines} --scheduler {policy}", log)
                    env = f"LSM_RDMA_PUSH=1 LSM_REMOTE_ENDPOINT={server[1]} LSM_REMOTE_CONNECTIONS=4 {client_env}"
                    mode, flags = "remote_merge", R9_FLAGS["service"]
                    record["server_cpu_before"] = cluster.cpu_seconds(g["host"], server[0])
                options = " ".join(f"--{k} {str(v).lower() if isinstance(v, bool) else v}" for k, v in flags.items())
                copy = database_copy_lines(scratch, golden, n)
                lines = database_run_lines(cluster, scratch, n, env, mode, options, records, seconds)
                if gpu_telemetry and server:
                    # Copy first, so the GPU samples cover only the databases' run.
                    cluster.run(c["host"], "bash -c " + shlex.quote("\n".join(copy)))
                    sampler = cluster.start_gpu_sampler(g["host"], sampler_log)
                else:
                    lines = copy + lines[1:]
                text = cluster.run(c["host"], "bash -c " + shlex.quote("\n".join(lines)))
                if server:
                    after = cluster.cpu_seconds(g["host"], server[0])
                    before = record.pop("server_cpu_before")
                    record["server_cpu_seconds"] = after - before if None not in (after, before) else None
                parse_database_output(text, record)
            finally:
                if sampler:
                    cluster.stop(g["host"], sampler)
                    record["gpu_telemetry"] = cluster.gpu_samples(g["host"], sampler_log)
                if server:
                    cluster.stop(g["host"], server[0])
                    record["server_statistics"] = cluster.statistics(g["host"], log)
            append(results, record)
            ops = [r["throughput"]["ops_per_second"] for r in record["results"].values()]
            print(f"r9 repetition {repetition} {design} N={n}: per-database "
                  f"{[round(o / 1e3) for o in ops]} k ops/s, exits {record['exit_codes']}", flush=True)


def ensure_golden(cluster: Cluster, records: int) -> str:
    c, scratch = cluster.config["client"], cluster.config["scratch"]
    golden_scratch = cluster.config.get("golden_scratch", scratch)
    golden = f"{golden_scratch}/rdma-golden-{records}"
    valid_golden = (f"test -f {shlex.quote(f'{golden}.done')} && "
                    f"test -f {shlex.quote(f'{golden}/CURRENT')} && echo done || true")
    if "done" not in cluster.run(c["host"], valid_golden):
        cluster.run(c["host"], f"mkdir -p {shlex.quote(golden_scratch)} && "
                               f"rm -rf {shlex.quote(golden)} {shlex.quote(f'{golden}.done')} && "
                               f"{shlex.quote(f'{cluster.build}/lsm_system_bench')} --phase preload "
                               f"--dir {shlex.quote(golden)} --records {records} --threads 8 "
                               f"> {shlex.quote(f'{golden}.json')} 2>&1 && "
                               f"test -f {shlex.quote(f'{golden}/CURRENT')} && "
                               f"touch {shlex.quote(f'{golden}.done')}")
    return golden


def server_threads(cores: str) -> int:
    """Hardware threads in a cpu list such as "0-3,10-13"."""
    count = 0
    for part in cores.split(","):
        low, _, high = part.partition("-")
        count += int(high or low) - int(low) + 1
    return count


def experiment_r10(cluster: Cluster, out: Path, repetitions: int, records: int, seconds: tuple[int, int], n: int,
                   engines: int, kill_at: int, outage: int, seed_base: int = 1300, port: int = 7471) -> None:
    """E4: N databases share the GPU service (M5 settings); in "service-kill" trials the server is killed
    with SIGKILL kill_at seconds into the measurement window and restarted on the same endpoint outage
    seconds later. "service" (no failure) and "native" (RocksDB) run in the same blocks. Every database
    keeps its per-second timeline; the kill and restart times are recorded as Unix times."""
    c, g = cluster.config["client"], cluster.config["gpu"]
    gpu_build = g.get("build", cluster.build)
    scratch = cluster.config["scratch"]
    golden = ensure_golden(cluster, records)
    results = out / "r10-failure.jsonl"
    done = {(r["repetition"], r["design"]) for r in map(json.loads, results.open())} if results.exists() else set()
    log, restart_log = "/tmp/lsm-r10-server.log", "/tmp/lsm-r10-restart.log"
    command = (f"{GPU_SERVER_ENV} LSM_SCHEDULER_LOG=1 {pin(g)} {gpu_build}/lsm_compaction_server "
               f"--listen rdma://{g['rdma_address']}:{port} --backend cuda_merge --engines {engines} --scheduler fair")
    for repetition in range(1, repetitions + 1):
        order = ["service", "service-kill", "native"]
        random.Random(seed_base + repetition).shuffle(order)
        for design in order:
            if (repetition, design) in done:
                continue
            record = {"experiment": "r10", "repetition": repetition, "design": design, "instances": n,
                      "engines": engines if design != "native" else None,
                      "kill_at_measurement_second": kill_at if design == "service-kill" else None,
                      "outage_seconds": outage if design == "service-kill" else None,
                      "load_before": {"client": cluster.load(c["host"]), "gpu": cluster.load(g["host"])}}
            server, killer = None, None
            try:
                env, mode, flags = "", "native_cpu", R9_FLAGS["native"]
                if design != "native":
                    server = cluster.start(g["host"], command, log)
                    env = f"LSM_RDMA_PUSH=1 LSM_REMOTE_ENDPOINT={server[1]} LSM_REMOTE_CONNECTIONS=4"
                    mode, flags = "remote_merge", R9_FLAGS["service"]
                    record["endpoint"] = server[1]
                options = " ".join(f"--{k} {str(v).lower() if isinstance(v, bool) else v}" for k, v in flags.items())
                cluster.run(c["host"], "bash -c " + shlex.quote("\n".join(database_copy_lines(scratch, golden, n))))
                if design == "service-kill":
                    # Started just before the databases; the restarted server replaces this shell (exec), so
                    # stopping the killer's PID stops the restarted server.
                    script = (f"sleep {seconds[0] + kill_at}; echo killed $(date +%s.%N); kill -9 {server[0]}; "
                              f"sleep {outage}; echo restarted $(date +%s.%N); exec env {command}")
                    wrapped = f"nohup setsid bash -c {shlex.quote(script)} > {restart_log} 2>&1 < /dev/null & echo $!"
                    killer = int(cluster.run(g["host"], wrapped).strip().splitlines()[-1])
                record["databases_started_unix"] = time.time()
                text = cluster.run(c["host"], "bash -c " + shlex.quote(
                    "\n".join(database_run_lines(cluster, scratch, n, env, mode, options, records, seconds))))
                parse_database_output(text, record, keep_timeline=True)
            finally:
                if killer:
                    cluster.stop(g["host"], killer)
                    events = {}
                    for line in cluster.run(g["host"], f"cat {restart_log}", check=False).splitlines():
                        word, _, stamp = line.partition(" ")
                        if word in ("killed", "restarted"):
                            events[f"{word}_unix"] = float(stamp)
                        elif word == "listening":
                            events["restart_endpoint"] = stamp.split()[0]
                    record["failure"] = events
                    record["server_statistics"] = cluster.statistics(g["host"], restart_log)
                if server:
                    cluster.stop(g["host"], server[0])
                    if not killer:
                        record["server_statistics"] = cluster.statistics(g["host"], log)
            append(results, record)
            ops = [r["throughput"]["ops_per_second"] for r in record["results"].values()]
            print(f"r10 repetition {repetition} {design} N={n}: per-database {[round(o / 1e3) for o in ops]} k ops/s, "
                  f"exits {record['exit_codes']}, failure {record.get('failure')}", flush=True)


def experiment_r11(cluster: Cluster, out: Path, repetitions: int, records: int, seconds: tuple[int, int], n: int,
                   engines: int, pools: list[str], seed_base: int = 1400, transport: str = "rdma",
                   cpu_pool_host: dict | None = None) -> None:
    """Compare native RocksDB, a shared GPU, and CPU worker pools for N databases.

    CPU workers may run on a separate host. The request transport can be RDMA
    or TCP; CPU-pool trials wait for their selected worker cores to be free.
    """
    c, g = cluster.config["client"], cluster.config["gpu"]
    cpu_pool_host = cpu_pool_host or g
    if transport not in ("rdma", "tcp"):
        raise ValueError(f"unsupported r11 transport: {transport}")
    scratch = cluster.config["scratch"]
    golden = ensure_golden(cluster, records)
    results = out / "r11-cpu-pool.jsonl"
    done = {(r["repetition"], r["design"]) for r in map(json.loads, results.open())} if results.exists() else set()
    log = f"/tmp/lsm-r11-{out.name}-server.log"
    for host in {g["host"], cpu_pool_host["host"]}:
        cluster.run(host, f"mkdir -p {scratch}")
    arms = {"service-gpu": ("cuda_merge", g["cores"], engines), "native": (None, None, None)}
    for cores in pools:
        arms[f"service-cpu-{server_threads(cores)}"] = ("cpu_merge", cores, server_threads(cores))
    for repetition in range(1, repetitions + 1):
        order = list(arms)
        random.Random(seed_base + repetition).shuffle(order)
        for design in order:
            if (repetition, design) in done:
                continue
            backend, cores, slots = arms[design]
            service_host = cpu_pool_host if backend == "cpu_merge" else g
            service_build = service_host.get("build", cluster.build)
            service_node = service_host["node"]
            record = {"experiment": "r11", "repetition": repetition, "design": design, "instances": n,
                      "client_host": c["host"].rsplit("@", 1)[-1].split(".", 1)[0],
                      "backend": backend, "server_cores": cores, "engines": slots,
                      "transport": transport if backend else None,
                      "service_host": (service_host["host"].rsplit("@", 1)[-1].split(".", 1)[0]
                                       if backend else None),
                      "load_before": {"client": cluster.load(c["host"]), "gpu": cluster.load(g["host"]),
                                      "cpu_pool": cluster.load(cpu_pool_host["host"])}}
            if backend == "cpu_merge":
                record["free_threads_wait"] = cluster.wait_for_free_threads(service_host["host"], slots)
            else:
                record["gpu_host_busy_threads"] = cluster.busy_threads(g["host"])
            server = None
            try:
                env, mode, flags = "", "native_cpu", R9_FLAGS["native"]
                if backend:
                    server_env = GPU_SERVER_ENV if backend == "cuda_merge" else GPU_ENV
                    address = service_host["rdma_address"] if transport == "rdma" else service_host["address"]
                    server = cluster.start(service_host["host"], f"{server_env} LSM_SCHEDULER_LOG=1 "
                                                      f"numactl --physcpubind={cores} --membind={service_node} "
                                                      f"{service_build}/lsm_compaction_server "
                                                      f"--listen {transport}://{address}:0 --backend {backend} "
                                                      f"--engines {slots} --scheduler fair", log)
                    push = "LSM_RDMA_PUSH=1 " if transport == "rdma" else ""
                    env = f"{push}LSM_REMOTE_ENDPOINT={server[1]} LSM_REMOTE_CONNECTIONS=4"
                    mode, flags = "remote_merge", R9_FLAGS["service"]
                    record["server_cpu_before"] = cluster.cpu_seconds(service_host["host"], server[0])
                options = " ".join(f"--{k} {str(v).lower() if isinstance(v, bool) else v}" for k, v in flags.items())
                lines = database_copy_lines(scratch, golden, n) + \
                    database_run_lines(cluster, scratch, n, env, mode, options, records, seconds)[1:]
                text = cluster.run(c["host"], "bash -c " + shlex.quote("\n".join(lines)))
                if server:
                    after, before = cluster.cpu_seconds(service_host["host"], server[0]), record.pop("server_cpu_before")
                    record["server_cpu_seconds"] = after - before if None not in (after, before) else None
                parse_database_output(text, record)
            finally:
                if server:
                    cluster.stop(service_host["host"], server[0])
                    record["server_statistics"] = cluster.statistics(service_host["host"], log)
            record["load_after"] = {"client": cluster.load(c["host"]), "gpu": cluster.load(g["host"]),
                                    "cpu_pool": cluster.load(cpu_pool_host["host"])}
            append(results, record)
            ops = [r["throughput"]["ops_per_second"] for r in record["results"].values()]
            print(f"r11 repetition {repetition} {design} N={n}: per-database {[round(o / 1e3) for o in ops]} k ops/s, "
                  f"exits {record['exit_codes']}", flush=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--hosts", type=Path, required=True)
    parser.add_argument("--experiment", choices=["r1", "r2", "r6", "r7", "r8", "r9", "r10", "r11"], required=True)
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
    parser.add_argument("--seed-base", type=int, default=900, help="r9: run-order seed base (seed = base + repetition)")
    parser.add_argument("--gpu-telemetry", action="store_true", help="r9: sample the GPU host with nvidia-smi")
    parser.add_argument("--results-name", default="r9-sharing.jsonl", help="r9: results file name in OUT_DIR")
    parser.add_argument("--native-counts", help="r9: database counts for RocksDB (default: smallest and largest)")
    parser.add_argument("--server-env", default="", help="r9: extra environment for the compaction server")
    parser.add_argument("--client-env", default="", help="r9: extra environment for the databases using the server")
    parser.add_argument("--instances", type=int, default=4, help="r10 and r11: databases sharing the server")
    parser.add_argument("--kill-at", type=int, default=60, help="r10: measurement second at which the server is killed")
    parser.add_argument("--outage", type=int, default=60, help="r10: seconds before the server is restarted")
    parser.add_argument("--pools", default="0-1,10-11;0-3,10-13;0-7,10-17",
                        help="r11: semicolon-separated CPU lists for the selected CPU-pool host")
    parser.add_argument("--transport", choices=["rdma", "tcp"], default="rdma", help="r11: worker transport")
    parser.add_argument("--cpu-host", help="r11: hosts-config key for the CPU-pool host (default: GPU host)")
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
                      [int(n) for n in arguments.counts.split(",")], arguments.engines, arguments.policies.split(","),
                      arguments.seed_base, arguments.gpu_telemetry, arguments.results_name,
                      [int(n) for n in arguments.native_counts.split(",")] if arguments.native_counts else None,
                      arguments.server_env, arguments.client_env)
    elif arguments.experiment == "r10":
        experiment_r10(cluster, arguments.out, arguments.repetitions or 3, arguments.records,
                       (arguments.warmup_seconds, arguments.duration_seconds), arguments.instances,
                       arguments.engines, arguments.kill_at, arguments.outage)
    elif arguments.experiment == "r11":
        cpu_pool_host = cluster.config.get(arguments.cpu_host) if arguments.cpu_host else None
        if arguments.cpu_host and cpu_pool_host is None:
            parser.error(f"--cpu-host {arguments.cpu_host!r} is not a key in the hosts configuration")
        experiment_r11(cluster, arguments.out, arguments.repetitions or 3, arguments.records,
                       (arguments.warmup_seconds, arguments.duration_seconds), arguments.instances,
                       arguments.engines, arguments.pools.split(";"), transport=arguments.transport,
                       cpu_pool_host=cpu_pool_host)
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
