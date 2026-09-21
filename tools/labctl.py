#!/usr/bin/env python3
"""Unmeasured environment and experiment administration."""

import argparse
import datetime
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]


def probe_command(arguments: list[str], timeout: float = 10) -> dict:
    executable = shutil.which(arguments[0])
    if executable is None:
        return {"status": "UNAVAILABLE", "command": arguments, "exit_code": None}
    try:
        completed = subprocess.run(
            [executable, *arguments[1:]], capture_output=True, text=True,
            timeout=timeout, check=False,
        )
        return {
            "status": "PASS" if completed.returncode == 0 else "FAIL",
            "command": arguments,
            "exit_code": completed.returncode,
            "output": (completed.stdout + completed.stderr).strip()[:16384],
        }
    except (OSError, subprocess.TimeoutExpired) as error:
        return {"status": "FAIL", "command": arguments, "exit_code": None,
                "detail": str(error)}


def doctor(directory: Path) -> dict:
    directory = directory.resolve(strict=True)
    disk = shutil.disk_usage(directory)
    checks = {
        "compiler": probe_command(["c++", "--version"]),
        "cmake": probe_command(["cmake", "--version"]),
        "git": probe_command(["git", "--version"]),
        "ssh": probe_command(["ssh", "-V"]),
        "cuda_compiler": probe_command(["nvcc", "--version"]),
        "cuda_devices": probe_command([
            "nvidia-smi", "--query-gpu=name,uuid,driver_version,memory.total",
            "--format=csv,noheader,nounits",
        ]),
    }
    if platform.system() == "Darwin":
        checks["metal_compiler"] = probe_command(["xcrun", "--find", "metal"])
        checks["display_devices"] = probe_command(
            ["system_profiler", "SPDisplaysDataType", "-json"], timeout=30,
        )
        checks["physical_memory"] = probe_command(["sysctl", "-n", "hw.memsize"])
    else:
        checks["metal_compiler"] = {"status": "UNAVAILABLE", "detail": "requires native macOS"}
        memory = Path("/proc/meminfo")
        checks["physical_memory"] = {
            "status": "PASS" if memory.is_file() else "UNAVAILABLE",
            "output": memory.read_text() if memory.is_file() else None,
        }
    for name in ("lsm_admin", "lsm_db", "lsm_worker", "lsm_loadgen"):
        binary = ROOT / "build" / "dev" / name
        checks[name] = {
            "status": "PASS" if binary.is_file() and os.access(binary, os.X_OK) else "UNAVAILABLE",
            "path": str(binary), "scope": "executable presence only",
        }
    return {
        "schema_version": 1,
        "recorded_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "host": platform.node(), "os": platform.platform(), "machine": platform.machine(),
        "logical_cpus": os.cpu_count(), "python": sys.version, "python_executable": sys.executable,
        "disk": {"path": str(directory), "total_bytes": disk.total, "free_bytes": disk.free},
        "checks": checks,
        "qualification": {
            "cuda_execution": "UNVERIFIED", "metal_execution": "UNVERIFIED",
            "cross_host_storage": "UNVERIFIED", "performance": "UNVERIFIED",
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    doctor_parser = commands.add_parser("doctor", help="read-only local environment report")
    doctor_parser.add_argument("--directory", type=Path, default=ROOT)
    arguments = parser.parse_args()
    try:
        print(json.dumps(doctor(arguments.directory), indent=2))
        return 0
    except (OSError, ValueError) as error:
        print(json.dumps({"status": "error", "detail": str(error)}), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())