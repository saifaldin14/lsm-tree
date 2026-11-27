#!/usr/bin/env python3
"""E2 (PREREGISTRATION.md): Kvrocks with its compactions sent to the GPU compaction service.

Runs on pyke. Kvrocks (our patched RocksDB 11.8.1 plus the hook in kvrocks-lsm-hook.diff) is pinned to
4 physical cores of node 0 with its data in /dev/shm; redis-benchmark runs on node 1. Arms, shuffled in
each block (seed 1500 + block):
  stock    Kvrocks' own settings (snappy, compaction filters, table format 5), local compaction;
  local    the settings the service needs (lz4, no compaction filters, table format 6), local compaction;
  service  as local, compactions sent over TCP to lsm_compaction_server (cuda_merge, 4 engines) on malphite.
Each trial copies a preloaded golden database (every key of the keyspace), warms up, measures SETs to
random keys, then GETs, then waits for leftover compaction and checks a random sample of keys.

Usage (on pyke): python3 results/e2/e2_campaign.py OUT [--blocks 3] [--records 50000000]
"""
import argparse
import json
import os
import random
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "rdma"))
from rdma_campaign import GPU_SERVER_ENV, Cluster  # noqa: E402

HOME = Path("/tmp/sa29ali/e2")
KVROCKS = HOME / "bin" / "kvrocks"
CONF = HOME / "kvrocks.conf"
SCRATCH = Path("/dev/shm/sa29ali/e2")
PORT = 16700
KVROCKS_CPUS = "0-3,48-51"
BENCH_CPUS = "24-35,72-83"
VALUE = 100
GPU = {"host": "sa29ali@192.168.100.45", "address": "192.168.100.45", "cores": "0-3", "node": 0,
       "repo": "/tmp/sa29ali/lsm-e", "build": "build/e-cuda"}
SERVER_PORT = 7481
ARMS = {
    "stock": ({}, {"rocksdb.compression": "snappy"}),
    "local": ({"LSM_KVROCKS_TABLE_FORMAT": "lsm", "LSM_KVROCKS_NO_COMPACTION_FILTERS": "1"}, {}),
    "service": ({"LSM_KVROCKS_TRANSFORM": "remote_merge", "LSM_KVROCKS_NO_COMPACTION_FILTERS": "1",
                 "LSM_REMOTE_ENDPOINT": f"tcp://{GPU['address']}:{SERVER_PORT}", "LSM_REMOTE_CONNECTIONS": "4"}, {}),
}


def resp(*parts: bytes) -> bytes:
    return b"*%d\r\n" % len(parts) + b"".join(b"$%d\r\n%s\r\n" % (len(p), p) for p in parts)


class Client:
    """Minimal RESP client: enough for INFO, PING, COMPACT, SHUTDOWN and pipelined SET/GET."""

    def __init__(self, port: int = PORT):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=600)
        self.buffer = b""

    def _fill(self) -> None:
        chunk = self.sock.recv(1 << 20)
        if not chunk:
            raise ConnectionError("closed")
        self.buffer += chunk

    def reply(self):
        while b"\r\n" not in self.buffer:
            self._fill()
        line, self.buffer = self.buffer.split(b"\r\n", 1)
        kind, rest = line[:1], line[1:]
        if kind == b"$":
            length = int(rest)
            if length < 0:
                return None
            while len(self.buffer) < length + 2:
                self._fill()
            data, self.buffer = self.buffer[:length], self.buffer[length + 2:]
            return data
        if kind == b"-":
            raise RuntimeError(rest.decode())
        return rest

    def call(self, *parts: str):
        self.sock.sendall(resp(*(p.encode() for p in parts)))
        return self.reply()

    def pipeline(self, commands: list[list[bytes]]) -> list:
        self.sock.sendall(b"".join(resp(*c) for c in commands))
        return [self.reply() for _ in commands]


def key(i: int) -> bytes:
    return b"key:%012d" % i  # redis-benchmark's __rand_int__ format


def info(section: str) -> dict:
    text = Client().call("INFO", section).decode()
    return dict(line.split(":", 1) for line in text.splitlines() if ":" in line and not line.startswith("#"))


def wait_ready(process: subprocess.Popen, seconds: int = 300) -> None:
    for _ in range(seconds * 5):
        if process.poll() is not None:
            raise RuntimeError(f"kvrocks exited with {process.returncode}")
        try:
            if Client().call("PING") == b"PONG":
                return
        except OSError:
            pass
        time.sleep(0.2)
    raise RuntimeError("kvrocks did not start")


def start_kvrocks(directory: Path, env: dict, overrides: dict, log: Path) -> subprocess.Popen:
    replaced = set(overrides) | {"dir"}
    lines = [line for line in CONF.read_text().splitlines() if line.split(" ", 1)[0] not in replaced]
    lines += [f"dir {directory}"] + [f"{k} {v}" for k, v in overrides.items()]
    conf = directory.parent / f"{directory.name}.conf"
    conf.write_text("\n".join(lines) + "\n")
    return subprocess.Popen(["numactl", f"--physcpubind={KVROCKS_CPUS}", "--membind=0", str(KVROCKS), "-c", str(conf)],
                            env={**os.environ, **env}, stdout=log.open("w"), stderr=subprocess.STDOUT)


def stop_kvrocks(process: subprocess.Popen) -> int:
    try:
        Client().call("SHUTDOWN")
    except (OSError, ConnectionError, RuntimeError):
        pass
    try:
        return process.wait(timeout=600)
    except subprocess.TimeoutExpired:
        process.kill()
        return process.wait()


def cpu_seconds(pid: int) -> float:
    fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")


def running_compactions() -> int | None:
    """Running plus pending compactions from INFO rocksdb, or None if Kvrocks does not report them."""
    fields = info("rocksdb")
    values = [int(v) for k, v in fields.items()
              if ("num_running_compaction" in k or "compaction_pending" in k) and v.strip().isdigit()]
    return sum(values) if values else None


def wait_compactions(limit: int) -> float | None:
    started = time.time()
    while time.time() - started < limit:
        running = running_compactions()
        if running is None:
            return None
        if running == 0:
            return time.time() - started
        time.sleep(1)
    return None


def preload(golden: Path, records: int) -> None:
    """Every key of the keyspace, in random order, through Kvrocks with the local arm's settings."""
    shutil.rmtree(golden, ignore_errors=True)
    golden.mkdir(parents=True)
    env, overrides = ARMS["local"]
    process = start_kvrocks(golden, env, overrides, golden.parent / "golden.log")
    wait_ready(process)
    client, value, order = Client(), b"x" * VALUE, list(range(records))
    random.Random(7).shuffle(order)
    for start in range(0, records, 10_000):
        client.pipeline([[b"SET", key(i), value] for i in order[start:start + 10_000]])
    client.call("COMPACT")
    wait_compactions(3600)
    if stop_kvrocks(process) != 0:
        raise RuntimeError("preload: kvrocks did not exit cleanly")
    (golden.parent / "golden.done").write_text(json.dumps({"records": records, "value_bytes": VALUE}))


def bench_chunk(test: str, records: int, requests: int) -> dict:
    command = ["numactl", f"--physcpubind={BENCH_CPUS}", "--membind=1", "redis-benchmark", "-h", "127.0.0.1",
               "-p", str(PORT), "-t", test, "-r", str(records), "-d", str(VALUE), "-c", "64", "-P", "16",
               "--threads", "8", "-n", str(requests), "--csv"]
    started = time.time()
    out = subprocess.run(command, capture_output=True, text=True, check=True).stdout
    ended = time.time()
    rows = [line.replace('"', "").split(",") for line in out.strip().splitlines()]
    row = dict(zip(rows[0], rows[-1]))
    number = lambda name: float(row[name]) if name in row else None  # noqa: E731
    return {"start": started, "end": ended, "requests": requests, "rps": number("rps"),
            "p50_ms": number("p50_latency_ms"), "p99_ms": number("p99_latency_ms"), "max_ms": number("max_latency_ms")}


def phase(process: subprocess.Popen, test: str, records: int, seconds: int, chunk: int) -> dict:
    """redis-benchmark runs of `chunk` requests until `seconds` have passed; counters read between runs."""
    def commands() -> int:
        return int(info("stats").get("total_commands_processed", "0"))
    first_cpu, first_ops, started = cpu_seconds(process.pid), commands(), time.time()
    chunks = []
    while time.time() - started < seconds:
        chunks.append(bench_chunk(test, records, chunk))
    elapsed, ops, cpu = time.time() - started, commands() - first_ops, cpu_seconds(process.pid) - first_cpu
    p99 = sorted(c["p99_ms"] for c in chunks if c["p99_ms"] is not None)
    return {"seconds": elapsed, "commands": ops, "ops_per_second": ops / elapsed, "kvrocks_cpu_seconds": cpu,
            "cpu_microseconds_per_op": cpu / ops * 1e6 if ops else None,
            "p99_ms_max": p99[-1] if p99 else None, "p99_ms_median": p99[len(p99) // 2] if p99 else None,
            "chunks": chunks}


def verify(records: int, sample: int, seed: int) -> dict:
    client, rng = Client(), random.Random(seed)
    keys = [rng.randrange(records) for _ in range(sample)]
    missing = wrong = 0
    for start in range(0, sample, 10_000):
        for value in client.pipeline([[b"GET", key(i)] for i in keys[start:start + 10_000]]):
            if value is None:
                missing += 1
            elif len(value) != VALUE:
                wrong += 1
    return {"sampled": sample, "missing": missing, "wrong_length": wrong}


def hook_statistics(log: Path) -> dict | None:
    """The hook's last JSON statistics line (printed when Kvrocks exits)."""
    for line in reversed(log.read_text(errors="replace").splitlines()):
        start = line.find("{")
        if start >= 0 and "merge" in line:
            try:
                return json.loads(line[start:])
            except json.JSONDecodeError:
                continue
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("out", type=Path)
    parser.add_argument("--blocks", type=int, default=3)
    parser.add_argument("--records", type=int, default=50_000_000)
    parser.add_argument("--warmup-seconds", type=int, default=60)
    parser.add_argument("--write-seconds", type=int, default=300)
    parser.add_argument("--read-seconds", type=int, default=60)
    parser.add_argument("--chunk", type=int, default=2_000_000, help="requests per redis-benchmark run")
    parser.add_argument("--sample", type=int, default=1_000_000, help="keys checked after each trial")
    parser.add_argument("--arms", default="stock,local,service")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    results = args.out / "e2-kvrocks.jsonl"
    done = {(r["block"], r["arm"]) for r in map(json.loads, results.open())} if results.exists() else set()

    SCRATCH.mkdir(parents=True, exist_ok=True)
    golden = SCRATCH / f"golden-{args.records}"
    if not (SCRATCH / "golden.done").exists():
        preload(golden, args.records)
    gpu = Cluster({"ssh": ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=20"], "repo": GPU["repo"],
                   "build": GPU["build"]})
    server_log = "/tmp/lsm-e2-server.log"
    server_command = (f"{GPU_SERVER_ENV} LSM_SCHEDULER_LOG=1 numactl --physcpubind={GPU['cores']} "
                      f"--membind={GPU['node']} {GPU['build']}/lsm_compaction_server --listen "
                      f"tcp://{GPU['address']}:{SERVER_PORT} --backend cuda_merge --engines 4 --scheduler fair")
    for block in range(args.blocks):
        order = args.arms.split(",")
        random.Random(1500 + block).shuffle(order)
        for arm in order:
            if (block, arm) in done:
                continue
            env, overrides = ARMS[arm]
            record = {"experiment": "e2", "block": block, "arm": arm, "env": env, "conf": overrides,
                      "records": args.records, "load_before": os.getloadavg()}
            database, log = SCRATCH / "db", args.out / f"kvrocks-{block}-{arm}.log"
            shutil.rmtree(database, ignore_errors=True)
            subprocess.run(["cp", "-a", str(golden), str(database)], check=True)
            server = process = None
            try:
                if arm == "service":
                    server = gpu.start(GPU["host"], server_command, server_log)
                    record["endpoint"] = server[1]
                process = start_kvrocks(database, env, overrides, log)
                wait_ready(process)
                phase(process, "set", args.records, args.warmup_seconds, args.chunk)
                record["write"] = phase(process, "set", args.records, args.write_seconds, args.chunk)
                record["rocksdb_after_write"] = info("rocksdb")
                record["read"] = phase(process, "get", args.records, args.read_seconds, args.chunk)
                record["leftover_compaction_seconds"] = wait_compactions(1800)
                record["verification"] = verify(args.records, args.sample, 1500 + block)
                record["exit_code"] = stop_kvrocks(process)
                record["hook_statistics"] = hook_statistics(log)
            finally:
                if process and process.poll() is None:
                    process.kill()
                    process.wait()
                if server:
                    gpu.stop(GPU["host"], server[0])
                    record["server_statistics"] = gpu.statistics(GPU["host"], server_log)
            with results.open("a") as out:
                out.write(json.dumps(record) + "\n")
            print(f"e2 block {block} {arm}: SET {record['write']['ops_per_second'] / 1e3:.0f}k/s, "
                  f"{record['write']['cpu_microseconds_per_op']:.1f} us CPU/op, "
                  f"GET {record['read']['ops_per_second'] / 1e3:.0f}k/s, verify {record['verification']}, "
                  f"exit {record['exit_code']}", flush=True)
            shutil.rmtree(database, ignore_errors=True)
    (args.out / "DONE").touch()
    return 0


if __name__ == "__main__":
    sys.exit(main())
