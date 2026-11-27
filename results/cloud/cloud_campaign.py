#!/usr/bin/env python3
"""Cloud experiments with the shared compaction service (PREREGISTRATION.md, "Cloud: the
compaction service on Kubernetes"; docs/cloud.md).

  python3 results/cloud/cloud_campaign.py --platform PLATFORM.json --experiment c1 --out OUT_DIR

PLATFORM.json selects how commands reach the database slots ("db-0", "db-1", ...) and the
service ("svc"); the commands themselves are identical on every platform:
  {"backend": "k8s", "namespace": "lsm", "image": "..."}   pods of infra/k8s (kubectl exec)
  {"backend": "vm", "ssh": [...], "targets": {...}}         containers on plain VMs (vms.sh writes it)
  {"backend": "local", "root": DIR, "bin": BUILD_DIR}       this machine (offline check of the driver)
Optional keys: "backend_name" (cpu_merge or cuda_merge; default cpu_merge), "engines",
"server_env", "client_env", "gpu" (sample the service's GPU with nvidia-smi).

Experiments (records in the r9 format of rdma_campaign.py, read by multiplex/analyze.py m2()):
  setup    k8s: render and apply infra/k8s (namespace, database slots, service pod) from the
           platform's "render" values and wait until every pod is ready
  preload  preload the golden database on every slot (once per platform)
  c1       N databases sharing one service against RocksDB (capacity, cloud TCP)
  c2       one heavy and three open-loop light tenants through a contended service, per policy,
           and each light tenant alone (isolation)
  c3       elastic: databases through the Kubernetes Service while the service Deployment is
           scaled out and then in (drain); k8s only
  c4       c1's comparison at one N, run on each platform (what Kubernetes costs)
Every run is resumable: finished trials (repetition, design, N) are skipped.
"""

from __future__ import annotations

import argparse
import json
import os
import random
import shlex
import subprocess
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "rdma"))
from rdma_campaign import R9_FLAGS  # noqa: E402  (the M5 settings)

GOLDEN = "/golden/db"
WORK = "/work/db"


class Platform:
    """Runs shell commands in a target (database slot or service) and reports its address."""

    def __init__(self, config: dict):
        self.config = config
        self.backend = config["backend"]
        if config.get("kubeconfig"):
            os.environ["KUBECONFIG"] = os.path.expanduser(config["kubeconfig"])

    def command(self, target: str, command: str) -> list[str]:
        if self.backend == "local":
            # Offline check of the driver: each target is a directory under "root", standing in
            # for the pod's /golden and /work; binaries from "bin".
            root = Path(self.config["root"]) / target
            for name in ("golden", "work", "tmp"):
                (root / name).mkdir(parents=True, exist_ok=True)
            command = (command.replace("/tmp/", "\0TMP").replace("/golden", f"{root}/golden")
                       .replace("/work", f"{root}/work").replace("\0TMP", f"{root}/tmp/")
                       .replace("nohup setsid ", "nohup "))  # macOS has no setsid
            return ["bash", "-c", f"export PATH={shlex.quote(self.config['bin'])}:$PATH; {command}"]
        if self.backend == "k8s":
            return ["kubectl", "-n", self.config.get("namespace", "lsm"), "exec", target, "--", "bash", "-c", command]
        t = self.config["targets"][target]
        return self.config["ssh"] + [t["host"], f"docker exec {target} bash -c {shlex.quote(command)}"]

    def execute(self, target: str, command: str, timeout: int | None = None) -> tuple[int, str, str]:
        # Retries only failures of the connection itself (kubectl's API stream, ssh exit 255).
        for attempt in range(6):
            result = subprocess.run(self.command(target, command), capture_output=True, text=True, timeout=timeout)
            transient = (result.returncode == 255 and self.backend == "vm") or (
                self.backend == "k8s" and result.returncode != 0 and any(
                    text in result.stderr for text in ("error dialing backend", "unable to upgrade connection",
                                                       "connection refused", "i/o timeout", "TLS handshake")))
            if not transient:
                return result.returncode, result.stdout, result.stderr
            time.sleep(min(60, 5 * 2 ** attempt))
        return result.returncode, result.stdout, result.stderr

    def run(self, target: str, command: str, check: bool = True, timeout: int | None = None) -> str:
        code, out, err = self.execute(target, command, timeout)
        if check and code != 0:
            raise RuntimeError(f"{target}: {command}\n{out[-2000:]}\n{err[-2000:]}")
        return out

    def ip(self, target: str) -> str:
        if self.backend == "local":
            return "127.0.0.1"
        if self.backend == "k8s":
            return kubectl(["get", "pod", target, "-o", "jsonpath={.status.podIP}"], self.config).strip()
        return self.config["targets"][target]["private"]

    def node(self, target: str) -> dict:
        if self.backend == "local":
            return {"node": "local"}
        if self.backend == "k8s":
            name = kubectl(["get", "pod", target, "-o", "jsonpath={.spec.nodeName}"], self.config).strip()
            size = kubectl(["get", "node", name, "-o",
                            "jsonpath={.metadata.labels.node\\.kubernetes\\.io/instance-type}"], self.config).strip()
            return {"node": name, "size": size}
        t = self.config["targets"][target]
        return {"node": t["node"], "cpuset": t["cpuset"]}

    def start(self, target: str, command: str, log: str) -> tuple[int, str]:
        """Starts a server in the background; returns its PID and the endpoint it reports, with the
        target's address in place of a wildcard address."""
        pid = int(self.run(target, f"nohup setsid bash -c {shlex.quote(command)} > {log} 2>&1 < /dev/null & echo $!")
                  .strip().splitlines()[-1])
        for _ in range(300):
            for line in self.run(target, f"cat {log} 2>/dev/null || true").splitlines():
                if line.startswith("listening "):
                    endpoint = line.split()[1]
                    return pid, endpoint.replace("0.0.0.0", self.ip(target))
            time.sleep(0.2)
        raise RuntimeError(f"{target}: server did not start: {self.run(target, f'cat {log}', check=False)}")

    def stop(self, target: str, pid: int) -> None:
        self.run(target, f"kill {pid} 2>/dev/null; for i in $(seq 1 100); do kill -0 {pid} 2>/dev/null || exit 0; "
                         f"sleep 0.2; done; kill -9 {pid} 2>/dev/null; true", check=False)

    def statistics(self, target: str, log: str) -> dict | None:
        lines = [line for line in self.run(target, f"cat {log}", check=False).splitlines() if line.startswith("{")]
        return json.loads(lines[-1]) if lines else None

    def cpu_seconds(self, target: str, pid: int) -> float | None:
        out = self.run(target, f"cat /proc/{pid}/stat; getconf CLK_TCK", check=False).split("\n")
        fields = out[0].rsplit(")", 1)[-1].split()
        if len(fields) >= 15 and len(out) > 1 and out[1].strip().isdigit():
            return sum(int(value) for value in fields[11:15]) / int(out[1])
        return None

    def start_gpu_sampler(self, target: str, log: str) -> int:
        loop = ("while true; do printf '%s, ' \"$(date +%s.%N)\"; "
                "nvidia-smi --query-gpu=utilization.gpu,memory.used,memory.total --format=csv,noheader,nounits; "
                "sleep 1; done")
        return int(self.run(target, f"nohup setsid bash -c {shlex.quote(loop)} > {log} 2>&1 < /dev/null & echo $!")
                   .strip().splitlines()[-1])

    def gpu_samples(self, target: str, log: str) -> dict | None:
        rows, total = [], None
        for line in self.run(target, f"cat {log}", check=False).splitlines():
            fields = [field.strip() for field in line.split(",")]
            try:
                stamp, utilisation, used, total = float(fields[0]), int(fields[1]), int(fields[2]), int(fields[3])
            except (ValueError, IndexError):
                continue
            rows.append([stamp, utilisation, used])
        return {"samples": rows, "memory_total_mib": total} if rows else None


def kubectl(arguments: list[str], config: dict, check: bool = True) -> str:
    result = subprocess.run(["kubectl", "-n", config.get("namespace", "lsm")] + arguments, capture_output=True,
                            text=True)
    if check and result.returncode != 0:
        raise RuntimeError(f"kubectl {' '.join(arguments)}: {result.stderr[-1000:]}")
    return result.stdout


def append(path: Path, record: dict) -> None:
    with path.open("a") as out:
        out.write(json.dumps(record) + "\n")


def parallel(work: dict) -> dict:
    """Runs callables at once on threads; returns their results (or exceptions) by key."""
    results: dict = {}

    def call(key, function):
        try:
            results[key] = function()
        except Exception as error:  # reported in the record, not raised from a thread
            results[key] = error

    threads = [threading.Thread(target=call, args=item) for item in work.items()]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    return results


def flags_text(flags: dict) -> str:
    return " ".join(f"--{k} {str(v).lower() if isinstance(v, bool) else v}" for k, v in flags.items())


def slots(n: int) -> list[str]:
    return [f"db-{index}" for index in range(n)]


def preload(platform: Platform, records: int, count: int) -> None:
    def one(target: str) -> str:
        return platform.run(target, f"test -f {GOLDEN}.done && echo done && exit 0; rm -rf {GOLDEN}; "
                                    f"lsm_system_bench --phase preload --dir {GOLDEN} --records {records} --threads 4 "
                                    f"> {GOLDEN}.json 2>&1 && echo {records} > {GOLDEN}.done && echo preloaded",
                            timeout=7200).strip()
    for target, result in parallel({t: (lambda t=t: one(t)) for t in slots(count)}).items():
        if isinstance(result, Exception):
            raise result
        print(f"{target}: {result}", flush=True)


def check_golden(platform: Platform, targets: list[str], records: int) -> None:
    for target in targets:
        if platform.run(target, f"cat {GOLDEN}.done 2>/dev/null || true").strip() != str(records):
            raise SystemExit(f"{target}: no preloaded {records}-key database; run --experiment preload first")


def run_databases(platform: Platform, specs: dict[str, dict], seconds: tuple[int, int]) -> dict:
    """Copies the golden database into memory on every slot, then runs every database at once.
    specs: target -> {"mode", "flags", "env", "records", optional "extra" flags}."""
    copies = parallel({t: (lambda t=t: platform.run(t, f"rm -rf {WORK} && cp -a {GOLDEN} {WORK}", timeout=1800))
                       for t in specs})
    for target, result in copies.items():
        if isinstance(result, Exception):
            raise result

    def one(target: str, spec: dict):
        command = (f"cd /work && LSM_TENANT={target} {spec['env']} lsm_system_bench --phase run --mode {spec['mode']} "
                   f"--dir {WORK} --records {spec['records']} --warmup-seconds {spec.get('warmup', seconds[0])} "
                   f"--duration-seconds {spec.get('duration', seconds[1])} {flags_text(spec['flags'])} "
                   f"{spec.get('extra', '')}; code=$?; rm -rf {WORK}; exit $code")
        if spec.get("delay"):
            time.sleep(spec["delay"])
        return platform.execute(target, command, timeout=seconds[0] + seconds[1] + 3600)

    outcome = {"results": {}, "exit_codes": {}, "errors": {}}
    for target, result in parallel({t: (lambda t=t, s=s: one(t, s)) for t, s in specs.items()}).items():
        if isinstance(result, Exception):
            outcome["exit_codes"][target], outcome["errors"][target] = -1, repr(result)[-300:]
            continue
        code, out, err = result
        outcome["exit_codes"][target] = code
        line = next((x for x in reversed(out.splitlines()) if x.startswith("{")), None)
        if line:
            outcome["results"][target] = json.loads(line)
        if err.strip():
            outcome["errors"][target] = err[-300:].replace("\n", " ")
    return outcome


def service_trial(platform: Platform, config: dict, record: dict, specs: dict, seconds: tuple[int, int],
                  engines: int | None, policy: str | None, keep_timeline: bool = False) -> None:
    """One trial: (re)starts the service if any database uses it, runs the databases, and fills
    the record with their results and the service's statistics, CPU time and GPU samples."""
    server = sampler = None
    log, sampler_log = "/tmp/lsm-cloud-server.log", "/tmp/lsm-cloud-gpu.csv"
    try:
        if any(spec["mode"] == "remote_merge" for spec in specs.values()):
            backend = config.get("backend_name", "cpu_merge")
            server = platform.start("svc", f"{config.get('server_env', '')} LSM_SCHEDULER_LOG=1 lsm_compaction_server "
                                           f"--listen tcp://0.0.0.0:0 --backend {backend} --engines {engines} "
                                           f"--scheduler {policy}", log)
            for spec in specs.values():
                if spec["mode"] == "remote_merge":
                    spec["env"] = (f"LSM_REMOTE_ENDPOINT={server[1]} LSM_REMOTE_CONNECTIONS=4 "
                                   f"{config.get('client_env', '')} {spec.get('env', '')}")
            record["server_cpu_before"] = platform.cpu_seconds("svc", server[0])
            if config.get("gpu"):
                sampler = platform.start_gpu_sampler("svc", sampler_log)
        outcome = run_databases(platform, specs, seconds)
        if not keep_timeline:
            for result in outcome["results"].values():
                result.pop("timeline", None)
        record.update(outcome)
        if server:
            after, before = platform.cpu_seconds("svc", server[0]), record.pop("server_cpu_before")
            record["server_cpu_seconds"] = after - before if None not in (after, before) else None
    finally:
        if sampler:
            platform.stop("svc", sampler)
            record["gpu_telemetry"] = platform.gpu_samples("svc", sampler_log)
        if server:
            platform.stop("svc", server[0])
            record["server_statistics"] = platform.statistics("svc", log)


def base_record(platform: Platform, config: dict, experiment: str, repetition: int, design: str, n: int,
                engines: int | None, targets: list[str]) -> dict:
    return {"experiment": experiment, "platform": platform.backend, "image": config.get("image"),
            "repetition": repetition, "design": design, "instances": n,
            "engines": engines if design != "native" else None,
            "server_env": config.get("server_env") if design != "native" else None,
            "client_env": config.get("client_env") if design != "native" else None,
            "placement": {t: platform.node(t) for t in targets + (["svc"] if design != "native" else [])},
            "started_unix": time.time()}


def experiment_c1(platform: Platform, config: dict, out: Path, name: str, repetitions: int, records: int,
                  seconds: tuple[int, int], counts: list[int], native_counts: list[int], engines: int,
                  policies: list[str], seed_base: int) -> None:
    results = out / name
    done = {(r["repetition"], r["design"], r["instances"]) for r in map(json.loads, results.open())} \
        if results.exists() else set()
    check_golden(platform, slots(max(counts + native_counts)), records)
    for repetition in range(1, repetitions + 1):
        order = [(f"service-{policy}", n) for n in counts for policy in policies]
        order += [("native", n) for n in native_counts]
        random.Random(seed_base + repetition).shuffle(order)
        for design, n in order:
            if (repetition, design, n) in done:
                continue
            targets = slots(n)
            record = base_record(platform, config, "c1", repetition, design, n, engines, targets)
            native = design == "native"
            specs = {t: {"mode": "native_cpu" if native else "remote_merge",
                         "flags": R9_FLAGS["native" if native else "service"], "env": "", "records": records}
                     for t in targets}
            service_trial(platform, config, record, specs, seconds, engines,
                          None if native else design.split("-", 1)[1])
            append(results, record)
            ops = [r["throughput"]["ops_per_second"] for r in record["results"].values()]
            print(f"{record['experiment']} rep {repetition} {design} N={n}: {[round(o / 1e3) for o in ops]} k ops/s, "
                  f"exits {record['exit_codes']}", flush=True)


def experiment_c2(platform: Platform, config: dict, out: Path, repetitions: int, records: int,
                  seconds: tuple[int, int], engines: int, policies: list[str], light_rate: int, seed_base: int) -> None:
    """Tenant db-0 is heavy (closed loop); db-1..db-3 are light (open loop at light_rate). Each
    light tenant also runs alone at the same rate through the same service configuration."""
    results = out / "c2-isolation.jsonl"
    done = {(r["repetition"], r["design"], r["instances"]) for r in map(json.loads, results.open())} \
        if results.exists() else set()
    check_golden(platform, slots(4), records)
    for repetition in range(1, repetitions + 1):
        order = [(f"shared-{policy}", 4) for policy in policies] + [(f"solo-{policy}", 1) for policy in policies]
        random.Random(seed_base + repetition).shuffle(order)
        for design, n in order:
            if (repetition, design, n) in done:
                continue
            policy = design.split("-", 1)[1]
            targets = ["db-1"] if design.startswith("solo") else slots(4)
            record = base_record(platform, config, "c2", repetition, design, n, engines, targets)
            record["light_rate"] = light_rate
            specs = {t: {"mode": "remote_merge", "flags": R9_FLAGS["service"], "env": "", "records": records,
                         "extra": "" if t == "db-0" else f"--rate {light_rate}"} for t in targets}
            service_trial(platform, config, record, specs, seconds, engines, policy)
            append(results, record)
            p999 = {t: round(r["write_latency"]["p999_ns"] / 1e6, 1) for t, r in record["results"].items()}
            print(f"c2 rep {repetition} {design}: p99.9 ms {p999}, exits {record['exit_codes']}", flush=True)


def experiment_c3(platform: Platform, config: dict, out: Path, repetitions: int, records: int, n: int,
                  warmup: int, schedule: tuple[int, int, int], render: dict) -> None:
    """Elastic: n databases use the Service "compaction" (infra/k8s/compaction-service.yaml) with
    one replica; at warmup + schedule[0] seconds a second replica is added, at warmup + schedule[1]
    one is removed (it drains); writing stops at warmup + schedule[2]. Per-second throughput comes
    from the databases' timelines; every replica's statistics are kept from its log."""
    if platform.backend != "k8s":
        raise SystemExit("c3 needs the k8s platform")
    results = out / "c3-elastic.jsonl"
    done = {r["repetition"] for r in map(json.loads, results.open())} if results.exists() else set()
    check_golden(platform, slots(n), records)
    manifest = Path(__file__).resolve().parents[2] / "infra" / "k8s" / "compaction-service.yaml"
    for repetition in range(1, repetitions + 1):
        if repetition in done:
            continue
        kubectl(["delete", "deployment", "compaction", "--ignore-not-found", "--wait=true"], config)
        apply(manifest, {"IMAGE": config["image"], "GPU_LIMIT": "", **render}, config)
        kubectl(["rollout", "status", "deployment/compaction", "--timeout=300s"], config)
        logs: dict[str, subprocess.Popen] = {}
        log_dir = out / f"c3-logs-{repetition}"
        log_dir.mkdir(exist_ok=True)

        def follow():
            for pod in kubectl(["get", "pods", "-l", "app=lsm-compaction", "-o", "name"], config).split():
                pod = pod.split("/", 1)[1]
                if pod not in logs:
                    logs[pod] = subprocess.Popen(["kubectl", "-n", config.get("namespace", "lsm"), "logs", "-f", pod],
                                                 stdout=(log_dir / f"{pod}.log").open("w"), stderr=subprocess.STDOUT)

        follow()
        targets = slots(n)
        record = base_record(platform, config, "c3", repetition, "service-elastic", n, int(render["ENGINES"]), targets)
        record["placement"].pop("svc", None)
        record["schedule"] = {"warmup": warmup, "scale_out_s": schedule[0], "scale_in_s": schedule[1],
                              "duration_s": schedule[2]}
        env = (f"LSM_REMOTE_ENDPOINT=tcp://compaction.{config.get('namespace', 'lsm')}.svc.cluster.local:7471 "
               f"LSM_REMOTE_CONNECTIONS=4 LSM_REMOTE_JOBS_PER_CONNECTION={render.get('JOBS_PER_CONNECTION', 4)} "
               f"{config.get('client_env', '')}")
        specs = {t: {"mode": "remote_merge", "flags": R9_FLAGS["service"], "env": env, "records": records}
                 for t in targets}
        events = []

        def scaler():
            # The databases start after their copies; wait for the first one's warm-up to begin.
            begin = time.time()
            while time.time() - begin < 3600:
                if any(platform.run(t, "pgrep -f 'lsm_system_bench --phase run' >/dev/null && echo up || true",
                                    check=False).strip() == "up" for t in targets[:1]):
                    break
                time.sleep(1)
            start = time.time()
            for at, replicas in ((warmup + schedule[0], 2), (warmup + schedule[1], 1)):
                time.sleep(max(0.0, start + at - time.time()))
                kubectl(["scale", "deployment/compaction", f"--replicas={replicas}"], config)
                events.append({"unix": time.time(), "replicas": replicas})
                for _ in range(120):
                    follow()
                    ready = kubectl(["get", "deployment", "compaction", "-o", "jsonpath={.status.readyReplicas}"],
                                    config).strip()
                    if ready == str(replicas):
                        events.append({"unix": time.time(), "ready_replicas": replicas})
                        break
                    time.sleep(1)

        thread = threading.Thread(target=scaler)
        thread.start()
        outcome = run_databases(platform, specs, (warmup, schedule[2]))
        thread.join()
        record.update(outcome)
        record["scale_events"] = events
        kubectl(["scale", "deployment/compaction", "--replicas=0"], config)
        time.sleep(int(render.get("GRACE_SECONDS", 90)) + 5)
        for process in logs.values():
            process.terminate()
        record["server_statistics"] = {}
        for pod in logs:
            lines = [x for x in (log_dir / f"{pod}.log").read_text().splitlines() if x.startswith("{")]
            record["server_statistics"][pod] = json.loads(lines[-1]) if lines else None
        append(results, record)
        fallbacks = [(r.get("transform") or {}).get("merge_fallbacks") for r in record["results"].values()]
        print(f"c3 rep {repetition}: fallbacks {fallbacks}, events {len(events)}, exits {record['exit_codes']}",
              flush=True)


def render_text(text: str, values: dict) -> str:
    for key, value in values.items():
        text = text.replace(f"__{key}__", str(value))
    if "__" in "".join(line for line in text.splitlines() if not line.lstrip().startswith("#")):
        missing = sorted({part.split("__")[0] for part in text.split("__")[1::2]})
        raise SystemExit(f"unrendered placeholders: {missing}")
    return text


def apply(manifest: Path, values: dict, config: dict) -> None:
    text = render_text(manifest.read_text(), values)
    subprocess.run(["kubectl", "apply", "-f", "-"], input=text, text=True, check=True, capture_output=True)


def setup(config: dict) -> None:
    directory = Path(__file__).resolve().parents[2] / "infra" / "k8s"
    values = {"IMAGE": config["image"], "GPU_LIMIT": "", **config.get("render", {})}
    for name in ("namespace.yaml", "tenants.yaml", "service-pod.yaml"):
        apply(directory / name, values, config)
    kubectl(["rollout", "status", "statefulset/db", "--timeout=900s"], config)
    kubectl(["wait", "--for=condition=Ready", "pod/svc", "--timeout=900s"], config)
    print(kubectl(["get", "pods", "-o", "wide"], config))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--platform", type=Path, required=True)
    parser.add_argument("--experiment", choices=["setup", "preload", "c1", "c2", "c3", "c4"], required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--records", type=int, default=10_000_000)
    parser.add_argument("--warmup-seconds", type=int, default=30)
    parser.add_argument("--duration-seconds", type=int, default=120)
    parser.add_argument("--slots", type=int, default=6, help="preload: database slots")
    parser.add_argument("--counts", default="1,2,4,6", help="c1: databases sharing the service")
    parser.add_argument("--native-counts", default="1,4,6", help="c1: databases for RocksDB")
    parser.add_argument("--engines", type=int, default=8, help="merges the service runs at once")
    parser.add_argument("--policies", default="fair", help="scheduler policies")
    parser.add_argument("--light-rate", type=int, help="c2: open-loop writes/s of each light tenant")
    parser.add_argument("--seed-base", type=int, default=2000)
    parser.add_argument("--results-name", help="c1/c4: results file name in OUT_DIR")
    parser.add_argument("--render", default="{}", help="c3: JSON values for compaction-service.yaml")
    parser.add_argument("--schedule", default="60,150,240", help="c3: scale-out, scale-in, end (s after warm-up)")
    arguments = parser.parse_args()
    config = json.loads(arguments.platform.read_text())
    platform = Platform(config)
    arguments.out.mkdir(parents=True, exist_ok=True)
    seconds = (arguments.warmup_seconds, arguments.duration_seconds)
    counts = [int(x) for x in arguments.counts.split(",")]
    native_counts = [int(x) for x in arguments.native_counts.split(",")] if arguments.native_counts else []
    policies = arguments.policies.split(",")
    if arguments.experiment == "setup":
        setup(config)
    elif arguments.experiment == "preload":
        preload(platform, arguments.records, arguments.slots)
    elif arguments.experiment in ("c1", "c4"):
        name = arguments.results_name or f"{arguments.experiment}-{platform.backend}.jsonl"
        experiment_c1(platform, config, arguments.out, name, arguments.repetitions, arguments.records, seconds,
                      counts, native_counts, arguments.engines, policies, arguments.seed_base)
    elif arguments.experiment == "c2":
        if not arguments.light_rate:
            parser.error("c2 needs --light-rate")
        experiment_c2(platform, config, arguments.out, arguments.repetitions, arguments.records, seconds,
                      arguments.engines, policies, arguments.light_rate, arguments.seed_base)
    else:
        schedule = tuple(int(x) for x in arguments.schedule.split(","))
        experiment_c3(platform, config, arguments.out, arguments.repetitions, arguments.records, counts[-1],
                      arguments.warmup_seconds, schedule, json.loads(arguments.render))
    return 0


if __name__ == "__main__":
    sys.exit(main())
