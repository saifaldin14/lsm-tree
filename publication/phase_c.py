#!/usr/bin/env python3
"""Run the registered profiling, write-amplification and baseline checks after cost32."""

from __future__ import annotations

import argparse
import ctypes
import errno
import fcntl
import hashlib
import json
import os
import platform
import select
import signal
import subprocess
import sys
import time
from pathlib import Path

from system_campaign import GPU_ENV, physical_cores

ROOT = Path(__file__).resolve().parents[1]


def write_once(path, document):
    if path.exists():
        if json.loads(path.read_text()) != document:
            raise RuntimeError(f"refusing to change an existing protocol: {path}")
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("x") as output:
        json.dump(document, output, indent=2)
        output.write("\n")


def verified(result, records):
    check = result.get("verification") or {}
    return (check.get("keys") == records and check.get("deleted_keys", 0) == 0
            and check.get("expired_keys", 0) == 0)


def campaign_complete(directory):
    schedule = json.loads((directory / "schedule.json").read_text())
    rows = [json.loads(line) for line in (directory / "results.jsonl").read_text().splitlines() if line.strip()]
    indices = [row["index"] for row in rows]
    if len(indices) != len(schedule) or sorted(indices) != list(range(len(schedule))):
        raise RuntimeError(f"incomplete or duplicate campaign records: {directory}")
    for row in rows:
        if row.get("trial") != schedule[row["index"]]:
            raise RuntimeError(f"trial differs from its schedule: {directory}")
        if row.get("exit_code") != 0 or not verified(row.get("result", {}), row["flags"]["records"]):
            raise RuntimeError(f"failed or unverified trial {row['index']}: {directory}")
    return rows


def wait_for_campaign(pid, directory):
    proc = Path(f"/proc/{pid}")
    if not proc.exists():
        return campaign_complete(directory)
    state = (proc / "stat").read_text().rsplit(") ", 1)[1].split()[0]
    if state == "Z":
        return campaign_complete(directory)
    command = (proc / "cmdline").read_bytes().replace(b"\0", b" ").decode()
    if proc.stat().st_uid != os.getuid() or "system_campaign.py" not in command or f"{directory} " not in command:
        raise RuntimeError(f"PID {pid} is not the expected campaign")
    if hasattr(os, "pidfd_open"):
        try:
            fd = os.pidfd_open(pid)
        except ProcessLookupError:
            return campaign_complete(directory)
    elif platform.machine() == "x86_64":
        libc = ctypes.CDLL(None, use_errno=True)
        fd = libc.syscall(434, pid, 0)
        if fd < 0:
            error = ctypes.get_errno()
            if error == errno.ESRCH:
                return campaign_complete(directory)
            raise OSError(error, os.strerror(error))
    else:
        raise RuntimeError("event-based process waiting is unavailable on this host")
    try:
        poller = select.poll()
        poller.register(fd, select.POLLIN)
        poller.poll()
    finally:
        os.close(fd)
    return campaign_complete(directory)


def assert_idle():
    for proc in Path("/proc").glob("[0-9]*"):
        try:
            if proc.stat().st_uid != os.getuid():
                continue
            args = (proc / "cmdline").read_bytes().split(b"\0")
            if any(Path(arg.decode()).name in {"lsm_system_bench", "system_campaign.py"} for arg in args if arg):
                raise RuntimeError(f"another benchmark is active: PID {proc.name}")
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            continue


def build_grids(headline, golden):
    tuned = {t["design"]: t for t in headline["trials"] if t["cores"] == 8}
    common = {"records": 50000000, "read-percent": 0, "warmup-seconds": 30,
              "duration-seconds": 120, "verify": True}

    def trial(design, extra, **metadata):
        return {"design": design, "mode": tuned[design]["mode"], "cores": 8, "writes": "batched",
                "flags": {**tuned[design]["flags"], **extra}, **metadata}

    writes = []
    for trigger in (16, 24):
        flags = {"l0-trigger": trigger, "l0-slowdown": 2 * trigger, "l0-stop": 3 * trigger,
                 "max-bytes-for-level-base": trigger * 64 * 2**20}
        writes.extend(trial(d, flags, l0_trigger=trigger) for d in ("native", "full"))
    baselines = [trial("native", {"compaction-style": "universal"}, baseline="universal",
                       golden=str(golden / "universal")),
                 trial("native", {"blob-min-bytes": 64}, baseline="blobdb", golden=str(golden / "blobdb")),
                 trial("full", {}, baseline="ours"),
                 trial("full", {"l0-trigger": 12, "max-bytes-for-level-base": 12 * 64 * 2**20},
                       baseline="ours-trigger12")]
    smoke = trial("full", {"blob-min-bytes": 64}, baseline="ours-blobdb", golden=str(golden / "blobdb"))
    return {"w": {"common": common, "trials": writes}, "b": {"common": common, "trials": baselines},
            "blob-smoke": {"common": {**common, "warmup-seconds": 5, "duration-seconds": 20}, "trials": [smoke]},
            "profile": {"common": {**common, "duration-seconds": 60, "stats-all": True},
                        "trials": [trial(d, {}) for d in ("native", "full")]}}


def benchmark_command(binary, directory, trial, common):
    cpus, nodes = physical_cores(0, trial["cores"])
    flags = {**common, **trial["flags"]}
    args = ["numactl", f"--physcpubind={','.join(map(str, cpus))}", f"--membind={','.join(map(str, nodes))}",
            str(binary), "--phase", "run", "--mode", trial["mode"], "--dir", str(directory)]
    for name, value in flags.items():
        args.extend([f"--{name}", str(value).lower() if isinstance(value, bool) else str(value)])
    return args


def run_checked(command, *, timeout=3600, **kwargs):
    print("RUN", " ".join(map(str, command)), flush=True)
    process = subprocess.Popen(command, start_new_session=True, **kwargs)
    try:
        code = process.wait(timeout=timeout)
        if code:
            raise subprocess.CalledProcessError(code, command)
    finally:
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()


def run_campaign(args, name, grid, blocks, seed):
    directory = args.work / name
    command = [sys.executable, str(ROOT / "publication/system_campaign.py"), str(directory),
               "--golden", str(args.golden), "--grid", str(grid), "--blocks", str(blocks), "--seed", str(seed),
               "--scratch", str(args.scratch), "--idle-seconds", "0"]
    if directory.exists():
        if not (directory / "schedule.json").exists():
            raise RuntimeError(f"output exists without a schedule: {directory}")
        rows = [json.loads(line) for line in (directory / "results.jsonl").read_text().splitlines() if line.strip()]
        if any(row.get("exit_code") != 0 for row in rows):
            raise RuntimeError(f"failed trials need an explicit retry decision: {directory}")
        schedule = json.loads((directory / "schedule.json").read_text())
        if len(rows) == len(schedule):
            campaign_complete(directory)
            return directory
        command.append("--resume")
    assert_idle()
    run_checked(command, env={**os.environ, "LSM_BUILD_DIR": str(args.build)}, timeout=7200)
    campaign_complete(directory)
    return directory


def run_profiles(args, grid):
    root = args.work / "profile-checked"
    for trial in grid["trials"]:
        directory = root / trial["design"]
        if (directory / "complete.json").exists():
            continue
        directory.mkdir(parents=True, exist_ok=False)
        database = args.scratch / f"profile-{trial['design']}"
        if database.exists():
            raise RuntimeError(f"scratch database already exists: {database}")
        assert_idle()
        run_checked(["cp", "-a", "--reflink=auto", str(args.golden), str(database)])
        bench = benchmark_command(args.build / "lsm_system_bench", database, trial, grid["common"])
        command = ["perf", "record", "-e", "cpu-clock:u", "-F", "99", "--call-graph", "dwarf,8192",
                   "--timestamp", "--clockid", "mono", "-o", str(directory / "oncpu.data"), "--", *bench]
        environment = {**os.environ, **GPU_ENV}
        offset = time.monotonic() - time.time()
        write_once(directory / "protocol.json", {"command": command, "environment": GPU_ENV,
                   "statistics_scope": "database open through catch-up, before verification",
                   "off_cpu": "unavailable: sched:sched_switch tracepoint not accessible"})
        with (directory / "bench.json").open("x") as output, (directory / "perf.log").open("x") as errors:
            run_checked(command, env=environment, stdout=output, stderr=errors, timeout=900)
        result = json.loads((directory / "bench.json").read_text())
        if not verified(result, grid["common"]["records"]):
            raise RuntimeError(f"profile verification failed: {directory}")
        drift = abs(offset - (time.monotonic() - time.time()))
        if drift > 0.1:
            raise RuntimeError(f"clock offset changed during profiling: {drift}")
        window = result["window"]
        interval = f"{window['start_unix'] + offset:.6f},{window['end_unix'] + offset:.6f}"
        report = ["perf", "report", "-i", str(directory / "oncpu.data"), "--stdio", "--no-children",
                  "--time", interval, "--sort", "comm,dso,symbol", "--percent-limit", "0.5"]
        with (directory / "oncpu.txt").open("x") as output:
            run_checked(report, stdout=output, env={**environment, "PAGER": "cat"}, timeout=120)
        write_once(directory / "complete.json", {"verified": True, "window_perf_seconds": interval,
                   "clock_offset_drift_seconds": drift, "raw_profile_retained": True})


def preload_baselines(args, root):
    for name, extra in (("universal", ["--compaction-style", "universal"]), ("blobdb", ["--blob-min-bytes", "64"])):
        directory = root / name
        meta = root / f"{name}.json"
        if meta.exists():
            if not verified(json.loads(meta.read_text()), 50000000) or not directory.exists():
                raise RuntimeError(f"invalid baseline preload: {meta}")
            continue
        if directory.exists():
            raise RuntimeError(f"partial preload exists: {directory}")
        root.mkdir(parents=True, exist_ok=True)
        assert_idle()
        command = ["numactl", "--physcpubind=0-7", "--membind=0", str(args.build / "lsm_system_bench"),
                   "--phase", "preload", "--dir", str(directory), "--records", "50000000", "--threads", "8",
                   "--allow-fallocate", "false", "--verify", "true", *extra]
        with meta.open("x") as output, (root / f"{name}.err").open("x") as errors:
            run_checked(command, stdout=output, stderr=errors, timeout=3600)
        if not verified(json.loads(meta.read_text()), 50000000):
            raise RuntimeError(f"baseline preload failed verification: {meta}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--golden", type=Path, required=True)
    parser.add_argument("--scratch", type=Path, required=True)
    parser.add_argument("--wait-pid", type=int, required=True)
    parser.add_argument("--wait-directory", type=Path)
    parser.add_argument("--refresh-build", action="store_true")
    parser.add_argument("--resume-legacy", action="store_true")
    args = parser.parse_args()
    args.scratch.mkdir(parents=True, exist_ok=True)
    with (args.work / "phase-c.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        golden = args.scratch / "baseline-goldens"
        grids = build_grids(json.loads((ROOT / "configs/system/v3-headline.json").read_text()), golden)
        paths = {name: args.work / "phase-c-checked-grids" / f"{name}.json" for name in grids}
        for name, grid in grids.items():
            write_once(paths[name], grid)
        waiting = args.wait_directory or args.work / "cost32"
        print("WAIT", waiting, args.wait_pid, flush=True)
        wait_for_campaign(args.wait_pid, waiting)
        assert_idle()
        if args.refresh_build:
            cache = (args.build / "CMakeCache.txt").read_text().splitlines()
            cmake = next(line.split("=", 1)[1] for line in cache if line.startswith("CMAKE_COMMAND:INTERNAL="))
            run_checked([cmake, "-S", str(ROOT), "-B", str(args.build)])
            run_checked([cmake, "--build", str(args.build), "--target", "lsm_system_bench",
                         "lsm_db_options_tests", "--parallel", "8"])
            run_checked([str(args.build / "lsm_db_options_tests")], timeout=120)
        binary = args.build / "lsm_system_bench"
        write_once(args.work / "phase-c-build.json", {"binary": str(binary),
                   "sha256": hashlib.sha256(binary.read_bytes()).hexdigest(), "environment": GPU_ENV,
                   "script_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest()})
        print("START profile", flush=True)
        run_profiles(args, grids["profile"])
        run_campaign(args, "wa-w", paths["w"], 2, 114)
        preload_baselines(args, golden)
        try:
            smoke = run_campaign(args, "base-blob-smoke", paths["blob-smoke"], 1, 115)
            rows = campaign_complete(smoke)
            transform = rows[0]["result"].get("transform") or {}
            print("BlobDB smoke counters", json.dumps(transform), flush=True)
        except (RuntimeError, subprocess.CalledProcessError) as error:
            print("BlobDB offload smoke failed; retained, native baseline still runs:", error, flush=True)
        run_campaign(args, "base-b", paths["b"], 2, 115)
        print("DONE phase-c", flush=True)
        if args.resume_legacy:
            environment = {**os.environ, "LSM_BUILD_DIR": str(ROOT / "build/zyra-cuda"),
                           "SHM": str(args.golden.parent), "THEN_PRIOR": str(args.work / "prior-tuned")}
            run_checked(["bash", str(ROOT / "publication/shared_mps.sh"), str(args.work / "shared-mps")],
                        env=environment, timeout=28800)
    return 0


if __name__ == "__main__":
    def terminate(signum, frame):
        raise SystemExit(128 + signum)

    signal.signal(signal.SIGTERM, terminate)
    sys.exit(main())