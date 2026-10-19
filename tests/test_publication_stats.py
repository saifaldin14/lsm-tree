import importlib.util
import math
import unittest
from pathlib import Path

_SPEC = importlib.util.spec_from_file_location(
    "publication_analyze", Path(__file__).resolve().parents[1] / "publication" / "analyze.py"
)
analyze = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(analyze)


class PairedStatisticsTest(unittest.TestCase):
    def test_sign_test_exact(self):
        self.assertAlmostEqual(analyze.sign_test([0.8] * 12), 2 / 2**12)
        # 2 of 10 above one: 2 * P(X <= 2 | n=10, p=0.5) = 2 * 56 / 1024.
        self.assertAlmostEqual(analyze.sign_test([1.2, 1.1] + [0.9] * 8), 2 * 56 / 1024)
        self.assertEqual(analyze.sign_test([1.0, 1.0]), 1.0)

    def test_wilcoxon_exact(self):
        self.assertAlmostEqual(analyze.wilcoxon_exact([0.9] * 3 + [0.8, 0.7]), 2 / 2**5)
        # Smallest-magnitude difference is the only positive one: W+ = 1, n = 5.
        # P(W+ <= 1) = 2/32, two-sided 4/32.
        ratios = [1.01, 0.9, 0.8, 0.7, 0.6]
        self.assertAlmostEqual(analyze.wilcoxon_exact(ratios), 4 / 32)

    def test_geomean_ci(self):
        gm, lo, hi = analyze.geomean_ci([0.5, 2.0])
        self.assertAlmostEqual(gm, 1.0)
        self.assertLess(lo, 1.0)
        self.assertGreater(hi, 1.0)
        gm, lo, hi = analyze.geomean_ci([0.8, 0.8, 0.8])
        self.assertAlmostEqual(gm, 0.8)
        self.assertAlmostEqual(lo, 0.8)
        self.assertTrue(math.isnan(analyze.geomean_ci([0.8])[1]))

    def test_holm_adjustment(self):
        adjusted = analyze.holm([0.01, 0.04, 0.03, 0.005])
        self.assertEqual([round(p, 6) for p in adjusted], [0.03, 0.06, 0.06, 0.02])
        self.assertEqual(analyze.holm([0.5, 0.9]), [1.0, 1.0])

    def test_point_level_weights_points_equally(self):
        result = analyze.point_level([[0.8, 0.8, 0.8], [0.8] * 30, [0.9]])
        self.assertEqual(result["points"], 3)
        self.assertAlmostEqual(result["ratio"], math.exp((2 * math.log(0.8) + math.log(0.9)) / 3))
        self.assertEqual(result["points_gpu_better"], 3)

    def test_t_quantiles(self):
        self.assertAlmostEqual(analyze.t975(2), 4.303)
        self.assertAlmostEqual(analyze.t975(11), 2.201)
        self.assertAlmostEqual(analyze.t975(32), 2.036933343, places=5)
        self.assertAlmostEqual(analyze.t975(60), 2.000297822, places=5)


if __name__ == "__main__":
    unittest.main()
