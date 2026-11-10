"""Local regression tests for publication figures 27 and 28."""

import copy
import io
import json
import math
import statistics
import sys
import tempfile
import unittest
from contextlib import redirect_stderr
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "publication"))
import make_figures_v3 as figures


HEADER = "Device r/s rMB/s rrqm/s %rrqm r_await rareq-sz w/s wMB/s wrqm/s %wrqm w_await wareq-sz %util"
SHORT_HEADER = "Device r/s rMB/s w/s wMB/s %util"


def cpu_result(used=3.0, roles=None):
    return {"verification": {"keys": 1}, "process_cpu": {"cores_used": used},
            "accounting": {"window": {"cores_by_role": roles if roles is not None else {"foreground": 1.0}}}}


def record(index=0, design="native", block=0, started=1000.0, start=1002.25, end=1006.75):
    result = cpu_result()
    result["window"] = {"start_unix": start, "end_unix": end}
    return {"index": index, "exit_code": 0, "started_unix": started, "result": result,
            "trial": {"design": design, "block": block, "cores": 8},
            "flags": {"warmup-seconds": 999, "duration-seconds": 999}}


def iostat_text(rows):
    lines = ["Linux test host"]
    for read_iops, read_mib, write_iops, write_mib, busy in rows:
        lines += [HEADER, f"nvme0n1 {read_iops} {read_mib} 0 0 0 0 {write_iops} {write_mib} 0 0 0 0 {busy}", ""]
    return "\n".join(lines) + "\n"


class TelemetryTestCase(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.path = self.root / "iostat.txt"
        self.addCleanup(figures.plt.close, "all")

    def write_log(self, text):
        self.path.write_text(text)
        return self.path

    def write_campaign(self, records, samples):
        root = self.root / "data/malphite-nvme/followup/n6"
        root.mkdir(parents=True, exist_ok=True)
        (root / "results.jsonl").write_text("\n".join(json.dumps(r) for r in records) + "\n")
        for index, rows in samples.items():
            path = root / f"trial-{index:04d}" / "iostat.txt"
            path.parent.mkdir()
            path.write_text(iostat_text(rows))
        return root


class IostatSeriesTests(TelemetryTestCase):
    def test_preserves_first_sample_and_zero_reads(self):
        rows = [(0.0, 0.0, 2.0, 0.05, 0.0), (3.0, 4.0, 5.0, 6.0, 70.0)]
        self.assertEqual(figures.iostat_series(self.write_log(iostat_text(rows))), rows)

    def test_since_boot_is_explicit(self):
        rows = [(99, 99, 99, 99, 99), (0, 0, 1, 2, 3)]
        path = self.write_log(iostat_text(rows))
        self.assertEqual(figures.iostat_series(path, since_boot=True), rows[1:])
        self.write_log(iostat_text(rows[:1]))
        with self.assertRaisesRegex(ValueError, "interval samples"):
            figures.iostat_series(path, since_boot=True)

    def test_permuted_and_repeated_headers(self):
        text = ("Device: %util wMB/s r/s w/s rMB/s aqu-sz\n"
                "nvme0n1 6 4 1 3 2 0\n\n"
                "Device w/s rMB/s %util wMB/s r/s\n"
                "nvme0n1 30 20 60 40 10\n")
        self.assertEqual(figures.iostat_series(self.write_log(text)), [(1, 2, 3, 4, 6), (10, 20, 30, 40, 60)])

    def test_kibibytes_convert_explicitly_to_mebibytes(self):
        text = "Device r/s rkB/s w/s wkB/s %util\nnvme0n1 1 1024 2 2560 3\n"
        self.assertEqual(figures.iostat_series(self.write_log(text)), [(1, 1, 2, 2.5, 3)])

    def test_units_are_resolved_for_each_direction(self):
        text = "Device w/s rMB/s %util wkB/s r/s\nnvme0n1 3 2 5 4096 1\n"
        self.assertEqual(figures.iostat_series(self.write_log(text)), [(1, 2, 3, 4, 5)])

    def test_exact_device_match(self):
        text = SHORT_HEADER + "\nnvme0n1p1 1 2 3 4 5\nnvme0n10 6 7 8 9 10\n"
        with self.assertRaisesRegex(ValueError, "nvme0n1.*absent"):
            figures.iostat_series(self.write_log(text))

    def test_other_devices_do_not_change_sample_count(self):
        text = SHORT_HEADER + "\nnvme0n1p1 invalid\nnvme0n1 1 2 3 4 5\n"
        self.assertEqual(figures.iostat_series(self.write_log(text)), [(1, 2, 3, 4, 5)])

    def test_missing_device_in_a_report_is_not_silently_removed(self):
        text = (SHORT_HEADER + "\nnvme0n1 1 2 3 4 5\n" + SHORT_HEADER + "\nsda 1 2 3 4 5\n"
                + SHORT_HEADER + "\nnvme0n1 6 7 8 9 10\n")
        with self.assertRaisesRegex(ValueError, "nvme0n1.*absent"):
            figures.iostat_series(self.write_log(text))

    def test_malformed_rows_fail_without_compressing_time(self):
        for row in ("1 2 3 4", "1 2 3 4 5 6", "bad 2 3 4 5", "nan 2 3 4 5",
                    "1 inf 3 4 5", "1 2 -3 4 5", "1 2 3 4 -5"):
            with self.subTest(row=row), self.assertRaisesRegex(ValueError, "iostat.txt:2.*malformed"):
                figures.iostat_series(self.write_log(SHORT_HEADER + "\nnvme0n1 " + row + "\n"))

    def test_missing_duplicate_or_unsupported_columns_fail(self):
        for header in ("Device r/s rMB/s w/s wMB/s", "Device r/s rGB/s w/s wGB/s %util",
                       "Device r/s rMB/s rMB/s w/s wMB/s %util"):
            with self.subTest(header=header), self.assertRaisesRegex(ValueError, "header"):
                figures.iostat_series(self.write_log(header + "\n"))

    def test_missing_header_empty_file_and_missing_file_fail(self):
        for text in ("nvme0n1 1 2 3 4 5\n", ""):
            with self.subTest(text=text), self.assertRaisesRegex(ValueError, "header"):
                figures.iostat_series(self.write_log(text))
        self.path.unlink()
        with self.assertRaisesRegex(ValueError, "recording"):
            figures.iostat_series(self.path)


class MeasurementWindowTests(TelemetryTestCase):
    def test_full_bins_ignore_warmup_verification_and_busy_thresholds(self):
        rows = [(999, 999, 999, 999, 100)] * 3 + [(0, 0, 10, 20, 0), (2, 1, 30, 40, 2),
                                                (0, 0, 50, 60, 49)] + [(999, 999, 999, 999, 100)] * 2
        run = record(started=1000.25, start=1002.75, end=1006.75)
        selected = figures.iostat_window(rows, run)
        self.assertEqual(selected["samples"], rows[3:6])
        self.assertEqual(selected["relative_edges_seconds"], [0.5, 1.5, 2.5, 3.5])
        self.assertEqual(selected["sample_count"], 3)
        self.assertEqual(selected["recorded_samples"], 8)

    def test_exact_boundaries_include_first_and_last_full_bins(self):
        rows = [(i, i, i, i, i) for i in range(3)]
        selected = figures.iostat_window(rows, record(started=1000.25, start=1000.25, end=1003.25))
        self.assertEqual(selected["samples"], rows)
        self.assertEqual(selected["relative_edges_seconds"], [0, 1, 2, 3])

    def test_epoch_boundaries_are_not_rounded_to_admit_partial_bins(self):
        epoch = 1794325567.0
        rows = [(i, i, i, i, i) for i in range(8)]
        run = record(started=epoch, start=math.nextafter(epoch + 1, math.inf),
                     end=math.nextafter(epoch + 5, -math.inf))
        selected = figures.iostat_window(rows, run)
        self.assertEqual(selected["samples"], rows[2:4])
        self.assertEqual(selected["sample_count"], 2)

    def test_insufficient_recording_is_an_error_not_a_shortened_window(self):
        rows = [(0, 0, 1, 2, 3)] * 5
        for start, end in ((999.9, 1003), (1001, 1005.1), (1008, 1010)):
            with self.subTest(start=start, end=end), self.assertRaisesRegex(ValueError, "insufficient.*recording"):
                figures.iostat_window(rows, record(start=start, end=end))
        with self.assertRaisesRegex(ValueError, "recording"):
            figures.iostat_window([], record())

    def test_no_full_measurement_bins_is_an_error(self):
        with self.assertRaisesRegex(ValueError, "no full measurement bins"):
            figures.iostat_window([(0, 0, 0, 0, 0)], record(start=1000.2, end=1000.8))

    def test_missing_nonfinite_or_reversed_timestamps_fail(self):
        for field in ("start_unix", "end_unix"):
            for value in (None, float("nan"), float("inf")):
                run = record()
                run["result"]["window"][field] = value
                with self.subTest(field=field, value=value), self.assertRaisesRegex(ValueError, "window|timestamp"):
                    figures.iostat_window([(0, 0, 0, 0, 0)] * 10, run)
        run = record()
        del run["started_unix"]
        with self.assertRaisesRegex(ValueError, "started_unix"):
            figures.iostat_window([(0, 0, 0, 0, 0)] * 10, run)
        with self.assertRaisesRegex(ValueError, "window"):
            figures.iostat_window([(0, 0, 0, 0, 0)] * 10, record(start=1005, end=1004))


class DiskAnalysisTests(TelemetryTestCase):
    def test_failed_and_unverified_records_are_excluded_with_reasons(self):
        failed, unverified, invalid_keys = record(1), record(2), record(3)
        failed["exit_code"] = 1
        failed.pop("result")
        unverified["result"].pop("verification")
        invalid_keys["result"]["verification"]["keys"] = None
        root = self.write_campaign([failed, unverified, invalid_keys, record(4)], {4: [(0, 0, 1, 2, 3)] * 8})
        diagnostics = io.StringIO()
        with redirect_stderr(diagnostics):
            series, excluded = figures.load_disk_runs(root)
        self.assertEqual([r["index"] for r in series[figures.NATIVE]], [4])
        self.assertEqual([r["index"] for r in excluded], [1, 2, 3])
        self.assertIn("exit_code", excluded[0]["reason"])
        self.assertIn("verification", excluded[1]["reason"])
        self.assertIn("excluded trial 1", diagnostics.getvalue())

    def test_zero_verified_keys_is_not_treated_as_unverified(self):
        run = record()
        run["result"]["verification"]["keys"] = 0
        root = self.write_campaign([run], {0: [(0, 0, 0, 0, 0)] * 8})
        series, excluded = figures.load_disk_runs(root)
        self.assertEqual(excluded, [])
        self.assertEqual(len(series[figures.NATIVE]), 1)

    def test_eligible_missing_recording_is_an_error(self):
        root = self.write_campaign([record()], {})
        with self.assertRaisesRegex(ValueError, "trial-0000.*recording|recording.*trial-0000"):
            figures.load_disk_runs(root)

    def test_missing_results_is_an_error(self):
        with self.assertRaisesRegex(ValueError, "results.jsonl"):
            figures.load_disk_runs(self.root)

    def test_summary_is_median_of_run_means_not_pooled_samples(self):
        records = [record(0, start=1000, end=1001), record(1, block=1, start=1000, end=1003)]
        root = self.write_campaign(records, {0: [(0, 0, 0, 0, 0)], 1: [(0, 0, 100, 100, 100)] * 3})
        series, excluded = figures.load_disk_runs(root)
        summary = figures.disk_summary(series, excluded)
        native = summary["designs"][figures.NATIVE]
        self.assertEqual(native["n"], 2)
        self.assertEqual(native["sample_counts"], [1, 3])
        self.assertEqual(native["median_per_run_mean"]["write_mib_s"], 50)
        self.assertEqual(native["median_per_run_mean"]["read_iops"], 0)
        json.dumps(summary, allow_nan=False)

    def test_figure_uses_first_paired_block_zero_and_measurement_bins(self):
        records = [record(3, "full", 1), record(2, "native", 1), record(1, "native"), record(0, "full")]
        rows = [(900, 900, 900, 900, 100)] * 3 + [(0, 0, 1, 2, 3)] * 3 + [(800, 800, 800, 800, 100)] * 2
        self.write_campaign(records, {r["index"]: rows for r in records})
        with patch.object(figures, "HERE", self.root), patch.object(figures, "save") as save:
            summary = figures.figure_disk()
        fig, name = save.call_args.args
        self.assertEqual(name, "fig27_nvme_disk")
        self.assertEqual(len(fig.axes), 6)
        self.assertEqual(summary["trace_pair"], {"block": 0, "trials": {figures.NATIVE: 1, figures.FULL: 0}})
        self.assertEqual(summary["designs"][figures.NATIVE]["sample_counts"], [3, 3])
        traces = fig.axes[0].patches
        self.assertEqual(len(traces), 4)
        for trace in traces:
            data = trace.get_data()
            self.assertEqual(list(data.edges), [0.75, 1.75, 2.75, 3.75])
            self.assertIn(list(data.values), [[0, 0, 0], [2, 2, 2]])
        self.assertEqual(summary["designs"][figures.NATIVE]["median_per_run_mean"]["write_mib_s"], 2)
        for ax in fig.axes[3:]:
            self.assertTrue(all(rect.get_height() >= 0 for rect in ax.patches))

    def test_small_nonzero_read_labels_stay_distinct_from_zero(self):
        records = [record(0, "native"), record(1, "full")]
        self.write_campaign(records, {0: [(0.006689, 0.000134, 1, 2, 3)] * 8,
                                      1: [(0, 0, 1, 2, 3)] * 8})
        with patch.object(figures, "HERE", self.root), patch.object(figures, "save") as save:
            figures.figure_disk()
        fig = save.call_args.args[0]
        bandwidth_labels = [text.get_text() for text in fig.axes[3].texts]
        self.assertIn("1.34e-04", bandwidth_labels)
        self.assertIn("0", bandwidth_labels)
        self.assertIn("0.00669", [text.get_text() for text in fig.axes[4].texts])

    def test_missing_block_zero_pair_is_an_error(self):
        records = [record(0, "native", 1), record(1, "full", 1)]
        self.write_campaign(records, {0: [(0, 0, 0, 0, 0)] * 8, 1: [(0, 0, 0, 0, 0)] * 8})
        with patch.object(figures, "HERE", self.root), self.assertRaisesRegex(ValueError, "block 0"):
            figures.figure_disk()


class CpuAnalysisTests(TelemetryTestCase):
    def test_other_and_unattributed_sum_to_process_cpu(self):
        roles = {"foreground": 1.0, "flush": 0.5, "compaction": 0.75, "other": 0.1,
                 "gpu": 0.05, "merge_read": 0.1, "replication": 0.2, "new_role": 0.1}
        row = figures.cpu_run_components(cpu_result(3.25, roles), 4)
        self.assertAlmostEqual(row["components"]["other"], 0.55)
        self.assertAlmostEqual(row["components"]["unattributed"], 0.45)
        self.assertAlmostEqual(math.fsum(row["components"].values()), 3.25)
        self.assertAlmostEqual(row["unused_core_budget"], 0.75)

    def test_means_preserve_per_run_sums_and_residuals(self):
        runs = [cpu_result(2, {"foreground": 1}), cpu_result(3, {"flush": 2}), cpu_result(4, {"compaction": 4})]
        headline = {(d, 4): runs for d in (figures.NATIVE, figures.FULL)}
        summary = figures.cpu_use_summary(headline)
        for group in summary["groups"]:
            self.assertEqual(group["n"], 3)
            self.assertEqual(group["process_cores_used"], 3)
            self.assertAlmostEqual(group["components"]["foreground"], 1 / 3)
            self.assertAlmostEqual(group["components"]["flush"], 2 / 3)
            self.assertAlmostEqual(group["components"]["compaction"], 4 / 3)
            self.assertAlmostEqual(group["components"]["unattributed"], 2 / 3)
            self.assertAlmostEqual(math.fsum(group["components"].values()), group["process_cores_used"])
            self.assertEqual(group["unused_core_budget"], 1)
        json.dumps(summary, allow_nan=False)

    def test_empty_role_accounting_is_all_unattributed(self):
        row = figures.cpu_run_components(cpu_result(2, {}), 4)
        self.assertEqual(row["components"]["unattributed"], 2)
        self.assertEqual(math.fsum(row["components"].values()), 2)

    def test_role_excess_is_checked_per_run_before_averaging(self):
        runs = [cpu_result(2, {"foreground": 2.01}), cpu_result(3, {"foreground": 1})]
        with self.assertRaisesRegex(ValueError, "roles.*exceed.*process"):
            figures.cpu_use_summary({(d, 4): runs for d in (figures.NATIVE, figures.FULL)})

    def test_rounding_excess_is_reconciled_without_negative_segments(self):
        row = figures.cpu_run_components(cpu_result(1, {"foreground": 1.0000005}), 4)
        self.assertAlmostEqual(math.fsum(row["components"].values()), 1)
        self.assertAlmostEqual(row["rounding_adjustment_cores"], 0.0000005)
        self.assertTrue(all(v >= 0 for v in row["components"].values()))
        self.assertEqual(row["components"]["unattributed"], 0)

    def test_invalid_cpu_numbers_and_over_budget_fail(self):
        for value in (-1, float("nan"), float("inf"), None, True):
            with self.subTest(process=value), self.assertRaises(ValueError):
                figures.cpu_run_components(cpu_result(value), 4)
            with self.subTest(role=value), self.assertRaises(ValueError):
                figures.cpu_run_components(cpu_result(2, {"foreground": value}), 4)
        with self.assertRaisesRegex(ValueError, "budget"):
            figures.cpu_run_components(cpu_result(4.01, {}), 4)
        missing = cpu_result()
        del missing["process_cpu"]
        with self.assertRaisesRegex(ValueError, "process_cpu"):
            figures.cpu_run_components(missing, 4)

    def test_unverified_cpu_results_are_excluded_with_reasons(self):
        bad = cpu_result()
        bad.pop("verification")
        headline = {(d, 4): [cpu_result(), copy.deepcopy(bad)] for d in (figures.NATIVE, figures.FULL)}
        with redirect_stderr(io.StringIO()):
            summary = figures.cpu_use_summary(headline)
        self.assertEqual(len(summary["excluded"]), 2)
        self.assertTrue(all("verification" in e["reason"] for e in summary["excluded"]))
        self.assertTrue(all(g["n"] == 1 for g in summary["groups"]))

    def test_cpu_figure_has_paired_nonnegative_sum_preserving_bars(self):
        headline = {(d, c): [cpu_result(c * 0.75, {"foreground": c * 0.25, "other": 0.1})]
                    for c in (4, 8, 16) for d in (figures.NATIVE, figures.FULL)}
        with patch.object(figures, "save") as save:
            summary = figures.figure_cpu_use(headline)
        fig, name = save.call_args.args
        self.assertEqual(name, "fig28_cpu_use")
        self.assertGreaterEqual(fig.get_figwidth(), figures.DOUBLE_COLUMN)
        ax = fig.axes[0]
        self.assertEqual([label.get_text() for label in ax.get_xticklabels()], ["4 cores", "8 cores", "16 cores"])
        self.assertNotIn("idle", ax.get_ylabel().lower())
        bars = {}
        for rect in ax.patches:
            self.assertGreaterEqual(rect.get_height(), 0)
            self.assertGreaterEqual(rect.get_y(), 0)
            bars.setdefault(rect.get_x(), []).append(rect)
        self.assertEqual(len(bars), 6)
        self.assertEqual(sorted(round(sum(r.get_height() for r in stack), 8) for stack in bars.values()),
                         [4, 4, 8, 8, 16, 16])
        for group in summary["groups"]:
            self.assertAlmostEqual(sum(group["components"].values()), group["process_cores_used"])


class SavedTelemetryTests(unittest.TestCase):
    def test_all_six_local_n6_logs_supply_299_full_bins(self):
        root = ROOT / "publication/data/malphite-nvme/followup/n6"
        if not (root / "results.jsonl").exists():
            self.skipTest("N6 archive is not installed")
        series, excluded = figures.load_disk_runs(root)
        self.assertEqual(excluded, [])
        self.assertEqual(sum(map(len, series.values())), 6)
        for runs in series.values():
            self.assertEqual(len(runs), 3)
            self.assertEqual([r["sample_count"] for r in runs], [299, 299, 299])
            for run in runs:
                self.assertGreaterEqual(run["relative_edges_seconds"][0], 0)
                self.assertLessEqual(run["relative_edges_seconds"][-1], run["window_end_unix"] - run["window_start_unix"])

    def test_saved_cpu_stacks_match_process_means(self):
        headline = figures.load_campaign("v3-headline", "v3-headline-more")
        if not headline:
            self.skipTest("Headline archive is not installed")
        summary = figures.cpu_use_summary(headline)
        self.assertEqual(len(summary["groups"]), 6)
        for group in summary["groups"]:
            self.assertEqual(group["n"], 5)
            expected = statistics.mean(r["process_cpu"]["cores_used"] for r in headline[(group["design"], group["cores"])])
            self.assertAlmostEqual(group["process_cores_used"], expected)
            self.assertAlmostEqual(math.fsum(group["components"].values()), expected)
            self.assertTrue(all(value >= 0 for value in group["components"].values()))


if __name__ == "__main__":
    unittest.main()