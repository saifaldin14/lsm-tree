"""Statistics for repeated campaigns: medians with percentile-bootstrap 95% CIs and
paired ratios within repetition blocks (see PREREGISTRATION.md)."""

from __future__ import annotations

import random
import statistics
from collections.abc import Callable, Sequence

RESAMPLES = 10_000


def bootstrap_ci(values: Sequence[float], statistic: Callable[[Sequence[float]], float] = statistics.median,
                 resamples: int = RESAMPLES, seed: int = 1) -> tuple[float, float, float]:
    """(point estimate, 2.5th percentile, 97.5th percentile)."""
    values = list(values)
    if not values:
        raise ValueError("no values")
    point = statistic(values)
    if len(values) == 1:
        return point, point, point
    generator = random.Random(seed)
    estimates = sorted(statistic([generator.choice(values) for _ in values]) for _ in range(resamples))
    return point, estimates[int(0.025 * resamples)], estimates[int(0.975 * resamples) - 1]


def paired_ratios(treatment: dict[int, float], baseline: dict[int, float]) -> list[float]:
    """treatment/baseline for every repetition block present in both."""
    return [treatment[block] / baseline[block] for block in sorted(treatment) if block in baseline and baseline[block]]


def ratio_ci(treatment: dict[int, float], baseline: dict[int, float]) -> tuple[float, float, float]:
    return bootstrap_ci(paired_ratios(treatment, baseline))
