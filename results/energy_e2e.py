#!/usr/bin/env python3
"""E6 (PREREGISTRATION.md): energy per operation counted to the end of leftover compaction.

Uses only archived trials. Energy is RAPL package + DRAM, plus the GPU's board power, integrated
from the first operation to the end of the leftover compaction (energy_joules_end_to_end); the
operations are ingest_and_compact.ops. Ours ("full") is paired with RocksDB ("native") within a
block and cell. Two accountings:
  (i)  same machine: everything measured, including the idle GPU during RocksDB's runs;
  (ii) RocksDB on a machine without a GPU: RocksDB's GPU energy is left out (primary).

Usage: python3 results/energy_e2e.py [--out results/data/energy-e2e]
"""
import argparse
import json
import statistics
from pathlib import Path

DATA = Path(__file__).resolve().parent / "data"
HEADLINE = ["v3/v3-headline/results.jsonl", "v3/v3-headline-more/results.jsonl"]
CAMPAIGNS = ["main", "equal-cost", "ycsb", "contention", "prior-lz4", "prior-zstd", "prior-none",
             "prior-equal-cost"]
# In these, ours at one size is compared with RocksDB at several sizes.
UNEQUAL_CORES = {"equal-cost", "prior-equal-cost"}
IGNORED = {"design", "mode", "flags", "env", "block"}


def energy(row, with_gpu):
    joules = row.get("energy_joules_end_to_end") or {}
    ops = ((row.get("result") or {}).get("ingest_and_compact") or {}).get("ops")
    if not joules or not ops or row.get("exit_code", 0) != 0:
        return None
    total = joules["power/energy-pkg/"] + joules["power/energy-ram/"] + (joules["gpu"] if with_gpu else 0.0)
    return total / ops * 1e6  # microjoules per operation


def cell(row, ignore_cores):
    trial = row["trial"]
    return json.dumps({k: v for k, v in trial.items() if k not in IGNORED | ({"cores"} if ignore_cores else set())},
                      sort_keys=True)


def pairs(name, rows, unequal_cores):
    ours = [r for r in rows if r["trial"]["design"] == "full"]
    theirs = [r for r in rows if r["trial"]["design"] == "native"]
    found = []
    for a in ours:
        for b in theirs:
            if a["trial"].get("block") != b["trial"].get("block") or cell(a, unequal_cores) != cell(b, unequal_cores):
                continue
            if not unequal_cores and a["trial"]["cores"] != b["trial"]["cores"]:
                continue
            values = {"ours_i": energy(a, True), "rocksdb_i": energy(b, True), "rocksdb_ii": energy(b, False)}
            if None in values.values():
                continue
            label = {k: v for k, v in a["trial"].items() if k not in IGNORED}
            label["rocksdb_cores"] = b["trial"]["cores"]
            found.append({"campaign": name, "block": f'{name}:{a["trial"].get("block")}', "cell": label,
                          "ours_uj": values["ours_i"], "rocksdb_uj_i": values["rocksdb_i"],
                          "rocksdb_uj_ii": values["rocksdb_ii"],
                          "ratio_i": values["ours_i"] / values["rocksdb_i"],
                          "ratio_ii": values["ours_i"] / values["rocksdb_ii"]})
    return found


def load(path):
    return [json.loads(line) for line in open(path) if line.strip()]


def summarise(found):
    groups = {}
    for p in found:
        groups.setdefault(json.dumps(p["cell"], sort_keys=True), []).append(p)
    table = []
    for key, members in groups.items():
        table.append({"cell": json.loads(key), "blocks": len(members),
                      "ours_uj": statistics.median(p["ours_uj"] for p in members),
                      "rocksdb_uj_i": statistics.median(p["rocksdb_uj_i"] for p in members),
                      "rocksdb_uj_ii": statistics.median(p["rocksdb_uj_ii"] for p in members),
                      "ratio_i": statistics.median(p["ratio_i"] for p in members),
                      "ratio_ii": statistics.median(p["ratio_ii"] for p in members),
                      "ratio_ii_range": [min(p["ratio_ii"] for p in members), max(p["ratio_ii"] for p in members)]})
    return sorted(table, key=lambda t: json.dumps(t["cell"], sort_keys=True))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, default=DATA / "energy-e2e")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    headline = []
    for path in HEADLINE:
        headline += pairs(Path(path).parent.name, load(DATA / path), False)
    results = {"headline": summarise(headline)}
    for name in CAMPAIGNS:
        results[name] = summarise(pairs(name, load(DATA / "campaigns" / f"{name}.jsonl"), name in UNEQUAL_CORES))

    by_cores = {t["cell"]["cores"]: t for t in results["headline"]}
    results["hypotheses"] = {
        "Y-E6a (ii): ours <= RocksDB at 4, 8, 16 cores": all(by_cores[c]["ratio_ii"] <= 1.0 for c in (4, 8, 16)),
        "Y-E6b (i): ours < RocksDB at 4, 8, 16 cores": all(by_cores[c]["ratio_i"] < 1.0 for c in (4, 8, 16)),
    }
    (args.out / "energy_e2e.json").write_text(json.dumps(results, indent=1))

    lines = ["# E6: energy per operation to the end of leftover compaction", "",
             "Microjoules per operation, medians over blocks. Ratio = ours / RocksDB. (i) same machine, "
             "idle GPU counted for RocksDB; (ii) RocksDB without a GPU (primary).", ""]
    for name, table in results.items():
        if name == "hypotheses":
            continue
        lines += [f"## {name}", "", "| cell | blocks | ours µJ | RocksDB µJ (i) | RocksDB µJ (ii) | ratio (i) | ratio (ii) | ratio (ii) range |",
                  "|---|---|---|---|---|---|---|---|"]
        for t in table:
            label = ", ".join(f"{k}={v}" for k, v in t["cell"].items() if k != "design")
            lines.append(f'| {label} | {t["blocks"]} | {t["ours_uj"]:.2f} | {t["rocksdb_uj_i"]:.2f} | '
                         f'{t["rocksdb_uj_ii"]:.2f} | {t["ratio_i"]:.3f} | {t["ratio_ii"]:.3f} | '
                         f'{t["ratio_ii_range"][0]:.3f}–{t["ratio_ii_range"][1]:.3f} |')
        lines.append("")
    lines += ["## Hypotheses", ""] + [f"- {k}: {'supported' if v else 'not supported'}"
                                     for k, v in results["hypotheses"].items()]
    (args.out / "summary.md").write_text("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
