#!/usr/bin/env python3
"""Cost per write with the GPU dedicated or shared, from measured runs and prices.json.

  python3 publication/cost_model.py

Inputs: tuned whole-database runs (data/v3/v3-headline*), RocksDB's best 32-core tuning trial
(data/v2-tuning), the earlier shared-GPU campaign (data/shared-gpu, untuned RocksDB) and
prices.json. Prints, in markdown: cost per write at each core count; how much per-database
throughput ours must keep when N databases share one GPU to match RocksDB's cost per write;
the cheapest setup for each throughput target; and the shared-GPU measurements.
"""

from __future__ import annotations

import json
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from make_figures_v3 import FULL, NATIVE, load_campaign, native_32_runs, tuning_best, window_ops  # noqa: E402

HERE = Path(__file__).resolve().parent
PRICES = json.loads((HERE / "prices.json").read_text())
CORE, GPU = PRICES["physical_core_hour"], PRICES["gpu_hour"]


def per_billion(dollars_per_hour: float, writes_per_second: float) -> float:
    return dollars_per_hour / (writes_per_second * 3600 / 1e9)


def tuned_throughput() -> dict[tuple[str, int], float]:
    runs = load_campaign("v3-headline", "v3-headline-more")
    out = {(d, c): statistics.median(window_ops(r) for r in runs[(d, c)])
           for d in (NATIVE, FULL) for c in (4, 8, 16) if runs.get((d, c))}
    best = native_32_runs() or tuning_best("native", 32)
    if best:
        out[(NATIVE, 32)] = best
    return out


def shared_gpu(path: Path = HERE / "data" / "shared-gpu" / "results.jsonl") -> dict[tuple[str, int, int], list[float]]:
    """{(design, cores, instances): per-instance ingest-and-compact writes/s, one value per block}."""
    out: dict[tuple[str, int, int], list[float]] = {}
    if not path.exists():
        return out
    for line in path.read_text().splitlines():
        record = json.loads(line)
        trial = record["trial"]
        instances = trial.get("instances", 1)
        results = [record["result"]] if instances == 1 and "result" in record else record.get("results") or []
        if not results or any("keys" not in (r.get("verification") or {}) for r in results):
            continue
        per = statistics.mean(r["ingest_and_compact"]["ops_per_second"] for r in results)
        out.setdefault((trial["design"], trial["cores"], instances), []).append(per)
    return out


def shared_table(title: str, shared: dict[tuple[str, int, int], list[float]]) -> None:
    print(f"\n## {title}\n")
    print("| Cores | Databases | RocksDB per database | Ours per database | Ours kept vs 1 database | "
          "RocksDB $/billion | Ours $/billion (GPU / N) | Ours / RocksDB |")
    print("|---:|---:|---:|---:|---:|---:|---:|---:|")
    fmt = lambda v, f: f.format(v) if v is not None else "-"
    for cores in sorted({k[1] for k in shared}):
        base = shared.get(("full", cores, 1))
        base = statistics.median(base) if base else None
        nat1 = shared.get(("native", cores, 1))
        for n in sorted({k[2] for k in shared if k[1] == cores}):
            nat = shared.get(("native", cores, n)) or nat1
            ours = shared.get(("full", cores, n))
            nv = statistics.median(nat) if nat else None
            ov = statistics.median(ours) if ours else None
            nb = per_billion(cores * CORE, nv) if nv else None
            ob = per_billion(cores * CORE + GPU / n, ov) if ov else None
            print(f"| {cores} | {n} | {fmt(nv and nv / 1e3, '{:.0f}k')} | {fmt(ov and ov / 1e3, '{:.0f}k')} | "
                  f"{fmt(ov and base and ov / base, '{:.0%}')} | {fmt(nb, '{:.3f}')} | {fmt(ob, '{:.3f}')} | "
                  f"{fmt(nb and ob and ob / nb, '{:.2f}')} |")


def main() -> int:
    tput = tuned_throughput()
    print("## Cost per write at each core count (tuned runs, GPU dedicated)\n")
    print("| Cores | RocksDB writes/s | RocksDB $/billion | Ours writes/s | Ours $/billion | Ours / RocksDB |")
    print("|---:|---:|---:|---:|---:|---:|")
    for c in (4, 8, 16, 32):
        n, o = tput.get((NATIVE, c)), tput.get((FULL, c))
        nb = per_billion(c * CORE, n) if n else None
        ob = per_billion(c * CORE + GPU, o) if o else None
        cell = lambda v, f: f.format(v) if v is not None else "-"
        print(f"| {c} | {cell(n and n / 1e3, '{:.0f}k')} | {cell(nb, '{:.3f}')} | {cell(o and o / 1e3, '{:.0f}k')} "
              f"| {cell(ob, '{:.3f}')} | {cell(nb and ob and ob / nb, '{:.2f}')} |")

    print("\n## Ceiling: cost ratio if the GPU were free (= RocksDB writes/s / ours)\n")
    for c in (4, 8, 16):
        print(f"- {c} cores: ours at most {tput[(FULL, c)] / tput[(NATIVE, c)]:.2f}x cheaper per write")

    print("\n## Throughput each database must keep, with N databases sharing one GPU, to match RocksDB\n")
    print("Share of ours' single-database writes/s needed for parity (above 100% = parity impossible).\n")
    shares = (1, 2, 3, 4, 6, 8)
    print("| Cores | " + " | ".join(f"N={n}" for n in shares) + " |")
    print("|---:|" + "---:|" * len(shares))
    for c in (4, 8, 16):
        native_cost = per_billion(c * CORE, tput[(NATIVE, c)])
        needed = [(c * CORE + GPU / n) / (native_cost * tput[(FULL, c)] * 3600 / 1e9) for n in shares]
        print(f"| {c} | " + " | ".join(f"{x:.0%}" for x in needed) + " |")

    print("\n## Cheapest setup for a throughput target (one node, GPU dedicated)\n")
    print("| Target writes/s | RocksDB: cores, $/h | Ours: cores, $/h | Ours / RocksDB |")
    print("|---:|---|---|---:|")
    for target in (0.5e6, 0.75e6, 1.0e6, 1.25e6, 1.5e6, 2.0e6, 2.25e6):
        native = [(c * CORE, c) for c in (4, 8, 16, 32) if tput.get((NATIVE, c), 0) >= target]
        ours = [(c * CORE + GPU, c) for c in (4, 8, 16) if tput.get((FULL, c), 0) >= target]
        n, o = min(native, default=None), min(ours, default=None)
        ratio = f"{o[0] / n[0]:.2f}" if n and o else ("ours only" if o else "-")
        print(f"| {target / 1e6:.2f}M | {f'{n[1]}, {n[0]:.3f}' if n else 'not reached'} | "
              f"{f'{o[1]}, {o[0]:.3f}' if o else 'not reached'} | {ratio} |")

    shared = shared_gpu()
    if shared:
        shared_table("Earlier shared-GPU campaign (untuned RocksDB, no MPS)", shared)
    for extra in sys.argv[1:]:
        shared_table(f"Shared GPU, {extra} (RocksDB at N=1 where not run shared)", shared_gpu(Path(extra)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
