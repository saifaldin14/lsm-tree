"""Unmeasured native-host and shared-filesystem qualification probes."""

from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import signal
import socket
import subprocess
import sys


def is_relative_to(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def sync_directory(path: Path) -> None:
    descriptor = os.open(path, os.O_RDONLY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def durable(path: Path, payload: bytes) -> None:
    temporary = path.with_name(path.name + ".pending")
    with temporary.open("xb") as stream:
        stream.write(payload)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)
    sync_directory(path.parent)


def identifier(value: str) -> str:
    if not re.fullmatch(r"[a-zA-Z0-9_-]{1,80}", value):
        raise ValueError("invalid probe identity")
    return value


def payload(nonce: str) -> bytes:
    identifier(nonce)
    return b"".join(hashlib.sha256(f"{nonce}:{index}".encode()).digest() for index in range(2048))


def reserve(parent: Path, nonce: str) -> dict:
    identifier(nonce)
    parent = parent.resolve(strict=True)
    root = parent / nonce
    root.mkdir()
    marker = {"owner": "lsm-lab", "schema_version": 1, "probe_id": nonce}
    durable(root / "probe.json", json.dumps(marker).encode())
    sync_directory(parent)
    return {"directory": str(root), **marker}


def owned(root: Path) -> dict:
    if root.is_symlink() or not root.is_dir():
        raise ValueError("probe root must be a real directory")
    marker = json.loads((root / "probe.json").read_text())
    if marker.get("owner") != "lsm-lab" or marker.get("schema_version") != 1 or root.name != identifier(marker["probe_id"]):
        raise ValueError("unowned probe directory")
    return marker


def filesystem_probe(root: Path, action: str) -> dict:
    marker = owned(root)
    expected = payload(marker["probe_id"])
    if action == "publish":
        if (root / "payload").exists():
            raise ValueError("probe payload already exists")
        durable(root / "payload", expected)
    elif action == "rename":
        if (root / "renamed").exists() or (root / "payload").is_symlink():
            raise ValueError("invalid rename destination or source")
        os.replace(root / "payload", root / "renamed")
        sync_directory(root)
    elif action in ("verify", "verify-renamed"):
        name = "payload" if action == "verify" else "renamed"
        path = root / name
        if path.is_symlink() or path.stat().st_size != len(expected) or path.read_bytes() != expected:
            raise ValueError("shared-file full-byte verification failed")
    elif action == "lock":
        with (root / "probe.lock").open("a+b") as stream:
            fcntl.lockf(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
            durable(root / "lock.ready", json.dumps({"pid": os.getpid(), "probe_id": marker["probe_id"]}).encode())
            print(json.dumps({"state": "locked", "pid": os.getpid()}), flush=True)
            sys.stdin.buffer.read(1)
            fcntl.lockf(stream, fcntl.LOCK_UN)
        return {"state": "unlocked"}
    elif action in ("locked", "unlocked"):
        with (root / "probe.lock").open("a+b") as stream:
            locked = False
            try:
                fcntl.lockf(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                locked = True
            if locked != (action == "locked"):
                raise ValueError("shared filesystem lock exclusion did not match expected state")
    else:
        raise ValueError("unknown filesystem probe action")
    return {"status": "ok", "action": action, "probe_id": marker["probe_id"], "bytes": len(expected),
            "sha256": hashlib.sha256(expected).hexdigest()}


def command(arguments: list[str]) -> str:
    result = subprocess.run(arguments, capture_output=True, text=True, check=True, timeout=20)
    return result.stdout.strip()


def host_report(project: Path, shared: Path) -> dict:
    system = platform.system()
    if system == "Linux":
        identity = Path("/etc/machine-id").read_text().strip()
        mounts = json.loads(command(["findmnt", "--json", "-T", str(shared), "-o", "TARGET,SOURCE,FSTYPE,OPTIONS"]))["filesystems"]
        mount = mounts[0]
    elif system == "Darwin":
        hardware = command(["ioreg", "-rd1", "-c", "IOPlatformExpertDevice"])
        found = re.search(r'"IOPlatformUUID"\s*=\s*"([^"]+)"', hardware)
        if not found:
            raise ValueError("native host identity unavailable")
        identity = found.group(1)
        choices = []
        for line in command(["mount"]).splitlines():
            match = re.fullmatch(r"(.+) on (.+) \(([^)]+)\)", line)
            if match and is_relative_to(shared.resolve(), Path(match.group(2))):
                options = match.group(3).split(", ")
                choices.append({"target": match.group(2), "source": match.group(1), "fstype": options[0], "options": ",".join(options[1:])})
        mount = max(choices, key=lambda entry: len(entry["target"])) if choices else None
    else:
        raise ValueError("qualification supports native Linux and macOS hosts")
    if mount and mount["fstype"].startswith("nfs"):
        details = mount["options"]
        if system == "Darwin":
            details = command(["nfsstat", "-m"])
            sections = re.split(r"(?m)^(?=/.* from )", details)
            matching = [section for section in sections if section.startswith(mount["target"] + " from ")]
            details = matching[0] if len(matching) == 1 else ""
            if "-- Current mount parameters:" in details:
                details = details.split("-- Current mount parameters:", 1)[1]
        versions = set(re.findall(r"(?:nfsvers|vers)=([0-9]+(?:\.[0-9]+)?)", details))
        version = next(iter(versions)) if len(versions) == 1 else None
        mount["protocol"] = "4.0" if version == "4" else version
        mount["protocol_evidence"] = details[:16384]
    binaries = {}
    for name in ("lsm_admin", "lsm_db", "lsm_worker", "lsm_loadgen", "lsm_gpu_check", "lsm_telemetry"):
        path = project / "build" / "dev" / name
        binaries[name] = path.is_file() and os.access(path, os.X_OK)
    capabilities = None
    if binaries["lsm_admin"]:
        capabilities = json.loads(command([str(project / "build" / "dev" / "lsm_admin"), "capabilities"]))
    return {"schema_version": 1, "hostname": socket.gethostname(), "system": system, "machine": platform.machine(),
            "machine_id_sha256": hashlib.sha256(identity.encode()).hexdigest(), "shared_path": str(shared.resolve()),
            "mount": mount, "binaries": binaries, "capabilities": capabilities,
            "gpu_execution": "UNVERIFIED", "power_loss_durability": "UNVERIFIED"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["doctor", "reserve", "publish", "verify", "rename", "verify-renamed", "lock", "locked", "unlocked", "read", "write-spec", "stop"])
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--project", type=Path)
    parser.add_argument("--id")
    parser.add_argument("--name")
    parser.add_argument("--json")
    parser.add_argument("--pid", type=int)
    arguments = parser.parse_args()
    try:
        if arguments.action == "doctor":
            if arguments.project is None:
                raise ValueError("doctor requires project path")
            result = host_report(arguments.project, arguments.root)
        elif arguments.action == "reserve":
            result = reserve(arguments.root, arguments.id)
        elif arguments.action == "read":
            name = arguments.name
            if not isinstance(name, str) or not re.fullmatch(r"[a-zA-Z0-9_.-]{1,80}", name) or name in (".", ".."):
                raise ValueError("invalid read name")
            path = arguments.root / name
            if not path.exists():
                return 2
            if path.is_symlink() or path.stat().st_size > 1 << 20:
                raise ValueError("invalid probe read")
            result = json.loads(path.read_text())
        elif arguments.action == "write-spec":
            owned(arguments.root)
            value = json.loads(arguments.json)
            if (arguments.root / "dataset-spec.json").exists():
                raise ValueError("spec destination already exists")
            durable(arguments.root / "dataset-spec.json", json.dumps(value).encode())
            result = {"status": "ok"}
        elif arguments.action == "stop":
            owned(arguments.root)
            pid = arguments.pid
            if pid is None or pid <= 1:
                raise ValueError("invalid process id")
            process = command(["ps", "-p", str(pid), "-o", "command="])
            if str(arguments.root.resolve()) not in process or "lsm_" not in process:
                raise ValueError("refusing to signal a process outside the owned qualification root")
            os.kill(pid, signal.SIGTERM)
            result = {"status": "termination_requested", "pid": pid}
        else:
            result = filesystem_probe(arguments.root, arguments.action)
        print(json.dumps(result), flush=True)
        return 0
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        print(json.dumps({"status": "error", "detail": str(error)}), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())