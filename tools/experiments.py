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
MODES = {"native_cpu", "cpu_native", "cpu_batched", "metal_batched", "cuda_batched", "cuda_local", "cuda_merge"}


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


OPEN_LOOP_FIELDS = {"schema_version", "name", "dataset_spec", "modes", "rates", "repetitions", "seed",
                    "warmup_seconds", "measurement_seconds", "read_percent", "threads", "queue_capacity",
                    "deadline_ms", "read_slo_ms", "write_slo_ms", "max_wall_seconds",
                    "max_logical_write_bytes", "max_artifact_bytes"}
CLOSED_LOOP_FIELDS = (OPEN_LOOP_FIELDS - {"rates", "warmup_seconds", "measurement_seconds", "queue_capacity"}) | {
    "load_mode", "duration_seconds", "warmup_duration_seconds"}
# Optional fields; when absent the plan (and its checksum) is identical to plans made before they existed.
OPTIONAL_FIELDS = {"load_mode", "insert_percent", "core_budget", "numa_node", "background_load_percent", "trial_root"}


def validate_optional_fields(config: dict) -> None:
    if "insert_percent" in config:
        integer(config["insert_percent"], 0, 100, "insert_percent")
    if "core_budget" in config:
        integer(config["core_budget"], 1, 1024, "core_budget")
    if "numa_node" in config:
        integer(config["numa_node"], 0, 1023, "numa_node")
        if "core_budget" not in config:
            raise ValueError("numa_node requires core_budget")
    if "background_load_percent" in config:
        percent = integer(config["background_load_percent"], 0, 100, "background_load_percent")
        if percent and "core_budget" not in config:
            raise ValueError("background_load_percent requires core_budget")
    if "trial_root" in config:
        root = config["trial_root"]
        if not isinstance(root, str) or not root or len(root) > 4096 or "\0" in root:
            raise ValueError("trial_root must be a nonempty path string")
        if not os.path.isabs(os.path.expanduser(os.path.expandvars(root))):
            raise ValueError("trial_root must be an absolute path (environment variables and ~ are expanded)")


def make_plan(config: dict, dataset: dict, *, resource_accounting: bool = True) -> dict:
    if not isinstance(config, dict) or config.get("schema_version") != 1:
        raise ValueError("unknown, missing, or unsupported experiment configuration fields")
    load_mode = config.get("load_mode", "open_loop")
    if load_mode not in ("open_loop", "closed_loop"):
        raise ValueError("load_mode must be open_loop or closed_loop")
    fields = CLOSED_LOOP_FIELDS if load_mode == "closed_loop" else OPEN_LOOP_FIELDS
    if not fields <= set(config) or set(config) - fields - OPTIONAL_FIELDS:
        raise ValueError("unknown, missing, or unsupported experiment configuration fields")
    validate_optional_fields(config)
    name = config["name"]
    if not isinstance(name, str) or not name or len(name) > 64 or not all(character.isascii() and
            (character.isalnum() or character in "-_") for character in name):
        raise ValueError("experiment name must contain ASCII letters, digits, hyphens, or underscores")
    modes = config["modes"]
    if not isinstance(modes, list) or not modes or any(mode not in MODES for mode in modes) or len(set(modes)) != len(modes):
        raise ValueError("invalid or repeated treatment mode")
    if load_mode == "closed_loop":
        return make_closed_loop_plan(config, dataset, resource_accounting=resource_accounting)
    rates = config["rates"]
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


def make_closed_loop_plan(config: dict, dataset: dict, *, resource_accounting: bool = True) -> dict:
    """Saturation plan: each repetition runs every mode once with back-to-back clients for a fixed duration."""
    modes = config["modes"]
    repetitions = integer(config["repetitions"], 1, 100, "repetitions")
    seed = integer(config["seed"], 0, 2**64 - 1, "seed")
    warmup = integer(config["warmup_duration_seconds"], 0, 86400, "warmup_duration_seconds")
    duration = integer(config["duration_seconds"], 1, 86400, "duration_seconds")
    integer(config["read_percent"], 0, 100, "read_percent")
    clients = integer(config["threads"], 1, 1024, "threads")
    integer(config["deadline_ms"], 1, 60000, "deadline_ms")
    for field in ("read_slo_ms", "write_slo_ms"):
        integer(config[field], 1, 60000, field)
    wall = integer(config["max_wall_seconds"], 1, 31 * 86400, "max_wall_seconds")
    logical_budget = integer(config["max_logical_write_bytes"], 1, 2**60, "max_logical_write_bytes")
    artifact_budget = integer(config["max_artifact_bytes"], 1, 2**60, "max_artifact_bytes")
    records = dataset["dataset"]
    if records["records"] < clients:
        raise ValueError("closed-loop clients exceed dataset records")
    generator = random.Random(seed)
    trials = []
    for repetition in range(repetitions):
        workload_seed = generator.getrandbits(64)
        order = modes.copy()
        generator.shuffle(order)
        for mode in order:
            trials.append({"id": f"trial-{len(trials):04d}", "repetition": repetition, "mode": mode,
                           "load_mode": "closed_loop", "seed": workload_seed, "clients": clients,
                           "duration_seconds": duration, "warmup_duration_seconds": warmup})
    row_bytes = records["key_bytes"] + records["value_bytes"]
    baseline_bytes = records["records"] * row_bytes * (records["overwrite_rounds"] + 1)
    # The load generator stops a trial (and marks it invalid) once it exhausts its equal share of the write budget.
    trial_write_budget = 0 if config["read_percent"] == 100 else (logical_budget - baseline_bytes) // len(trials)
    if config["read_percent"] < 100 and trial_write_budget < row_bytes:
        raise ValueError("experiment exceeds declared logical-write or artifact budget")
    for trial in trials:
        trial["write_budget_bytes"] = max(1, trial_write_budget)
    logical_upper_bound = baseline_bytes + trial_write_budget * len(trials)
    resource_seconds = warmup + duration + 80
    telemetry = {"schema_version": 1, "interval_ms": 1000,
                 "host_budget_bytes": min(256 << 20, resource_seconds * (128 << 10)),
                 "process_budget_bytes": min(256 << 20, resource_seconds * (16 << 10)),
                 "host_collection": "once_per_physical_host", "includes_background_activity": True}
    resource_upper_bound = len(trials) * (telemetry["host_budget_bytes"] + 3 * telemetry["process_budget_bytes"])
    per_trial_artifacts = (warmup + duration + 62) * 256 + (1 << 20) + (resource_seconds + 2) * (256 << 10)
    artifact_upper_bound = len(trials) * per_trial_artifacts + (resource_upper_bound if resource_accounting else 0)
    if logical_upper_bound > logical_budget or artifact_upper_bound > artifact_budget:
        raise ValueError("experiment exceeds declared logical-write or artifact budget")
    if len(trials) * (warmup + duration + 10) + 30 > wall:
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


def primary_launch_prefix() -> list[str]:
    """Optional NUMA binding for the database process (CPU and memory), e.g. the GPU's node."""
    node = os.environ.get("LSM_LAB_PRIMARY_NUMA_NODE", "")
    if not node:
        return []
    if not node.isdigit():
        raise ValueError("LSM_LAB_PRIMARY_NUMA_NODE must be a NUMA node number")
    numactl = shutil.which("numactl")
    if numactl is None:
        raise ValueError("LSM_LAB_PRIMARY_NUMA_NODE requires numactl")
    return [numactl, f"--cpunodebind={node}", f"--membind={node}"]


def parse_cpu_list(text: str) -> list[int]:
    """Parse a kernel CPU list such as "0-3,8,10-11"."""
    cpus = set()
    for part in text.strip().split(","):
        if not part:
            continue
        first, separator, last = part.partition("-")
        if not first.isdigit() or (separator and not last.isdigit()):
            raise ValueError(f"malformed CPU list: {text!r}")
        low, high = int(first), int(last or first)
        if high < low:
            raise ValueError(f"malformed CPU list: {text!r}")
        cpus.update(range(low, high + 1))
    return sorted(cpus)


def core_threads(node: int, sys_root: Path = Path("/sys")) -> dict[int, list[int]]:
    """Map the lowest-numbered online hardware thread of each physical core on a NUMA node to all its threads."""
    cpu_root = sys_root / "devices" / "system" / "cpu"
    node_list = sys_root / "devices" / "system" / "node" / f"node{node}" / "cpulist"
    if not node_list.is_file():
        raise ValueError(f"NUMA node {node} is not present under {sys_root}")
    node_cpus = set(parse_cpu_list(node_list.read_text()))
    online_file = cpu_root / "online"
    online = set(parse_cpu_list(online_file.read_text())) if online_file.is_file() else node_cpus
    groups: dict = {}
    for cpu in sorted(node_cpus & online):
        topology = cpu_root / f"cpu{cpu}" / "topology"
        siblings_file = next((topology / name for name in ("core_cpus_list", "thread_siblings_list")
                              if (topology / name).is_file()), None)
        if siblings_file is not None:
            identity: object = frozenset(parse_cpu_list(siblings_file.read_text()))
        else:
            fields = [(topology / name).read_text().strip() for name in ("physical_package_id", "die_id", "core_id")
                      if (topology / name).is_file()]
            identity = tuple(fields) if fields else frozenset({cpu})
        groups.setdefault(identity, []).append(cpu)
    return {threads[0]: threads for threads in groups.values()}


def physical_cores(node: int, sys_root: Path = Path("/sys")) -> list[int]:
    """One hardware thread (the lowest-numbered sibling) per online physical core of a NUMA node."""
    return sorted(core_threads(node, sys_root))


def select_cores(count: int, node: int, sys_root: Path = Path("/sys")) -> list[int]:
    cores = physical_cores(node, sys_root)
    if count < 1 or count > len(cores):
        raise ValueError(f"core_budget {count} exceeds the {len(cores)} online physical cores of NUMA node {node}")
    return cores[:count]


def cpu_placement(config: dict, sys_root: Path = Path("/sys")) -> dict | None:
    """Resolve core_budget into the CPU list and launch prefix shared by lsm_db, lsm_worker, and lsm_burn."""
    if "core_budget" not in config:
        return None
    node = config.get("numa_node", 0)
    cpus = select_cores(config["core_budget"], node, sys_root)
    reserved = {thread for cpu in cpus for thread in core_threads(node, sys_root)[cpu]}
    numactl = shutil.which("numactl")
    if numactl is None:
        raise ValueError("core_budget requires numactl")
    cpu_list = ",".join(str(cpu) for cpu in cpus)
    every = set(parse_cpu_list((sys_root / "devices" / "system" / "cpu" / "online").read_text())) \
        if (sys_root / "devices" / "system" / "cpu" / "online").is_file() else set()
    # The load generator runs on the other online CPUs, excluding the idle sibling threads of the reserved cores.
    remaining = sorted(every - reserved)
    return {"cpus": cpus, "numa_node": node, "core_budget": config["core_budget"],
            "prefix": [numactl, f"--physcpubind={cpu_list}", f"--membind={node}"],
            "load_cpus": remaining,
            "load_prefix": [numactl, "--physcpubind=" + ",".join(str(cpu) for cpu in remaining)] if remaining else [],
            "background_load_percent": config.get("background_load_percent", 0)}


def proc_snapshot(proc_root: Path = Path("/proc")) -> dict | None:
    """Host CPU jiffies plus per-process CPU ticks and parent ids, or None without a Linux /proc."""
    try:
        fields = (proc_root / "stat").read_text().splitlines()[0].split()
    except OSError:
        return None
    if not fields or fields[0] != "cpu":
        return None
    total = sum(int(value) for value in fields[1:9])
    processes = {}
    for entry in proc_root.iterdir():
        if not entry.name.isdigit():
            continue
        try:
            text = (entry / "stat").read_text()
        except OSError:
            continue
        name_end = text.rfind(")")
        rest = text[name_end + 2:].split()
        if name_end < 0 or len(rest) < 13:
            continue
        processes[int(entry.name)] = {"ppid": int(rest[1]), "ticks": int(rest[11]) + int(rest[12]),
                                      "comm": text[text.find("(") + 1:name_end][:32]}
    return {"total_jiffies": total, "processes": processes}


def descendants(snapshot: dict, root: int) -> set[int]:
    children = {}
    for pid, process in snapshot["processes"].items():
        children.setdefault(process["ppid"], []).append(pid)
    found, pending = {root}, [root]
    while pending:
        for child in children.get(pending.pop(), []):
            if child not in found:
                found.add(child)
                pending.append(child)
    return found


def foreign_cpu_usage(before: dict, after: dict, runner_pid: int) -> dict:
    """Share of total host CPU capacity used between two snapshots by processes outside the runner's tree."""
    excluded = descendants(before, runner_pid) | descendants(after, runner_pid)
    elapsed = after["total_jiffies"] - before["total_jiffies"]
    usage = []
    for pid, process in after["processes"].items():
        if pid in excluded:
            continue
        previous = before["processes"].get(pid)
        ticks = process["ticks"] - previous["ticks"] if previous and previous["ticks"] <= process["ticks"] else process["ticks"]
        if ticks > 0:
            usage.append((ticks, pid, process["comm"]))
    usage.sort(reverse=True)
    foreign = sum(ticks for ticks, _, _ in usage)
    percent = 100.0 * foreign / elapsed if elapsed > 0 else None
    return {"foreign_cpu_percent": percent, "foreign_ticks": foreign, "host_jiffies": elapsed,
            "top_foreign_processes": [{"pid": pid, "comm": comm, "cpu_percent": 100.0 * ticks / elapsed if elapsed > 0 else None}
                                      for ticks, pid, comm in usage[:5]]}


def host_load(interval: float = 0.5, proc_root: Path = Path("/proc")) -> dict:
    """1-minute load average and CPU use of processes not started by this runner over a short interval."""
    report = {"interval_seconds": interval, "scope": "processes outside the runner's process tree, percent of all host CPUs"}
    try:
        report["loadavg_1m"] = float((proc_root / "loadavg").read_text().split()[0])
    except (OSError, ValueError, IndexError):
        report["loadavg_1m"] = os.getloadavg()[0] if hasattr(os, "getloadavg") else None
    before = proc_snapshot(proc_root)
    if before is None:
        report.update(status="unavailable", reason="no Linux /proc process accounting")
        return report
    time.sleep(interval)
    after = proc_snapshot(proc_root)
    report.update(status="ok", **foreign_cpu_usage(before, after, os.getpid()))
    return report


class NativeProcess:
    def __init__(self, arguments: list[str], ready: Path, log: Path, deadline: float, ready_timeout: float = 30):
        self.arguments = arguments
        self.ready_file = ready
        self.log_path = log
        self.deadline = deadline
        self.ready_timeout = ready_timeout
        self.log = log.open("xb")
        try:
            self.process = subprocess.Popen(arguments, stdout=self.log, stderr=subprocess.STDOUT)
        except BaseException:
            self.log.close()
            raise

    def ready_report(self) -> dict:
        end = min(self.deadline, time.monotonic() + self.ready_timeout)
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


def release_trial_clone(dataset: Path, metadata: Path, outcome: dict) -> None:
    """Keep the small trial manifests in the run bundle and delete the clone from trial_root."""
    try:
        if dataset.is_dir():
            metadata.mkdir(exist_ok=True)
            for name in ("dataset.json", "run.json"):
                if (dataset / name).is_file():
                    shutil.copy2(dataset / name, metadata / name)
    finally:
        shutil.rmtree(dataset, ignore_errors=True)
        outcome["trial_clone_removed"] = not dataset.exists()


def run_plan(plan_path: Path, output: Path) -> dict:
    plan = load_plan(plan_path)
    if "telemetry" not in plan:
        raise ValueError("legacy plans remain analyzable; freeze a new plan to include resource budgets before execution")
    if output.exists():
        raise ValueError("run destination already exists")
    config = plan["config"]
    burn_percent = config.get("background_load_percent", 0)
    names = ("lsm_admin", "lsm_db", "lsm_worker", "lsm_loadgen", "lsm_telemetry") + (("lsm_burn",) if burn_percent else ())
    executables = {name: BINARIES / name for name in names}
    for path in executables.values():
        if not path.is_file() or not os.access(path, os.X_OK):
            raise ValueError(f"required executable unavailable: {path}")
    output = output.absolute()
    placement = cpu_placement(config)
    primary_prefix = placement["prefix"] if placement else primary_launch_prefix()
    trial_root = Path(os.path.expandvars(config["trial_root"])).expanduser() if "trial_root" in config else None
    output.mkdir()
    output = output.resolve()
    shared, scratch_container = working_directory(output)
    trial_container = None
    deadline = time.monotonic() + plan["config"]["max_wall_seconds"]
    write_json(output / "plan.json", plan)
    write_json(output / "dataset-spec.json", plan["dataset"])
    build = {"binaries": {name: file_hash(path) for name, path in executables.items()},
             "toolchain_lock": json.loads((ROOT / "toolchains.lock.json").read_text()),
             "host": socket.gethostname(), "topology": "local_process", "cross_host_qualified": False,
             "working_directory": str(shared), "external_working_directory": scratch_container is not None,
             "primary_launch_prefix": primary_prefix, "cuda_sync": os.environ.get("LSM_CUDA_SYNC") or "auto",
             "lsm_environment": {key: value for key, value in sorted(os.environ.items()) if key.startswith("LSM_")}}
    if placement:
        build["cpu_placement"] = {key: value for key, value in placement.items() if key != "prefix"}
        build["cpu_placement"]["overrides_primary_numa_node"] = bool(os.environ.get("LSM_LAB_PRIMARY_NUMA_NODE"))
    if trial_root is not None:
        build["trial_root"] = str(trial_root)
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
        trial_shared = shared
        if trial_root is not None:
            # Trial clones live in their own shared root (e.g. tmpfs) and are deleted after each trial.
            trial_root.mkdir(parents=True, exist_ok=True)
            trial_container = Path(tempfile.mkdtemp(prefix=f"lsm-lab-{output.name}-", dir=trial_root))
            trial_shared = trial_container / "shared"
            command([str(executables["lsm_admin"]), "init-shared", str(trial_shared)], output / "init-trial-shared")
        (output / "trials").mkdir()
        for trial in plan["trials"]:
            remaining()
            directory = output / "trials" / trial["id"]
            directory.mkdir()
            dataset = trial_shared / trial["id"]
            command([str(executables["lsm_admin"]), "clone", str(shared / "baseline"), str(dataset)], directory / "clone")
            run_id = f"{plan['config']['name']}-{trial['id']}"
            primary = None
            worker = None
            host_collector = None
            burner = None
            outcome = {**trial, "state": "running", "valid": False}
            trials.append(outcome)
            outcome["host_load_before"] = host_load()
            if placement:
                outcome["cpu_placement"] = {"cpus": placement["cpus"], "numa_node": placement["numa_node"],
                                            "launch_prefix": placement["prefix"], "load_cpus": placement["load_cpus"]}
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
                mode = trial["mode"] if trial["mode"] in ("native_cpu", "cuda_local", "cuda_merge") else "remote_worker"
                if burn_percent:
                    burn_arguments = [*placement["prefix"], str(executables["lsm_burn"]),
                                      "--cpus", ",".join(str(cpu) for cpu in placement["cpus"]),
                                      "--percent", str(burn_percent), "--period-ms", "10",
                                      "--ready-file", str(directory / "burn.ready")]
                    burner = NativeProcess(burn_arguments, directory / "burn.ready", directory / "burn.log", deadline)
                    outcome["burner"] = {"cpus": placement["cpus"], "percent": burn_percent, "period_ms": 10,
                                         "ready": burner.ready_report()}
                arguments = [*primary_prefix, str(executables["lsm_db"]), "--dataset", str(dataset), "--run-id", run_id, "--mode", mode,
                             "--listen", "127.0.0.1:0", "--ready-file", str(directory / "primary.ready"),
                             "--events", str(directory / "primary.events.jsonl"), "--summary", str(directory / "primary-summary.json"),
                             "--resources", str(directory / "primary-resources.jsonl"), *resource_arguments]
                if mode == "remote_worker":
                    worker_arguments = [*(placement["prefix"] if placement else []), str(executables["lsm_worker"]),
                                        "--shared-root", str(trial_shared),
                                        "--journal", str(directory / "journal"), "--listen", "127.0.0.1:0",
                                        "--ready-file", str(directory / "worker.ready"),
                                        "--resources", str(directory / "worker-resources.jsonl"), *resource_arguments]
                    worker = NativeProcess(worker_arguments, directory / "worker.ready", directory / "worker.log", deadline)
                    arguments += ["--worker", worker.ready(), "--shared-root", str(trial_shared), "--backend", trial["mode"]]
                # The primary verifies every key of the trial clone before it reports ready.
                primary = NativeProcess(arguments, directory / "primary.ready", directory / "primary.log", deadline,
                                        ready_timeout=1800)
                endpoint = primary.ready()
                load_prefix = placement["load_prefix"] if placement else []
                load_arguments = [*load_prefix, str(executables["lsm_loadgen"]), "--endpoint", endpoint, "--run-id", run_id,
                                  "--require-mode", mode, "--output", str(directory / "load")]
                if trial.get("load_mode") == "closed_loop":
                    load_values = {"closed-loop": trial["clients"], "duration-seconds": trial["duration_seconds"],
                                   "warmup-duration-seconds": trial["warmup_duration_seconds"], "seed": trial["seed"],
                                   "read-percent": config["read_percent"], "deadline-ms": config["deadline_ms"],
                                   "write-budget-bytes": trial["write_budget_bytes"], "artifact-budget-bytes": config["max_artifact_bytes"]}
                else:
                    load_values = {"requests": trial["requests"], "warmup-requests": trial["warmup_requests"],
                                   "rate": trial["rate"], "seed": trial["seed"], "read-percent": config["read_percent"],
                                   "threads": config["threads"], "queue-capacity": config["queue_capacity"], "deadline-ms": config["deadline_ms"],
                                   "write-budget-bytes": config["max_logical_write_bytes"], "artifact-budget-bytes": config["max_artifact_bytes"]}
                if "insert_percent" in config:
                    load_values["insert-percent"] = config["insert_percent"]
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
                if burner:
                    outcome["burner_exit_code"] = burner.stop()
                    burner = None
                verify_exit, verification = command([str(executables["lsm_admin"]), "verify-trial", str(dataset), str(directory / "load")],
                                                    directory / "verification", accepted=(0, 1))
                outcome["verification"] = verification
                outcome["valid"] = bool(result and result["valid"] and verify_exit == 0 and primary_exit == 0 and
                                        outcome.get("worker_exit_code", 0) == 0 and outcome["host_collector_exit_code"] == 0 and
                                        outcome.get("burner_exit_code", 0) == 0)
                outcome["state"] = "complete"
                outcome["result"] = (directory / "load" / "result.json").relative_to(output).as_posix()
            except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
                outcome.update(state="failed", failure=str(error), valid=False)
                if isinstance(error, TimeoutError):
                    raise
            finally:
                for role, process in (("primary", primary), ("worker", worker), ("host_collector", host_collector),
                                      ("burner", burner)):
                    if process:
                        try:
                            outcome[f"{role}_exit_code"] = process.stop()
                            if outcome[f"{role}_exit_code"] != 0:
                                outcome["valid"] = False
                        except (OSError, TimeoutError) as error:
                            outcome.update(state="failed", failure=str(error), valid=False)
                if trial_container is not None:
                    release_trial_clone(dataset, directory / "dataset-metadata", outcome)
                outcome["host_load_after"] = host_load()
                write_json(output / "run.json", summary)
        summary["state"] = "complete"
    except BaseException as error:
        summary.update(state="incomplete", failure=str(error))
        raise
    finally:
        summary["wall_ns"] = time.monotonic_ns() - started
        write_json(output / "run.json", summary)
        if trial_container is not None:
            shutil.rmtree(trial_container, ignore_errors=True)
        if scratch_container is not None and shared.exists():
            shutil.copytree(shared, output / "shared")
            shutil.rmtree(scratch_container)
        seal_bundle(output)
    return summary
