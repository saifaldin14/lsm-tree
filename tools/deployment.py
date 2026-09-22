"""Provider-neutral, approval-gated cross-host qualification using real SSH and native binaries."""

import importlib
import ipaddress
import json
from pathlib import Path
import re
import shlex
import subprocess
import time
import uuid


experiments = importlib.import_module(f"{__package__}.experiments" if __package__ else "experiments")
SSH_OPTIONS = ["-T", "-oBatchMode=yes", "-oConnectTimeout=10", "-oStrictHostKeyChecking=yes", "-oServerAliveInterval=15", "-oServerAliveCountMax=2"]


def inventory(path: Path) -> dict:
    value = json.loads(path.read_text())
    expected = {"schema_version", "approved", "approval_id", "suite", "shared_parent", "nfs_source", "nfs_protocol", "max_wall_seconds", "hosts"}
    if set(value) != expected or value["schema_version"] != 1 or type(value["approved"]) is not bool:
        raise ValueError("invalid inventory fields or version")
    if value["suite"] not in ("metal", "cuda") or set(value["hosts"]) != {"load", "primary", "worker", "storage"}:
        raise ValueError("inventory requires one declared GPU suite and all four roles")
    if value["nfs_protocol"] not in ("3", "4", "4.0", "4.1", "4.2"):
        raise ValueError("inventory must explicitly declare the NFS protocol version")
    experiments.integer(value["max_wall_seconds"], 60, 3600, "max_wall_seconds")
    if not Path(value["shared_parent"]).is_absolute() or value["shared_parent"] == "/" or not value["nfs_source"]:
        raise ValueError("invalid shared mount path or export source")
    aliases = set()
    for role, host in value["hosts"].items():
        if set(host) != {"ssh", "project", "python", "work_parent", "endpoint", "tls"}:
            raise ValueError(f"invalid host fields for {role}")
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.@-]{0,127}", host["ssh"]) or host["ssh"] in aliases:
            raise ValueError("SSH aliases must be distinct and cannot contain options or shell syntax")
        aliases.add(host["ssh"])
        for key in ("project", "python", "work_parent"):
            path_value = Path(host[key])
            if not path_value.is_absolute() or str(path_value) == "/" or ".." in path_value.parts:
                raise ValueError(f"{role}.{key} must be an explicit absolute path")
        if role != "storage":
            if set(host["tls"]) != {"ca", "cert", "key"} or any(not Path(path).is_absolute() for path in host["tls"].values()):
                raise ValueError("non-loopback qualification requires existing mutual TLS files on every RPC host")
        if role in ("primary", "worker"):
            address, port = host["endpoint"].rsplit(":", 1)
            parsed = ipaddress.ip_address(address.strip("[]"))
            if not parsed.is_private or parsed.is_loopback or parsed.is_unspecified or not 1 <= int(port) <= 65535:
                raise ValueError("qualification endpoints must use explicit private, non-loopback addresses")
    return value


def ssh(host: dict, arguments: list[str]) -> list[str]:
    return ["ssh", *SSH_OPTIONS, host["ssh"], "exec " + shlex.join(arguments)]


def probe(host: dict, action: str, root: str, *extra: str) -> list[str]:
    return [host["python"], str(Path(host["project"]) / "tools" / "host_probe.py"), action, "--root", root, *extra]


def binary(host: dict, name: str) -> str:
    return str(Path(host["project"]) / "build" / "dev" / name)


def tls(host: dict, prefix: str = "") -> list[str]:
    result = []
    for key, flag in (("ca", "tls-ca"), ("cert", "tls-cert"), ("key", "tls-key")):
        result += [f"--{prefix}{flag}", host["tls"][key]]
    return result


def deployment_plan(path: Path, destination: Path) -> dict:
    hosts = inventory(path)
    if destination.exists():
        raise ValueError("deployment plan destination exists")
    report = {"schema_version": 1, "state": "prepared", "approved": hosts["approved"], "executed": False,
              "suite": hosts["suite"], "preflight_commands": {}, "required_gates": [
                  "four distinct machine identities", "same NFS export and path on primary/worker", "native GPU dispatch",
                  "cross-client file sync, full-byte reads, lock exclusion, and rename visibility", "mutually authenticated RPC",
                  "native CPU, remote CPU, CPU-batched, GPU full-value smoke and offline verification",
                  "native resource logs on load, primary, worker, and storage hosts"],
              "resource_collection": {"executable": "lsm_telemetry", "interval_ms": 1000,
                  "hosts": ["load", "primary", "worker", "storage"], "scope": "one collector per physical host",
                  "power_and_thermal_permissions": "operator_managed_no_automatic_elevation",
                  "cross_host_measurement_window_alignment": "UNVERIFIED"},
              "excluded_actions": ["purchasing hardware", "formatting disks", "mount changes", "driver installation", "power-loss or storage-disconnection tests"]}
    for role, host in hosts["hosts"].items():
        root = hosts["shared_parent"] if role in ("primary", "worker") else host["work_parent"]
        report["preflight_commands"][role] = ssh(host, probe(host, "doctor", root, "--project", host["project"]))
    experiments.write_json(destination, report)
    return report


def qualify(path: Path, output: Path, apply: bool) -> dict:
    configuration = inventory(path)
    if not apply or not configuration["approved"] or not configuration["approval_id"]:
        raise ValueError("qualification needs --apply and an approved inventory with an approval_id")
    if output.exists():
        raise ValueError("qualification output already exists")
    output.mkdir()
    deadline = time.monotonic() + configuration["max_wall_seconds"]
    hosts = configuration["hosts"]
    nonce = "qualification-" + uuid.uuid4().hex
    work = {role: str(Path(host["work_parent"]) / nonce) for role, host in hosts.items()}
    calls = []
    report = {"schema_version": 1, "state": "incomplete", "approval_id": configuration["approval_id"], "probe_id": nonce,
              "suite": configuration["suite"], "cross_host_qualified": False, "hardware_qualified": False, "trials": [],
              "power_loss_durability": "UNVERIFIED", "storage_reconnect": "UNVERIFIED"}
    processes = []
    resource_files = []
    resource_summaries = []
    reserved_roles = set()
    locker = None

    def timeout(limit=90):
        remaining = min(limit, deadline - time.monotonic())
        if remaining <= 0:
            raise TimeoutError("cross-host qualification wall-time budget exhausted")
        return remaining

    def call(role, arguments, accepted=(0,), cleanup=False):
        result = subprocess.run(ssh(hosts[role], arguments), capture_output=True, text=True,
                                timeout=15 if cleanup else timeout(), check=False)
        calls.append({"role": role, "arguments": arguments, "exit_code": result.returncode,
                      "stdout": result.stdout[:1048576], "stderr": result.stderr[:16384]})
        experiments.write_json(output / "commands.json", calls)
        if result.returncode not in accepted:
            raise RuntimeError(f"{role} command failed ({result.returncode}): {result.stderr[:1024]}")
        return json.loads(result.stdout) if result.returncode == 0 and result.stdout.strip() else None

    def start(role, arguments, ready_name):
        log = (output / f"{role}-{len(processes)}.log").open("xb")
        process = subprocess.Popen(ssh(hosts[role], arguments), stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
        entry = {"role": role, "process": process, "log": log, "pid": None, "stopped": False}
        processes.append(entry)
        end = time.monotonic() + timeout(30)
        while time.monotonic() < end:
            if process.poll() is not None:
                raise RuntimeError(f"{role} exited before readiness")
            ready = call(role, probe(hosts[role], "read", work[role], "--name", ready_name), accepted=(0, 2))
            if ready:
                if ready.get("state") != "ready":
                    raise ValueError("server reported non-ready state")
                entry["pid"] = ready["pid"]
                return entry
            time.sleep(0.1)
        raise TimeoutError(f"{role} readiness timed out")

    def stop(entry, cleanup=False):
        role = entry["role"]
        if entry["stopped"]:
            return
        if entry["process"].poll() is None:
            if entry["pid"] is None:
                raise RuntimeError(f"{role} process identity unavailable; remote cleanup needs inspection")
            call(role, probe(hosts[role], "stop", work[role], "--pid", str(entry["pid"])), cleanup=cleanup)
        exit_code = entry["process"].wait(timeout=70 if cleanup else timeout(70))
        entry["log"].close()
        entry["stopped"] = True
        if exit_code != 0:
            raise RuntimeError(f"{role} shutdown returned {exit_code}")

    try:
        reports = {}
        for role, host in hosts.items():
            root = configuration["shared_parent"] if role in ("primary", "worker") else host["work_parent"]
            reports[role] = call(role, probe(host, "doctor", root, "--project", host["project"]))
        report["hosts"] = reports
        if any(not host["binaries"].get("lsm_telemetry", False) for host in reports.values()):
            raise ValueError("native telemetry collector is required on all four hosts")
        if len({host["machine_id_sha256"] for host in reports.values()}) != 4:
            raise ValueError("qualification requires four distinct hosts; aliases on one host do not count")
        for role in ("primary", "worker"):
            mount = reports[role]["mount"]
            if not mount or not mount["fstype"].startswith("nfs") or mount["source"] != configuration["nfs_source"] or "soft" in mount["options"].split(","):
                raise ValueError(f"{role} does not use the declared hard NFS export")
            expected_protocol = "4.0" if configuration["nfs_protocol"] == "4" else configuration["nfs_protocol"]
            if mount.get("protocol") != expected_protocol:
                raise ValueError(f"{role} NFS protocol is unknown or differs from the declared version")
            if reports[role]["shared_path"] != configuration["shared_parent"]:
                raise ValueError("primary and worker shared paths must be canonical and identical")
        backend = configuration["suite"] + "_batched"
        hardware = call("worker", [binary(hosts["worker"], "lsm_gpu_check"), backend, "--require"])
        if hardware["status"] != "PASS" or hardware["dispatches"] <= 0:
            raise ValueError("worker did not execute and verify GPU work")
        report["hardware"] = hardware
        report["hardware_qualified"] = True
        for role, host in hosts.items():
            call(role, probe(host, "reserve", host["work_parent"], "--id", nonce))
            reserved_roles.add(role)
        shared_probe = str(Path(configuration["shared_parent"]) / nonce)
        call("primary", probe(hosts["primary"], "reserve", configuration["shared_parent"], "--id", nonce))
        call("primary", probe(hosts["primary"], "publish", shared_probe))
        call("worker", probe(hosts["worker"], "verify", shared_probe))
        locker = subprocess.Popen(ssh(hosts["primary"], probe(hosts["primary"], "lock", shared_probe)),
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        end = time.monotonic() + timeout(30)
        while time.monotonic() < end:
            if call("worker", probe(hosts["worker"], "read", shared_probe, "--name", "lock.ready"), accepted=(0, 2)):
                break
            time.sleep(0.1)
        call("worker", probe(hosts["worker"], "locked", shared_probe))
        call("worker", probe(hosts["worker"], "rename", shared_probe))
        call("primary", probe(hosts["primary"], "verify-renamed", shared_probe))
        locker.communicate(input="x", timeout=timeout(10))
        if locker.returncode != 0:
            raise RuntimeError("remote lock holder failed")
        locker = None
        call("worker", probe(hosts["worker"], "unlocked", shared_probe))
        report["filesystem_contract"] = "PASS"
        shared = str(Path(shared_probe) / "data")
        call("primary", [binary(hosts["primary"], "lsm_admin"), "init-shared", shared])
        spec = json.loads((experiments.ROOT / "configs" / "local-smoke.json").read_text())
        spec["dataset"]["records"] = 4096
        spec["engine"]["write_buffer_bytes"] = 65536
        spec["engine"]["target_file_bytes"] = 65536
        call("primary", probe(hosts["primary"], "write-spec", work["primary"], "--json", json.dumps(spec)))
        call("primary", [binary(hosts["primary"], "lsm_admin"), "create", str(Path(work["primary"]) / "dataset-spec.json"), shared + "/baseline"])
        worker_arguments = [binary(hosts["worker"], "lsm_worker"), "--shared-root", shared,
                            "--journal", str(Path(work["worker"]) / "journal"), "--listen", hosts["worker"]["endpoint"],
                            "--ready-file", str(Path(work["worker"]) / "worker.ready"),
                            "--resources", str(Path(work["worker"]) / "process-resources.jsonl"), *tls(hosts["worker"])]
        resource_files.append(("worker", "process-resources.jsonl", "process-resources.jsonl"))
        worker = start("worker", worker_arguments, "worker.ready")
        for mode in ("native_cpu", "cpu_native", "cpu_batched", backend):
            dataset = shared + "/" + mode
            run_id = nonce + "-" + mode
            call("primary", [binary(hosts["primary"], "lsm_admin"), "clone", shared + "/baseline", dataset])
            collectors = []
            for role, host in hosts.items():
                resource_name = mode + "-host-resources.jsonl"
                ready_name = mode + "-telemetry.ready"
                resource_files.append((role, resource_name, resource_name))
                collectors.append(start(role, [binary(host, "lsm_telemetry"), "--role", role,
                    "--output", str(Path(work[role]) / resource_name), "--interval-ms", "1000",
                    "--max-output-bytes", "33554432", "--ready-file", str(Path(work[role]) / ready_name)], ready_name))
            primary_resource_name = mode + "-process-resources.jsonl"
            resource_files.append(("primary", primary_resource_name, primary_resource_name))
            resource_files.append(("load", mode + "/resources.jsonl", primary_resource_name))
            primary_arguments = [binary(hosts["primary"], "lsm_db"), "--dataset", dataset, "--run-id", run_id,
                "--mode", "native_cpu" if mode == "native_cpu" else "remote_worker", "--listen", hosts["primary"]["endpoint"],
                "--ready-file", str(Path(work["primary"]) / (mode + ".ready")), "--events", str(Path(work["primary"]) / (mode + ".events")),
                "--summary", str(Path(work["primary"]) / (mode + ".summary")),
                "--resources", str(Path(work["primary"]) / primary_resource_name), *tls(hosts["primary"])]
            if mode != "native_cpu":
                primary_arguments += ["--worker", hosts["worker"]["endpoint"], "--shared-root", shared, "--backend", mode,
                                      *tls(hosts["primary"], "worker-")]
            primary = start("primary", primary_arguments, mode + ".ready")
            load_output = str(Path(work["load"]) / mode)
            result = call("load", [binary(hosts["load"], "lsm_loadgen"), "--endpoint", hosts["primary"]["endpoint"], "--run-id", run_id,
                "--output", load_output, "--requests", "6000", "--warmup-requests", "1000", "--rate", "1000", "--seed", "17",
                "--write-budget-bytes", "16777216", "--artifact-budget-bytes", "33554432", *tls(hosts["load"])], accepted=(0, 3))
            stop(primary)
            for collector in collectors:
                stop(collector)
            local_load = output / (mode + "-load")
            subprocess.run(["scp", "-r", "-oBatchMode=yes", "-oStrictHostKeyChecking=yes", f"{hosts['load']['ssh']}:{load_output}", str(local_load)],
                           check=True, timeout=timeout())
            shared_validation = shared_probe + "/" + mode + "-load"
            subprocess.run(["scp", "-r", "-oBatchMode=yes", "-oStrictHostKeyChecking=yes", str(local_load),
                            f"{hosts['primary']['ssh']}:{shared_validation}"], check=True, timeout=timeout())
            verification = call("primary", [binary(hosts["primary"], "lsm_admin"), "verify-trial", dataset, shared_validation])
            summary = call("primary", probe(hosts["primary"], "read", work["primary"], "--name", mode + ".summary"))
            gpu_work = mode != backend or summary["remote"]["gpu_blocks"] > 0
            useful = mode == "native_cpu" or summary["remote"]["committed"] > 0
            record = {"mode": mode, "valid": bool(result and result["valid"] and gpu_work and useful),
                      "verification": verification, "primary": summary}
            report["trials"].append(record)
            if not record["valid"]:
                raise ValueError(f"{mode} smoke failed correctness, RPC accounting, or useful-work gate")
        stop(worker)
        report.update(state="complete", cross_host_qualified=True)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        report["failure"] = str(error)
    finally:
        if locker:
            try:
                locker.communicate(input="x", timeout=10)
            except (OSError, subprocess.SubprocessError):
                locker.kill()
                locker.communicate()
        for entry in reversed(processes):
            if not entry["stopped"]:
                try:
                    stop(entry, cleanup=True)
                except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
                    report.setdefault("cleanup_failures", []).append(str(error))
                    report["cross_host_qualified"] = False
                    if entry["process"].poll() is None:
                        entry["process"].kill()
                        entry["process"].wait()
                    entry["log"].close()
        for role, remote_name, local_name in resource_files:
            if role not in reserved_roles:
                continue
            destination = output / "resources" / role
            destination.mkdir(parents=True, exist_ok=True)
            remote_path = str(Path(work[role]) / remote_name)
            try:
                subprocess.run(["scp", "-oBatchMode=yes", "-oStrictHostKeyChecking=yes", "-oConnectTimeout=10",
                                f"{hosts[role]['ssh']}:{remote_path}", str(destination / local_name)],
                               check=True, timeout=30, capture_output=True, text=True)
                analysis = importlib.import_module(f"{__package__}.analysis" if __package__ else "analysis")
                resource_summaries.append({"file": str((destination / local_name).relative_to(output)),
                    "host_role": role, **analysis.resource_stream(destination / local_name)})
            except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
                report.setdefault("resource_collection_failures", []).append({"role": role, "path": remote_name, "error": str(error)})
                report["cross_host_qualified"] = False
        report["resource_logs_retrieved"] = bool(resource_files) and not report.get("resource_collection_failures")
        report["resource_logs_complete"] = report["resource_logs_retrieved"] and all(
            summary["status"] == "complete" for summary in resource_summaries)
        if resource_summaries:
            analysis = importlib.import_module(f"{__package__}.analysis" if __package__ else "analysis")
            analysis.write_resource_report(resource_summaries, output)
            report["resource_report"] = "resources.json"
        report["resource_sensor_availability"] = "reported_per_stream_not_implied_by_cross_host_correctness"
        report["cross_host_resource_window_alignment"] = "UNVERIFIED: host-local clocks are not interchangeable"
        experiments.write_json(output / "qualification.json", report)
        experiments.seal_bundle(output)
    return report