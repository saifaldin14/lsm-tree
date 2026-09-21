import importlib.util
from pathlib import Path
import sys
import unittest


module_spec = importlib.util.spec_from_file_location(
    "labctl", Path(__file__).resolve().parents[1] / "tools" / "labctl.py",
)
assert module_spec is not None and module_spec.loader is not None
labctl = importlib.util.module_from_spec(module_spec)
module_spec.loader.exec_module(labctl)


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


if __name__ == "__main__":
    unittest.main()