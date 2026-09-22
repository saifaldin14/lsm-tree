import importlib.util
import datetime
import json
import plistlib
from pathlib import Path
import sys
import tempfile
import subprocess
import time
import unittest


module_spec = importlib.util.spec_from_file_location(
    "labctl", Path(__file__).resolve().parents[1] / "tools" / "labctl.py",
)
assert module_spec is not None and module_spec.loader is not None
labctl = importlib.util.module_from_spec(module_spec)
module_spec.loader.exec_module(labctl)

experiment_spec = importlib.util.spec_from_file_location(
    "experiments", Path(__file__).resolve().parents[1] / "tools" / "experiments.py",
)
assert experiment_spec is not None and experiment_spec.loader is not None
experiments = importlib.util.module_from_spec(experiment_spec)
experiment_spec.loader.exec_module(experiments)
sys.modules["experiments"] = experiments
analysis_spec = importlib.util.spec_from_file_location(
    "analysis", Path(__file__).resolve().parents[1] / "tools" / "analysis.py",
)
assert analysis_spec is not None and analysis_spec.loader is not None
analysis = importlib.util.module_from_spec(analysis_spec)
analysis_spec.loader.exec_module(analysis)
deployment_spec = importlib.util.spec_from_file_location(
    "deployment", Path(__file__).resolve().parents[1] / "tools" / "deployment.py",
)
assert deployment_spec is not None and deployment_spec.loader is not None
deployment = importlib.util.module_from_spec(deployment_spec)
deployment_spec.loader.exec_module(deployment)


class PlanTests(unittest.TestCase):
    def setUp(self):
        root = Path(__file__).resolve().parents[1]
        self.config = json.loads((root / "configs" / "local-experiment.json").read_text())
        self.dataset = json.loads((root / "configs" / "local-smoke.json").read_text())

    def test_repeatable_paired_schedule(self):
        plan = experiments.make_plan(self.config, self.dataset)
        self.assertEqual(plan, experiments.make_plan(self.config, self.dataset))
        self.assertEqual(len(plan["trials"]), 8)
        pairs = {}
        for trial in plan["trials"]:
            pairs.setdefault((trial["repetition"], trial["rate"]), []).append(trial)
        for trials in pairs.values():
            self.assertEqual({trial["mode"] for trial in trials}, {"native_cpu", "cpu_native"})
            self.assertEqual(len({trial["seed"] for trial in trials}), 1)

    def test_budget_and_unknown_fields_rejected(self):
        for field in ("max_wall_seconds", "max_logical_write_bytes", "max_artifact_bytes"):
            config = dict(self.config, **{field: 1})
            with self.assertRaises(ValueError):
                experiments.make_plan(config, self.dataset)
        with self.assertRaises(ValueError):
            experiments.make_plan(dict(self.config, typo=1), self.dataset)
        with self.assertRaises(ValueError):
            experiments.make_plan(dict(self.config, repetitions=True), self.dataset)

    def test_frozen_plan_and_bundle_tamper_detection(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = root / "plan.json"
            plan = experiments.make_plan(self.config, self.dataset)
            experiments.write_json(path, plan)
            self.assertEqual(experiments.load_plan(path), plan)
            experiments.seal_bundle(root)
            self.assertEqual(experiments.verify_bundle(root)["files_verified"], 1)
            path.write_text("{}")
            with self.assertRaises(ValueError):
                experiments.verify_bundle(root)

    def test_real_bounded_runner(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            config = dict(self.config, rates=[100], repetitions=1, warmup_seconds=0,
                          measurement_seconds=1, max_wall_seconds=120)
            plan = experiments.make_plan(config, self.dataset)
            experiments.write_json(root / "plan.json", plan)
            result = experiments.run_plan(root / "plan.json", root / "run")
            self.assertEqual(result["state"], "complete")
            self.assertEqual(len(result["trials"]), 2)
            self.assertTrue(all(trial["valid"] for trial in result["trials"]), result)
            for trial in result["trials"]:
                directory = root / "run" / "trials" / trial["id"]
                self.assertEqual(trial["host_collector_exit_code"], 0)
                paths = [directory / "host-resources.jsonl", directory / "primary-resources.jsonl",
                         directory / "load" / "resources.jsonl"]
                if trial["mode"] != "native_cpu":
                    paths.append(directory / "worker-resources.jsonl")
                for path in paths:
                    with path.open() as source:
                        records = [json.loads(line) for line in source]
                    self.assertEqual(records[0]["type"], "header")
                    self.assertEqual(records[-1]["status"], "complete")
                    self.assertEqual(records[0]["includes_host"], path.name == "host-resources.jsonl")
            self.assertEqual(experiments.verify_bundle(root / "run")["status"], "ok")
            report = analysis.analyze_run(root / "run", root / "analysis", plots=True)
            resources = json.loads((root / "analysis" / "resources.json").read_text())
            host_streams = [stream for stream in resources["streams"] if stream.get("includes_host") and stream["window"] == "collector_lifetime"]
            self.assertEqual(len(host_streams), 2)
            self.assertTrue(all(stream["status"] == "complete" for stream in resources["streams"]))
            self.assertTrue(all(stream["availability"]["network_bytes"]["status"] == "available" for stream in host_streams))
            self.assertGreater((root / "analysis" / "resources.csv").stat().st_size, 1000)
            self.assertGreater((root / "analysis" / "load-curves.png").stat().st_size, 1000)
            self.assertEqual(report["trial_count"], 2)
            self.assertFalse(report["cross_host_qualified"])
            self.assertTrue(all(curve["lower"] == 0 for curve in report["capacity_curves"]))
            self.assertTrue(all(comparison["qualifying_lower_bound_ratio"]["ci95"] is None for comparison in report["comparisons"]))
            rows = analysis.measured_rows(root / "run", result, plan)
            for row in rows:
                if row["mode"] != "native_cpu" and row["remote_committed"] == 0:
                    self.assertEqual(row["qualification"], "unverified")


class AnalysisTests(unittest.TestCase):
    def test_power_import_preserves_units_and_rejects_ambiguous_fields(self):
        document = {"timestamp": datetime.datetime(2026, 1, 1), "elapsed_ns": 2_000_000_000,
                    "processor": {"gpu_power": 2500, "cpu_energy": 9000}, "thermal_pressure": "Nominal"}
        parsed = analysis.powermetrics_sample(document)
        cpu, gpu = parsed["metrics"][:2]
        self.assertIsNone(cpu["power_watts"])
        self.assertEqual(gpu["power_watts"], 2.5)
        self.assertEqual(gpu["estimated_sample_energy_joules"], 5.0)
        self.assertFalse(gpu["hardware_energy_counter"])
        self.assertIsNone(parsed["temperature_celsius"])
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            capture = root / "capture.plist"
            capture.write_bytes(plistlib.dumps(document) + b"\0")
            report = analysis.import_powermetrics(capture, root / "imported")
            self.assertEqual(report["samples"], 1)
            self.assertEqual(report["available_power_readings"], 1)
            self.assertEqual(experiments.verify_bundle(root / "imported")["status"], "ok")

    def test_resource_intervals_reject_resets_gaps_and_foreign_clocks(self):
        host = {"host_id": "host", "boot_id": "boot"}
        header = {"type": "header", "schema_version": 1, "host": host, "role": "worker", "includes_host": False,
                  "interval_ms": 1000, "start_ns": 0}
        counter = {"name": "storage_read_bytes", "scope": "process", "entity": "42:start", "source": "OS",
                   "unit": "bytes", "kind": "counter", "status": "ok"}
        power = {"name": "power_watts", "scope": "host_gpu", "entity": "GPU-1", "source": "sensor",
                 "unit": "watts", "kind": "gauge", "status": "ok", "value": 10.0}
        records = [header]
        for sequence, (seconds, value) in enumerate(((0, 100), (1, 300), (2, 50), (8, 800), (9, 900))):
            records.append({"type": "sample", "sequence": sequence, "sample_start_ns": seconds * 10**9,
                            "sample_end_ns": seconds * 10**9, "errors": [], "metrics": [dict(counter, value=value), power]})
        records.append({"type": "footer", "status": "complete", "samples": 5, "end_ns": 10 * 10**9})
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "resources.jsonl"
            path.write_text("".join(json.dumps(record) + "\n" for record in records))
            report = analysis.resource_stream(path)
            traffic, energy = report["metrics"]
            self.assertEqual(report["availability"]["power"]["status"], "available")
            self.assertEqual(report["availability"]["hardware_energy"]["status"], "not_collected")
            self.assertEqual(traffic["observed_delta"], 300)
            self.assertEqual(traffic["invalid_intervals"], 2)
            self.assertEqual(traffic["coverage_fraction"], 0.2)
            self.assertEqual(energy["integrated_power_joules"], 30)
            window = {"host": host, "measurement_start_ns": 8 * 10**9, "measurement_end_ns": 10 * 10**9}
            self.assertEqual(analysis.resource_stream(path, window)["metrics"][0]["observed_delta"], 100)
            window["host"] = {"host_id": "elsewhere", "boot_id": "boot"}
            self.assertEqual(analysis.resource_stream(path, window)["status"], "unavailable")
            records[2]["metrics"] = [power]
            path.write_text("".join(json.dumps(record) + "\n" for record in records))
            missing = analysis.resource_stream(path)["metrics"][0]
            self.assertEqual(missing["unavailable_samples"], 1)
            self.assertEqual(missing["observed_delta"], 100)
            self.assertIn("metric absent from sample", missing["reasons"])

    def test_contiguous_capacity_and_censoring(self):
        rows = [{"rate": 100, "qualification": "pass"}, {"rate": 200, "qualification": "fail"},
                {"rate": 300, "qualification": "pass"}]
        self.assertEqual(analysis.capacity_bracket(rows), {"lower": 100, "upper": 200, "censoring": "interval"})
        self.assertEqual(analysis.capacity_bracket(rows[:1])["censoring"], "right")
        self.assertEqual(analysis.capacity_bracket(rows[1:])["lower"], 0)

    def test_paired_bootstrap_has_no_request_pseudoreplication(self):
        report = analysis.paired_bootstrap([(100, 200), (200, 400)], 1)
        self.assertAlmostEqual(report["estimate"], 2)
        self.assertEqual(report["ci95"], [2, 2])
        self.assertEqual(report["pairs"], 2)
        self.assertIsNone(analysis.paired_bootstrap([(100, 200)], 1)["ci95"])

    def test_unknown_is_not_a_failed_capacity_bound(self):
        rows = [{"rate": 100, "qualification": "pass"}, {"rate": 200, "qualification": "unverified"}]
        self.assertEqual(analysis.capacity_bracket(rows), {"lower": 100, "upper": None, "censoring": "unverified"})
        pairs = analysis.comparison_modes(["native_cpu", "cpu_native", "cpu_batched", "metal_batched"])
        self.assertIn(("cpu_native", "metal_batched"), pairs)
        self.assertIn(("cpu_batched", "metal_batched"), pairs)

    def test_work_counters_exclude_before_and_after_window(self):
        samples = [{"statistics": {"remote": {"committed": 4, "gpu_blocks": 200}}},
                   {"statistics": {"remote": {"committed": 5, "gpu_blocks": 232}}}]
        self.assertEqual(analysis.window_work(samples), {"committed": 1, "gpu_blocks": 32})
        self.assertEqual(analysis.window_work(samples[:1]), {"committed": 0, "gpu_blocks": 0})
        with self.assertRaises(ValueError):
            analysis.window_work(list(reversed(samples)))


class DoctorTests(unittest.TestCase):
    def test_real_process_success(self):
        report = labctl.probe_command([sys.executable, "-c", "print('verified')"])
        self.assertEqual(report["status"], "PASS")
        self.assertEqual(report["exit_code"], 0)
        self.assertEqual(report["output"], "verified")

    def test_real_process_failure(self):
        report = labctl.probe_command([sys.executable, "-c", "raise SystemExit(7)"])
        self.assertEqual(report["status"], "FAIL")
        self.assertEqual(report["exit_code"], 7)

    def test_unavailable_is_not_success(self):
        report = labctl.probe_command(["lsm-lab-command-that-does-not-exist"])
        self.assertEqual(report["status"], "UNAVAILABLE")
        self.assertIsNone(report["exit_code"])

    def test_timeout_is_reported(self):
        report = labctl.probe_command([sys.executable, "-c", "while True: pass"], timeout=0.05)
        self.assertEqual(report["status"], "FAIL")
        self.assertIsNone(report["exit_code"])


class DeploymentTests(unittest.TestCase):
    def test_plan_never_connects_and_execution_requires_approval(self):
        inventory = Path(__file__).resolve().parents[1] / "infra" / "inventory.example.json"
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            report = deployment.deployment_plan(inventory, root / "plan.json")
            self.assertFalse(report["executed"])
            self.assertEqual(set(report["preflight_commands"]), {"primary", "worker", "load", "storage"})
            self.assertEqual(set(report["resource_collection"]["hosts"]), {"primary", "worker", "load", "storage"})
            self.assertEqual(report["resource_collection"]["power_and_thermal_permissions"], "operator_managed_no_automatic_elevation")
            with self.assertRaises(ValueError):
                deployment.qualify(inventory, root / "rejected", apply=True)
            self.assertFalse((root / "rejected").exists())

    def test_rejects_option_injection_and_loopback_topology(self):
        source = Path(__file__).resolve().parents[1] / "infra" / "inventory.example.json"
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "inventory.json"
            for field, value in (("ssh", "-oProxyCommand=anything"), ("endpoint", "127.0.0.1:50051")):
                document = json.loads(source.read_text())
                document["hosts"]["primary"][field] = value
                path.write_text(json.dumps(document))
                with self.assertRaises(ValueError):
                    deployment.inventory(path)


class FilesystemProbeTests(unittest.TestCase):
    def test_real_files_lock_exclusion_and_rename(self):
        probe = Path(__file__).resolve().parents[1] / "tools" / "host_probe.py"
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)

            def invoke(action, directory, *extra):
                return subprocess.run([sys.executable, str(probe), action, "--root", str(directory), *extra],
                                      capture_output=True, text=True, check=True, timeout=10)

            invoke("reserve", root, "--id", "test-probe")
            directory = root / "test-probe"
            invoke("publish", directory)
            invoke("verify", directory)
            locker = subprocess.Popen([sys.executable, str(probe), "lock", "--root", str(directory)],
                                      stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                end = time.monotonic() + 5
                while not (directory / "lock.ready").exists() and time.monotonic() < end:
                    time.sleep(0.01)
                self.assertTrue((directory / "lock.ready").exists())
                invoke("locked", directory)
                invoke("rename", directory)
                invoke("verify-renamed", directory)
                locker.communicate(input="x", timeout=5)
                self.assertEqual(locker.returncode, 0)
                invoke("unlocked", directory)
                (directory / "renamed").write_bytes(b"corrupted")
                with self.assertRaises(subprocess.CalledProcessError):
                    invoke("verify-renamed", directory)
            finally:
                if locker.poll() is None:
                    locker.kill()
                    locker.communicate()


if __name__ == "__main__":
    unittest.main()