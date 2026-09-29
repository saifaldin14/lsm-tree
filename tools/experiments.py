"""Unmeasured experiment planning, execution, and artifact verification."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import random
import shutil
import subprocess
import tempfile
import time
import uuid
import socket


def is_relative_to(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


ROOT = Path(__file__).resolve().parents[1]
BINARIES = ROOT / "build" / "dev"
MODES = {"native_cpu", "cpu_native", "cpu_batched", "metal_batched", "cuda_batched", "cuda_local"}


def canonical(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()


def file_hash(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_json(path: Path, value: object) -> None:
    temporary = path.with_name(f".{path.name}-{uuid.uuid4().hex}")
    with temporary.open("xb") as output:
        output.write(json.dumps(value, indent=2, sort_keys=True, allow_nan=False).encode() + b"\n")
        output.flush()
        os.fsync(output.fileno())
    os.replace(temporary, path)
    descriptor = os.open(path.parent, os.O_RDONLY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def working_directory(output: Path) -> tuple[Path, Path | None]:
    configured = os.environ.get("LSM_LAB_SCRATCH_ROOT")
    if not configured:
        return output / "shared", None
    root = Path(configured).expanduser().resolve(strict=True)
    if not root.is_dir():
        raise ValueError("LSM_LAB_SCRATCH_ROOT must name an existing directory")
    container = Path(tempfile.mkdtemp(prefix=f"lsm-lab-{output.name}-", dir=root))
    return container / "shared", container


def integer(value: object, minimum: int, maximum: int, name: str) -> int:
    if type(value) is not int or not minimum <= value <= maximum:
        raise ValueError(f"{name} must be an integer from {minimum} to {maximum}")
    return value


def make_plan(config: dict, dataset: dict, *, resource_accounting: bool = True) -> dict:
    fields = {"schema_version", "name", "dataset_spec", "modes", "rates", "repetitions", "seed",
              "warmup_seconds", "measurement_seconds", "read_percent", "threads", "queue_capacity",
              "deadline_ms", "read_slo_ms", "write_slo_ms", "max_wall_seconds",
              "max_logical_write_bytes", "max_artifact_bytes"}
    if not isinstance(config, dict) or set(config) != fields or config["schema_version"] != 1:
        raise ValueError("unknown, missing, or unsupported experiment configuration fields")
    name = config["name"]
    if not isinstance(name, str) or not name or len(name) > 64 or not all(character.isascii() and
            (character.isalnum() or character in "-_") for character in name):
        raise ValueError("experiment name must contain ASCII letters, digits, hyphens, or underscores")
    modes = config["modes"]
    rates = config["rates"]
    if not isinstance(modes, list) or not modes or any(mode not in MODES for mode in modes) or len(set(modes)) != len(modes):
        raise ValueError("invalid or repeated treatment mode")
    if not isinstance(rates, list) or not rates or len(rates) > 32:
        raise ValueError("rates must be a nonempty list with at most 32 entries")
    rates = sorted(integer(rate, 1, 10_000_000, "rate") for rate in rates)
    if len(set(rates)) != len(rates):
        raise ValueError("repeated offered rate")
    repetitions = integer(config["repetitions"], 1, 100, "repetitions")
    seed = integer(config["seed"], 0, 2**64 - 1, "seed")
    warmup = integer(config["warmup_seconds"], 0, 3600, "warmup_seconds")
    measurement = integer(config["measurement_seconds"], 1, 3600, "measurement_seconds")
    integer(config["read_percent"], 0, 100, "read_percent")
    integer(config["threads"], 1, 64, "threads")
    integer(config["queue_capacity"], 1, 65536, "queue_capacity")
    integer(config["deadline_ms"], 1, 60000, "deadline_ms")
    for field in ("read_slo_ms", "write_slo_ms"):
        integer(config[field], 1, 60000, field)
    wall = integer(config["max_wall_seconds"], 1, 31 * 86400, "max_wall_seconds")
    logical_budget = integer(config["max_logical_write_bytes"], 1, 2**60, "max_logical_write_bytes")
    artifact_budget = integer(config["max_artifact_bytes"], 1, 2**60, "max_artifact_bytes")
    generator = random.Random(seed)
    trials = []
    for repetition in range(repetitions):
        workload_seed = generator.getrandbits(64)
        blocks = rates.copy()
        generator.shuffle(blocks)
        for rate in blocks:
            order = modes.copy()
            generator.shuffle(order)
            for mode in order:
                requests = rate * measurement
                warmup_requests = rate * warmup
                if requests > 1_000_000_000 or warmup_requests > 1_000_000_000:
                    raise ValueError("trial exceeds supported request count")
                trials.append({"id": f"trial-{len(trials):04d}", "repetition": repetition,
                               "mode": mode, "rate": rate, "seed": workload_seed,
                               "requests": requests, "warmup_requests": warmup_requests})
    records = dataset["dataset"]
    row_bytes = records["key_bytes"] + records["value_bytes"]
    baseline_bytes = records["records"] * row_bytes * (records["overwrite_rounds"] + 1)
    total_requests = sum(trial["requests"] + trial["warmup_requests"] for trial in trials)
    logical_upper_bound = baseline_bytes + (total_requests * row_bytes if config["read_percent"] < 100 else 0)
    resource_seconds = warmup + measurement + 80
    telemetry = {"schema_version": 1, "interval_ms": 1000,
                 "host_budget_bytes": min(256 << 20, resource_seconds * (128 << 10)),
                 "process_budget_bytes": min(256 << 20, resource_seconds * (16 << 10)),
                 "host_collection": "once_per_physical_host", "includes_background_activity": True}
    resource_upper_bound = len(trials) * (telemetry["host_budget_bytes"] + 3 * telemetry["process_budget_bytes"])
    artifact_upper_bound = total_requests * 2048 + (resource_upper_bound if resource_accounting else 0)
    if logical_upper_bound > logical_budget or artifact_upper_bound > artifact_budget:
        raise ValueError("experiment exceeds declared logical-write or artifact budget")
    if len(trials) * (warmup + measurement + 10) + 30 > wall:
        raise ValueError("wall-time budget does not allow the planned trials and startup margin")
    body = {"schema_version": 1, "owner": "lsm-lab", "topology": "local_process",
            "config": config, "dataset": dataset, "trials": trials,
            "logical_write_upper_bound": logical_upper_bound, "artifact_upper_bound": artifact_upper_bound}
    if resource_accounting:
        body.update(telemetry=telemetry, resource_artifact_upper_bound=resource_upper_bound)
    return {**body, "plan_sha256": hashlib.sha256(canonical(body)).hexdigest()}


def load_plan(path: Path) -> dict:
    plan = json.loads(path.read_text())
    digest = plan.pop("plan_sha256")
    if hashlib.sha256(canonical(plan)).hexdigest() != digest:
        raise ValueError("frozen plan checksum mismatch")
    regenerated = make_plan(plan["config"], plan["dataset"], resource_accounting="telemetry" in plan)
    if regenerated["plan_sha256"] != digest:
        raise ValueError("plan does not match its deterministic specification")
    return regenerated


def freeze_plan(config_path: Path, destination: Path) -> dict:
    if destination.exists():
        raise ValueError("plan destination already exists")
    config = json.loads(config_path.read_text())
    spec = (ROOT / config["dataset_spec"]).resolve(strict=True)
    completed = subprocess.run([str(BINARIES / "lsm_admin"), "validate", str(spec)],
                               capture_output=True, text=True, check=True, timeout=30)
    plan = make_plan(config, json.loads(completed.stdout)["spec"])
    write_json(destination, plan)
    return plan


class NativeProcess:
    def __init__(self, arguments: list[str], ready: Path, log: Path, deadline: float):
        self.arguments = arguments
        self.ready_file = ready
        self.log_path = log
        self.deadline = deadline
        self.log = log.open("xb")
        try:
            self.process = subprocess.Popen(arguments, stdout=self.log, stderr=subprocess.STDOUT)
        except BaseException:
            self.log.close()
            raise

    def ready_report(self) -> dict:
        end = min(self.deadline, time.monotonic() + 30)
        while time.monotonic() < end:
            if self.ready_file.exists():
                report = json.loads(self.ready_file.read_text())
                if report.get("state") != "ready" or report.get("pid") != self.process.pid:
                    raise ValueError("readiness record does not match the started process")
                return report
            if self.process.poll() is not None:
                raise RuntimeError(f"process exited before readiness: {self.log_path}")
            time.sleep(0.02)
        raise TimeoutError(f"process readiness deadline exceeded: {self.log_path}")

    def ready(self) -> str:
        return self.ready_report()["endpoint"]

    def stop(self) -> int:
        if self.process.poll() is None:
            self.process.terminate()
        try:
            result = self.process.wait(timeout=max(0.1, min(70, self.deadline - time.monotonic())))
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
            raise TimeoutError(f"process required forced termination: {self.log_path}")
        finally:
            self.log.close()
        return result


def seal_bundle(directory: Path) -> dict:
    files = []
    for path in sorted(directory.rglob("*")):
        if path.name == "bundle.json":
            continue
        if path.is_symlink():
            raise ValueError(f"symlink in run bundle: {path}")
        if path.is_file():
            files.append({"path": path.relative_to(directory).as_posix(), "bytes": path.stat().st_size,
                          "sha256": file_hash(path)})
    report = {"schema_version": 1, "owner": "lsm-lab", "files": files}
    write_json(directory / "bundle.json", report)
    return report


def verify_bundle(directory: Path) -> dict:
    manifest = json.loads((directory / "bundle.json").read_text())
    if manifest.get("owner") != "lsm-lab" or manifest.get("schema_version") != 1:
        raise ValueError("unsupported bundle manifest")
    root = directory.resolve(strict=True)
    paths = set()
    for entry in manifest["files"]:
        relative = Path(entry["path"])
        if relative.is_absolute() or ".." in relative.parts or entry["path"] in paths:
            raise ValueError("invalid or repeated bundle path")
        paths.add(entry["path"])
        path = root / relative
        if path.is_symlink() or not is_relative_to(path.resolve(strict=True), root):
            raise ValueError("bundle path escaped run directory")
        if path.stat().st_size != entry["bytes"] or file_hash(path) != entry["sha256"]:
            raise ValueError(f"bundle checksum mismatch: {entry['path']}")
    actual = {path.relative_to(root).as_posix() for path in root.rglob("*") if path.is_file() and path.name != "bundle.json"}
    if actual != paths:
        raise ValueError("bundle inventory mismatch")
    return {"status": "ok", "files_verified": len(paths)}


def run_plan(plan_path: Path, output: Path) -> dict:
    plan = load_plan(plan_path)
    if "telemetry" not in plan:
        raise ValueError("legacy plans remain analyzable; freeze a new plan to include resource budgets before execution")
    if output.exists():
        raise ValueError("run destination already exists")
    executables = {name: BINARIES / name for name in ("lsm_admin", "lsm_db", "lsm_worker", "lsm_loadgen", "lsm_telemetry")}
    for path in executables.values():
        if not path.is_file() or not os.access(path, os.X_OK):
            raise ValueError(f"required executable unavailable: {path}")
    output = output.absolute()
    output.mkdir()
    output = output.resolve()
    shared, scratch_container = working_directory(output)
    deadline = time.monotonic() + plan["config"]["max_wall_seconds"]
    write_json(output / "plan.json", plan)
    write_json(output / "dataset-spec.json", plan["dataset"])
    build = {"binaries": {name: file_hash(path) for name, path in executables.items()},
             "toolchain_lock": json.loads((ROOT / "toolchains.lock.json").read_text()),
             "host": socket.gethostname(), "topology": "local_process", "cross_host_qualified": False,
             "working_directory": str(shared), "external_working_directory": scratch_container is not None}
    write_json(output / "build.json", build)
    records = []

    def remaining() -> float:
        seconds = deadline - time.monotonic()
        if seconds <= 0:
            raise TimeoutError("experiment wall-time budget exhausted")
        return seconds

    def command(arguments: list[str], label: Path, accepted: tuple = (0,)) -> tuple[int, dict | None]:
        remaining()
        started = time.monotonic_ns()
        with label.with_suffix(".stdout").open("xb") as stdout, label.with_suffix(".stderr").open("xb") as stderr:
            completed = subprocess.run(arguments, stdout=stdout, stderr=stderr, timeout=remaining(), check=False)
        records.append({"command": arguments, "exit_code": completed.returncode,
                        "wall_ns": time.monotonic_ns() - started, "log": label.relative_to(output).as_posix()})
        write_json(output / "commands.json", records)
        if completed.returncode not in accepted:
            raise RuntimeError(f"command failed ({completed.returncode}); see {label.with_suffix('.stderr')}")
        text = label.with_suffix(".stdout").read_text()
        return completed.returncode, json.loads(text) if text.strip() else None

    started = time.monotonic_ns()
    trials = []
    summary = {"schema_version": 1, "state": "running", "plan_sha256": plan["plan_sha256"],
               "topology": "local_process", "cross_host_qualified": False, "trials": trials}
    write_json(output / "run.json", summary)
    try:
        command([str(executables["lsm_admin"]), "init-shared", str(shared)], output / "init-shared")
        command([str(executables["lsm_admin"]), "create", str(output / "dataset-spec.json"), str(shared / "baseline")],
                output / "baseline-create")
        command([str(executables["lsm_admin"]), "verify", str(shared / "baseline")], output / "baseline-verify")
        (output / "trials").mkdir()
        for trial in plan["trials"]:
            remaining()
            directory = output / "trials" / trial["id"]
            directory.mkdir()
            dataset = shared / trial["id"]
            command([str(executables["lsm_admin"]), "clone", str(shared / "baseline"), str(dataset)], directory / "clone")
            run_id = f"{plan['config']['name']}-{trial['id']}"
            primary = None
            worker = None
            host_collector = None
            outcome = {**trial, "state": "running", "valid": False}
            trials.append(outcome)
            write_json(output / "run.json", summary)
            try:
                telemetry = plan["telemetry"]
                host_arguments = [str(executables["lsm_telemetry"]), "--output", str(directory / "host-resources.jsonl"),
                                  "--role", "local_host", "--ready-file", str(directory / "host.ready"),
                                  "--interval-ms", str(telemetry["interval_ms"]),
                                  "--max-output-bytes", str(telemetry["host_budget_bytes"])]
                host_collector = NativeProcess(host_arguments, directory / "host.ready", directory / "host.log", deadline)
                outcome["resource_host"] = host_collector.ready_report()["host"]
                outcome["resource_scope"] = "one_shared_local_host_not_four_distinct_hosts"
                resource_arguments = ["--resource-interval-ms", str(telemetry["interval_ms"]),
                                      "--resource-budget-bytes", str(telemetry["process_budget_bytes"])]
                mode = trial["mode"] if trial["mode"] in ("native_cpu", "cuda_local") else "remote_worker"
                arguments = [str(executables["lsm_db"]), "--dataset", str(dataset), "--run-id", run_id, "--mode", mode,
                             "--listen", "127.0.0.1:0", "--ready-file", str(directory / "primary.ready"),
                             "--events", str(directory / "primary.events.jsonl"), "--summary", str(directory / "primary-summary.json"),
                             "--resources", str(directory / "primary-resources.jsonl"), *resource_arguments]
                if mode == "remote_worker":
                    worker_arguments = [str(executables["lsm_worker"]), "--shared-root", str(shared),
                                        "--journal", str(directory / "journal"), "--listen", "127.0.0.1:0",
                                        "--ready-file", str(directory / "worker.ready"),
                                        "--resources", str(directory / "worker-resources.jsonl"), *resource_arguments]
                    worker = NativeProcess(worker_arguments, directory / "worker.ready", directory / "worker.log", deadline)
                    arguments += ["--worker", worker.ready(), "--shared-root", str(shared), "--backend", trial["mode"]]
                primary = NativeProcess(arguments, directory / "primary.ready", directory / "primary.log", deadline)
                endpoint = primary.ready()
                config = plan["config"]
                load_arguments = [str(executables["lsm_loadgen"]), "--endpoint", endpoint, "--run-id", run_id,
                                  "--require-mode", mode, "--output", str(directory / "load")]
                load_values = {"requests": trial["requests"], "warmup-requests": trial["warmup_requests"],
                               "rate": trial["rate"], "seed": trial["seed"], "read-percent": config["read_percent"],
                               "threads": config["threads"], "queue-capacity": config["queue_capacity"], "deadline-ms": config["deadline_ms"],
                               "write-budget-bytes": config["max_logical_write_bytes"], "artifact-budget-bytes": config["max_artifact_bytes"]}
                for name, value in load_values.items():
                    load_arguments += [f"--{name}", str(value)]
                exit_code, result = command(load_arguments, directory / "load-command", accepted=(0, 3))
                outcome["load_exit_code"] = exit_code
                primary_exit = primary.stop()
                primary = None
                outcome["primary_exit_code"] = primary_exit
                if worker:
                    outcome["worker_exit_code"] = worker.stop()
                    worker = None
                outcome["host_collector_exit_code"] = host_collector.stop()
                host_collector = None
                verify_exit, verification = command([str(executables["lsm_admin"]), "verify-trial", str(dataset), str(directory / "load")],
                                                    directory / "verification", accepted=(0, 1))
                outcome["verification"] = verification
                outcome["valid"] = bool(result and result["valid"] and verify_exit == 0 and primary_exit == 0 and
                                        outcome.get("worker_exit_code", 0) == 0 and outcome["host_collector_exit_code"] == 0)
                outcome["state"] = "complete"
                outcome["result"] = (directory / "load" / "result.json").relative_to(output).as_posix()
            except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
                outcome.update(state="failed", failure=str(error), valid=False)
                if isinstance(error, TimeoutError):
                    raise
            finally:
                for role, process in (("primary", primary), ("worker", worker), ("host_collector", host_collector)):
                    if process:
                        try:
                            outcome[f"{role}_exit_code"] = process.stop()
                            if outcome[f"{role}_exit_code"] != 0:
                                outcome["valid"] = False
                        except (OSError, TimeoutError) as error:
                            outcome.update(state="failed", failure=str(error), valid=False)
                write_json(output / "run.json", summary)
        summary["state"] = "complete"
    except BaseException as error:
        summary.update(state="incomplete", failure=str(error))
        raise
    finally:
        summary["wall_ns"] = time.monotonic_ns() - started
        write_json(output / "run.json", summary)
        if scratch_container is not None and shared.exists():
            shutil.copytree(shared, output / "shared")
            shutil.rmtree(scratch_container)
        seal_bundle(output)
    return summary
