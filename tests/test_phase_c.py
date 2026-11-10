import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "publication"))
import phase_c


class PhaseCTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.headline = {"trials": [{"design": d, "mode": m, "cores": 8,
                                    "flags": {"threads": 16, "background-jobs": 8}}
                                   for d, m in (("native", "native_cpu"), ("full", "cuda_merge"))]}

    def test_fixed_grids_and_verification(self):
        grids = phase_c.build_grids(self.headline, self.root)
        self.assertEqual(len(grids["w"]["trials"]), 4)
        self.assertEqual(len(grids["b"]["trials"]), 4)
        for grid in grids.values():
            self.assertTrue(grid["common"]["verify"])
        self.assertEqual(grids["profile"]["common"]["duration-seconds"], 60)
        for trial in grids["w"]["trials"]:
            flags = trial["flags"]
            self.assertEqual(flags["l0-slowdown"], flags["l0-trigger"] * 2)
            self.assertEqual(flags["l0-stop"], flags["l0-trigger"] * 3)

    def test_baselines_have_own_preloaded_layouts(self):
        grid = phase_c.build_grids(self.headline, self.root)["b"]
        self.assertEqual(grid["trials"][0]["golden"], str(self.root / "universal"))
        self.assertEqual(grid["trials"][1]["golden"], str(self.root / "blobdb"))
        self.assertNotIn("golden", grid["trials"][2])

    def test_profile_command_has_no_bare_design(self):
        grid = phase_c.build_grids(self.headline, self.root)["profile"]
        with patch.object(phase_c, "physical_cores", return_value=(list(range(8)), [0])):
            for trial in grid["trials"]:
                command = phase_c.benchmark_command(Path("/bench"), self.root, trial, grid["common"])
                self.assertNotIn(trial["design"], command)
                self.assertEqual(command[command.index("--verify") + 1], "true")
                self.assertEqual(command[command.index("--mode") + 1], trial["mode"])
                self.assertTrue(all(arg.startswith("--") for arg in command[4::2]))

    def test_memory_retry_changes_all_baseline_arms_only(self):
        grids = phase_c.build_grids(self.headline, self.root, preferred_baselines=True)
        for name in ("b", "blob-smoke"):
            for trial in grids[name]["trials"]:
                self.assertEqual(trial["memory"], "preferred")
        for trial in grids["w"]["trials"] + grids["profile"]["trials"]:
            self.assertNotIn("memory", trial)

    def test_protocol_cannot_silently_change(self):
        path = self.root / "grid.json"
        phase_c.write_once(path, {"seed": 114})
        phase_c.write_once(path, {"seed": 114})
        with self.assertRaises(RuntimeError):
            phase_c.write_once(path, {"seed": 115})

    def test_verification_requires_expected_key_count(self):
        self.assertFalse(phase_c.verified({}, 10))
        self.assertFalse(phase_c.verified({"verification": {"keys": 9}}, 10))
        self.assertFalse(phase_c.verified({"verification": {"keys": 10, "deleted_keys": 1}}, 10))
        self.assertTrue(phase_c.verified({"verification": {"keys": 10}}, 10))

    def test_campaign_completion_rejects_bad_records(self):
        trial = {"design": "full", "block": 0}
        (self.root / "schedule.json").write_text(json.dumps([trial]))
        good = {"index": 0, "trial": trial, "flags": {"records": 10}, "exit_code": 0,
                "result": {"verification": {"keys": 10}}}
        path = self.root / "results.jsonl"
        path.write_text(json.dumps(good) + "\n")
        self.assertEqual(len(phase_c.campaign_complete(self.root)), 1)
        for rows in ([], [good, good], [{**good, "exit_code": 1}], [{**good, "trial": {"design": "native"}}],
                     [{**good, "result": {}}]):
            path.write_text("\n".join(json.dumps(row) for row in rows))
            with self.assertRaises(RuntimeError):
                phase_c.campaign_complete(self.root)


if __name__ == "__main__":
    unittest.main()