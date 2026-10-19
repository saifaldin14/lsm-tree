"""Tests for the experiment-harness options: closed-loop plans, inserts, CPU control, trial storage, engine metrics."""

import importlib.util
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]


def load_module(name: str):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / f"{name}.py")
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


experiments = load_module("experiments")
analysis = load_module("analysis")
BINARIES = experiments.BINARIES


def binaries_available(*names: str) -> bool:
    return all((BINARIES / name).is_file() for name in names)


def closed_loop_config(base: dict, **overrides) -> dict:
    config = {key: value for key, value in base.items()
              if key not in ("rates", "warmup_seconds", "measurement_seconds", "queue_capacity")}
    config.update(load_mode="closed_loop", duration_seconds=2, warmup_duration_seconds=1)
    config.update(overrides)
    return config


class PlanCompatibilityTests(unittest.TestCase):
    def setUp(self):
        self.config = json.loads((ROOT / "configs" / "local-experiment.json").read_text())
        self.dataset = json.loads((ROOT / "configs" / "local-smoke.json").read_text())

    def test_existing_plan_checksums_are_unchanged(self):
        self.assertEqual(experiments.make_plan(self.config, self.dataset)["plan_sha256"],
                         "37ed7e0a3724a53a95c7e3e6deaebdd80899ea596065a71dcb28bd34263f0e79")
        self.assertEqual(experiments.make_plan(self.config, self.dataset, resource_accounting=False)["plan_sha256"],
                         "7c6df1ff610d4d4a47fcb6d47dc28747392cfe9234700d4e48d3e3258f729cbb")

    def test_explicit_open_loop_and_optional_fields(self):
        baseline = experiments.make_plan(self.config, self.dataset)
        explicit = experiments.make_plan(dict(self.config, load_mode="open_loop"), self.dataset)
        self.assertEqual(explicit["trials"], baseline["trials"])
        options = dict(self.config, insert_percent=25, core_budget=2, numa_node=0, background_load_percent=50,
                       trial_root="/dev/shm/$USER/lsm-trials")
        plan = experiments.make_plan(options, self.dataset)
        self.assertEqual(plan["trials"], baseline["trials"])
        self.assertNotEqual(plan["plan_sha256"], baseline["plan_sha256"])

    def test_optional_fields_are_validated(self):
        invalid = [dict(insert_percent=101), dict(insert_percent=True), dict(core_budget=0),
                   dict(background_load_percent=10), dict(numa_node=1), dict(core_budget=2, background_load_percent=101),
                   dict(trial_root="relative/path"), dict(trial_root=""), dict(load_mode="saturate"),
                   dict(duration_seconds=5), dict(warmup_duration_seconds=1)]
        for overrides in invalid:
            with self.subTest(overrides=overrides), self.assertRaises(ValueError):
                experiments.make_plan(dict(self.config, **overrides), self.dataset)
        experiments.make_plan(dict(self.config, background_load_percent=0), self.dataset)


class ClosedLoopPlanTests(unittest.TestCase):
    def setUp(self):
        self.base = json.loads((ROOT / "configs" / "local-experiment.json").read_text())
        self.dataset = json.loads((ROOT / "configs" / "local-smoke.json").read_text())

    def test_closed_loop_plan_is_deterministic_and_budgeted(self):
        config = closed_loop_config(self.base)
        plan = experiments.make_plan(config, self.dataset)
        self.assertEqual(plan, experiments.make_plan(config, self.dataset))
        self.assertEqual(len(plan["trials"]), config["repetitions"] * len(config["modes"]))
        for repetition in range(config["repetitions"]):
            trials = [trial for trial in plan["trials"] if trial["repetition"] == repetition]
            self.assertEqual({trial["mode"] for trial in trials}, set(config["modes"]))
            self.assertEqual(len({trial["seed"] for trial in trials}), 1)
        for trial in plan["trials"]:
            self.assertEqual(trial["load_mode"], "closed_loop")
            self.assertEqual(trial["clients"], config["threads"])
            self.assertNotIn("rate", trial)
        self.assertLessEqual(plan["logical_write_upper_bound"], config["max_logical_write_bytes"])
        frozen = Path(tempfile.mkdtemp()) / "plan.json"
        try:
            experiments.write_json(frozen, plan)
            self.assertEqual(experiments.load_plan(frozen), plan)
        finally:
            frozen.unlink()
            frozen.parent.rmdir()

    def test_closed_loop_rejects_open_loop_fields_and_tiny_budgets(self):
        for field in ("rates", "warmup_seconds", "measurement_seconds", "queue_capacity"):
            with self.subTest(field=field), self.assertRaises(ValueError):
                experiments.make_plan(closed_loop_config(self.base, **{field: self.base[field]}), self.dataset)
        config = closed_loop_config(self.base)
        del config["duration_seconds"]
        with self.assertRaises(ValueError):
            experiments.make_plan(config, self.dataset)
        with self.assertRaises(ValueError):
            experiments.make_plan(closed_loop_config(self.base, max_logical_write_bytes=544000), self.dataset)
        with self.assertRaises(ValueError):
            experiments.make_plan(closed_loop_config(self.base, threads=1001), dict(self.dataset, dataset=dict(self.dataset["dataset"], records=1000)))
        with self.assertRaises(ValueError):
            experiments.make_plan(closed_loop_config(self.base, max_wall_seconds=10), self.dataset)


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def fake_sys(root: Path, online: str = "0-15", siblings: bool = True) -> Path:
    """Two NUMA nodes with eight physical cores and two hardware threads per core (cpu N and N+8)."""
    write(root / "devices/system/node/node0/cpulist", "0-3,8-11\n")
    write(root / "devices/system/node/node1/cpulist", "4-7,12-15\n")
    write(root / "devices/system/cpu/online", online + "\n")
    for cpu in range(16):
        core = cpu % 8
        topology = root / f"devices/system/cpu/cpu{cpu}/topology"
        if siblings:
            write(topology / "thread_siblings_list", f"{core},{core + 8}\n")
        write(topology / "core_id", f"{core % 4}\n")
        write(topology / "physical_package_id", f"{core // 4}\n")
    return root


class CpuSelectionTests(unittest.TestCase):
    def test_one_thread_per_physical_core_on_the_node(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = fake_sys(Path(temporary))
            self.assertEqual(experiments.physical_cores(0, root), [0, 1, 2, 3])
            self.assertEqual(experiments.physical_cores(1, root), [4, 5, 6, 7])
            self.assertEqual(experiments.select_cores(2, 0, root), [0, 1])
            self.assertEqual(experiments.select_cores(4, 1, root), [4, 5, 6, 7])
            with self.assertRaises(ValueError):
                experiments.select_cores(5, 0, root)
            with self.assertRaises(ValueError):
                experiments.select_cores(1, 2, root)

    def test_offline_threads_and_topology_fallback(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = fake_sys(Path(temporary), online="0-2,4-15")
            self.assertEqual(experiments.physical_cores(0, root), [0, 1, 2, 11])
        with tempfile.TemporaryDirectory() as temporary:
            root = fake_sys(Path(temporary), siblings=False)
            self.assertEqual(experiments.physical_cores(0, root), [0, 1, 2, 3])
            self.assertEqual(experiments.core_threads(0, root)[1], [1, 9])

    def test_cpu_list_parser(self):
        self.assertEqual(experiments.parse_cpu_list("0-2,5,7-8\n"), [0, 1, 2, 5, 7, 8])
        for text in ("0-", "a", "3-1", "1,-2"):
            with self.subTest(text=text), self.assertRaises(ValueError):
                experiments.parse_cpu_list(text)

    def test_placement_prefix_and_load_cpus(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = fake_sys(Path(temporary))
            with mock.patch.object(experiments.shutil, "which", return_value="/usr/bin/numactl"):
                self.assertIsNone(experiments.cpu_placement({}, root))
                placement = experiments.cpu_placement({"core_budget": 2, "background_load_percent": 30}, root)
                self.assertEqual(placement["cpus"], [0, 1])
                self.assertEqual(placement["prefix"], ["/usr/bin/numactl", "--physcpubind=0,1", "--membind=0"])
                self.assertEqual(placement["load_cpus"], [2, 3, 4, 5, 6, 7, 10, 11, 12, 13, 14, 15])
                self.assertEqual(placement["background_load_percent"], 30)
                other = experiments.cpu_placement({"core_budget": 1, "numa_node": 1}, root)
                self.assertEqual(other["prefix"], ["/usr/bin/numactl", "--physcpubind=4", "--membind=1"])
            with mock.patch.object(experiments.shutil, "which", return_value=None):
                with self.assertRaises(ValueError):
                    experiments.cpu_placement({"core_budget": 1}, root)


def fake_proc(root: Path, processes: dict, total: int, loadavg: str = "1.50 1.00 0.50 2/300 99") -> Path:
    write(root / "stat", f"cpu  {total} 0 0 0 0 0 0 0 0 0\ncpu0 {total} 0 0 0 0 0 0 0 0 0\n")
    write(root / "loadavg", loadavg + "\n")
    for pid, (ppid, ticks, name) in processes.items():
        fields = ["S", str(ppid)] + ["0"] * 9 + [str(ticks), "0"] + ["0"] * 10
        write(root / str(pid) / "stat", f"{pid} ({name}) " + " ".join(fields) + "\n")
    return root


class HostLoadTests(unittest.TestCase):
    def test_foreign_usage_excludes_runner_process_tree(self):
        runner = 100
        with tempfile.TemporaryDirectory() as first, tempfile.TemporaryDirectory() as second:
            before = experiments.proc_snapshot(fake_proc(Path(first), {
                1: (0, 10, "init"), runner: (1, 50, "python"), 101: (runner, 0, "lsm db"), 200: (1, 100, "stress")}, 1000))
            after = experiments.proc_snapshot(fake_proc(Path(second), {
                1: (0, 10, "init"), runner: (1, 90, "python"), 101: (runner, 300, "lsm db"), 102: (101, 50, "child"),
                200: (1, 300, "stress"), 201: (1, 100, "new")}, 2000))
            report = experiments.foreign_cpu_usage(before, after, runner)
            self.assertEqual(report["foreign_ticks"], 300)
            self.assertEqual(report["host_jiffies"], 1000)
            self.assertAlmostEqual(report["foreign_cpu_percent"], 30.0)
            self.assertEqual([entry["pid"] for entry in report["top_foreign_processes"]], [200, 201])

    def test_host_load_reads_loadavg_and_degrades_without_proc(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = fake_proc(Path(temporary), {os.getpid(): (1, 5, "python")}, 500)
            report = experiments.host_load(0.0, root)
            self.assertEqual(report["status"], "ok")
            self.assertEqual(report["loadavg_1m"], 1.5)
            self.assertEqual(report["foreign_cpu_percent"], None)
        with tempfile.TemporaryDirectory() as temporary:
            report = experiments.host_load(0.0, Path(temporary))
            self.assertEqual(report["status"], "unavailable")


class EngineMetricsTests(unittest.TestCase):
    def test_window_deltas_and_write_amplification(self):
        def sample(user, flush, compaction, stall, running):
            return {"statistics": {"io": {"user_bytes_written": user, "flush_bytes_written": flush,
                                          "compaction_bytes_read": compaction, "compaction_bytes_written": compaction},
                                   "write_stalls": {"stall_micros": stall, "cf": {"total-stops": 1, "total-delays": stall // 10}},
                                   "properties": {"rocksdb.num-running-compactions": running}}}
        samples = [sample(1000, 500, 0, 0, 0), sample(3000, 1500, 2000, 100, 2)]
        summary = {"io": {"user_bytes_written": 5000, "write_amplification": 2.5}, "write_stalls": {"stall_micros": 7}}
        metrics = analysis.engine_metrics(samples, summary)
        self.assertEqual(metrics["window_user_bytes_written"], 2000)
        self.assertEqual(metrics["window_write_amplification"], 1.5)
        self.assertEqual(metrics["window_write_stall_micros"], 100)
        self.assertEqual(metrics["window_write_stops"], 0)
        self.assertEqual(metrics["window_write_delays"], 10)
        self.assertEqual(metrics["total_write_amplification"], 2.5)
        self.assertEqual(metrics["total_write_stall_micros"], 7)
        self.assertEqual(metrics["window_mean_running_compactions"], 1)
        legacy = analysis.engine_metrics([{"statistics": {"properties": {}}}] * 3)
        self.assertTrue(all(value is None for value in legacy.values()))


@unittest.skipUnless(binaries_available("lsm_burn"), "lsm_burn is not built")
class BurnerTests(unittest.TestCase):
    def test_duty_cycle_burner_starts_and_stops_cleanly(self):
        with tempfile.TemporaryDirectory() as temporary:
            ready = Path(temporary) / "burn.ready"
            process = subprocess.Popen([str(BINARIES / "lsm_burn"), "--cpus", "0", "--percent", "30", "--period-ms", "10",
                                        "--ready-file", str(ready)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                deadline = time.monotonic() + 10
                while not ready.exists() and time.monotonic() < deadline:
                    time.sleep(0.02)
                report = json.loads(ready.read_text())
                self.assertEqual(report["state"], "ready")
                self.assertEqual(report["pid"], process.pid)
                self.assertEqual(report["cpus"], [0])
                expected = "pinned" if sys.platform.startswith("linux") else "unsupported_or_failed"
                self.assertEqual(report["affinity"], expected)
                time.sleep(0.3)
                process.send_signal(signal.SIGTERM)
                stdout, _ = process.communicate(timeout=10)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
            self.assertEqual(process.returncode, 0)
            summary = json.loads(stdout)
            self.assertEqual(summary["state"], "stopped")
            self.assertGreater(summary["busy_ns_total"], 0)
            self.assertLess(summary["busy_ns_total"], summary["wall_ns"] * 0.6)

    def test_rejects_invalid_arguments(self):
        for arguments in (["--cpus", "", "--percent", "10"], ["--cpus", "0", "--percent", "0"],
                          ["--cpus", "0", "--percent", "10", "--extra", "1"]):
            with self.subTest(arguments=arguments):
                completed = subprocess.run([str(BINARIES / "lsm_burn"), *arguments], capture_output=True, timeout=10)
                self.assertEqual(completed.returncode, 1)


@unittest.skipUnless(binaries_available("lsm_admin", "lsm_db", "lsm_worker", "lsm_loadgen", "lsm_telemetry"),
                     "native binaries are not built")
class ClosedLoopRunnerTests(unittest.TestCase):
    def test_real_closed_loop_run_with_inserts_and_trial_root(self):
        base = json.loads((ROOT / "configs" / "local-experiment.json").read_text())
        dataset = json.loads((ROOT / "configs" / "local-smoke.json").read_text())
        config = closed_loop_config(base, repetitions=1, threads=2, insert_percent=30, max_wall_seconds=200)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            config["trial_root"] = str(root / "trial-root")
            plan = experiments.make_plan(config, dataset)
            experiments.write_json(root / "plan.json", plan)
            scratch = root / "scratch"
            scratch.mkdir()
            with mock.patch.dict(os.environ, {"LSM_LAB_SCRATCH_ROOT": str(scratch), "LSM_LAB_PRIMARY_NUMA_NODE": ""}):
                result = experiments.run_plan(root / "plan.json", root / "run")
            self.assertEqual(result["state"], "complete")
            self.assertEqual(len(result["trials"]), 2)
            self.assertTrue(all(trial["valid"] for trial in result["trials"]), result)
            self.assertEqual(list(scratch.iterdir()), [])
            self.assertEqual(list((root / "trial-root").iterdir()), [])
            self.assertEqual(sorted(path.name for path in (root / "run" / "shared").iterdir()),
                             [".lsm-shared.json", ".lsm-staging", "baseline"])
            for trial in result["trials"]:
                self.assertTrue(trial["trial_clone_removed"])
                self.assertIn("loadavg_1m", trial["host_load_before"])
                self.assertIn("loadavg_1m", trial["host_load_after"])
                self.assertEqual(trial["verification"]["inserted_keys_verified"],
                                 json.loads((root / "run" / trial["result"]).read_text())["inserted_keys"])
                directory = root / "run" / "trials" / trial["id"]
                self.assertTrue((directory / "dataset-metadata" / "run.json").is_file())
                load = json.loads((directory / "load" / "result.json").read_text())
                self.assertEqual(load["load_mode"], "closed_loop")
                self.assertGreater(load["throughput"]["ops_per_second"], 0)
                summary = json.loads((directory / "primary-summary.json").read_text())
                self.assertIn("write_stalls", summary)
                self.assertGreater(summary["io"]["user_bytes_written"], 0)
            self.assertEqual(experiments.verify_bundle(root / "run")["status"], "ok")
            report = analysis.analyze_run(root / "run", root / "analysis", plots=True)
            self.assertEqual(report["load_mode"], "closed_loop")
            self.assertEqual(len(report["saturation"]), 2)
            self.assertTrue(all(entry["ops_per_second"] > 0 for entry in report["saturation"]))
            self.assertGreater((root / "analysis" / "saturation.png").stat().st_size, 1000)
            with (root / "analysis" / "trials.csv").open() as rows:
                header = rows.readline()
            self.assertIn("window_write_amplification", header)


if __name__ == "__main__":
    unittest.main()
