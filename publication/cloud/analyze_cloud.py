#!/usr/bin/env python3
"""Analysis of the cloud experiments (PREREGISTRATION.md, "Cloud: the compaction service on
Kubernetes"): C0 engines, C1 capacity, C2 isolation, C3 elastic, C4 Kubernetes against plain VMs,
C5 cost, and the registered hypotheses Z1-Z5. Reads the driver's records from DIR
(default publication/data/cloud/azure) with multiplex/analyze.py's r9 reader.

  python3 publication/cloud/analyze_cloud.py [DIR]
"""

from __future__ import annotations

import json
import math
import statistics
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "multiplex"))
import analyze  # noqa: E402

DATA = HERE.parent / "data" / "cloud" / "azure"
PRICES = HERE.parent / "prices-azure.json"
DB_CPUS, NODE_CPUS = 4, 16


def median(values):
    values = [v for v in values if v is not None]
    return statistics.median(values) if values else None


def load(path: Path) -> dict[tuple, dict]:
    """(design, N) -> medians over repetitions (analyze.row), with every repetition kept."""
    if not path.exists():
        return {}
    rows = {}
    for key, runs in analyze.m2(path).items():
        row = analyze.row(runs)
        row["runs"] = runs
        row["retries"] = None
        rows[key] = row
    return rows


def records(path: Path) -> list[dict]:
    return [json.loads(line) for line in path.open()] if path.exists() else []


def fmt(value, digits=0, scale=1.0, suffix=""):
    if value is None or (isinstance(value, float) and math.isnan(value)):
        return "–"
    return f"{value / scale:,.{digits}f}{suffix}"


def c0(directory: Path) -> int | None:
    results = {}
    for path in sorted(directory.glob("c0-k*.jsonl")):
        k = int(path.stem.split("-k")[1])
        rows = load(path)
        if rows:
            results[k] = next(iter(rows.values()))
    if not results:
        return None
    best = max(r["with_backlog"] for r in results.values())
    chosen = min(k for k, r in results.items() if r["with_backlog"] >= 0.95 * best)
    print("\n## C0: engines (service-fair, N = 6, one repetition)\n")
    print("| K | Writes/s per database, window | counted to end of leftover compaction | Fallbacks |")
    print("|---|---|---|---|")
    for k, r in sorted(results.items()):
        print(f"| {k} | {fmt(r['per_db'], scale=1e3, suffix='k')} | {fmt(r['with_backlog'], scale=1e3, suffix='k')} | "
              f"{sum(r['fallbacks'])} |")
    print(f"\nRule (smallest K within 95% of the best): K = {chosen}.")
    return chosen


def price(size: str) -> float:
    return json.loads(PRICES.read_text())["sizes"][size]["usd_per_hour"]


def cost_per_billion(writes_per_s: float, n: int, service: bool, tenant_size: str, service_size: str) -> float:
    dollars_per_hour = price(tenant_size) * DB_CPUS / NODE_CPUS + (price(service_size) / n if service else 0.0)
    return dollars_per_hour / (writes_per_s * 3600) * 1e9


def c1(directory: Path, name: str = "c1-k8s.jsonl") -> dict:
    rows = load(directory / name)
    if not rows:
        return {}
    print(f"\n## C1: capacity over cloud TCP ({name}; medians of {len(next(iter(rows.values()))['runs'])} repetitions)\n")
    print("| Design | N | Writes/s per DB, window | counted to end of leftover | Share of N = 1 | Leftover (s) | "
          "Service cores busy | Input GiB/s | Fallbacks | Worst p99.9 (ms) |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    base = {design: rows.get((design, 1)) for design, _ in rows}
    for (design, n), r in sorted(rows.items()):
        share = r["with_backlog"] / base[design]["with_backlog"] if base.get(design) else None
        busy = median([run.get("server_cores_busy") for run in r["runs"]])
        print(f"| {design} | {n} | {fmt(r['per_db'], scale=1e3, suffix='k')} | "
              f"{fmt(r['with_backlog'], scale=1e3, suffix='k')} | {fmt(share, 2)} | {fmt(r['drain_s'], 1)} | "
              f"{fmt(busy, 1)} | {fmt(r['input_gib_s'], 2)} | {r['fallbacks']} | {fmt(r['p999_ms'], 1)} |")
    service = sorted({d for d, _ in rows if d.startswith("service")})
    for design in service:
        n1 = rows.get((design, 1))
        capacity = max((n for d, n in rows if d == design and n1 and rows[(d, n)]["with_backlog"] >= 0.8 *
                        n1["with_backlog"] and all(f == 0 for f in rows[(d, n)]["fallbacks"])), default=None)
        print(f"\nCapacity of one service ({design}; largest N at >= 80% of N = 1, zero fallbacks): N = {capacity}.")
    return rows


def c5(rows: dict, tenant_size: str = "Standard_D16ds_v6", service_size: str = "Standard_D16ads_v6") -> None:
    if not rows or not PRICES.exists():
        return
    print("\n## C5: cost per billion writes (pay-as-you-go prices, prices-azure.json)\n")
    print("| Design | N | $ per billion writes | Service-node price at which the service breaks even ($/h) |")
    print("|---|---|---|---|")
    for (design, n), r in sorted(rows.items()):
        service = design != "native"
        cost = cost_per_billion(r["with_backlog"], n, service, tenant_size, service_size)
        breakeven = None
        native = rows.get(("native", n)) or rows.get(("native", 1))
        if service and native:
            # Price p at which (tenant share + p/N) / service writes = tenant share / native writes.
            tenant_share = price(tenant_size) * DB_CPUS / NODE_CPUS
            breakeven = n * tenant_share * (r["with_backlog"] / native["with_backlog"] - 1)
        print(f"| {design} | {n} | {fmt(cost, 3)} | {fmt(breakeven, 2) if service else ''} |")


def c2(directory: Path) -> None:
    data = records(directory / "c2-isolation.jsonl")
    if not data:
        return
    print("\n## C2: isolation (one heavy, three light tenants; p99.9 write latency, ms)\n")
    print("| Policy | Light tenants' worst p99.9, shared | Solo p99.9 | Ratio | Heavy writes/s | Fallbacks |")
    print("|---|---|---|---|---|---|")
    ratios = {}
    for policy in sorted({r["design"].split("-", 1)[1] for r in data}):
        shared = [r for r in data if r["design"] == f"shared-{policy}"]
        solo = [r for r in data if r["design"] == f"solo-{policy}"]
        worst = median([max(x["write_latency"]["p999_ns"] for t, x in r["results"].items() if t != "db-0") / 1e6
                        for r in shared])
        alone = median([r["results"]["db-1"]["write_latency"]["p999_ns"] / 1e6 for r in solo])
        heavy = median([r["results"]["db-0"]["throughput"]["ops_per_second"] for r in shared])
        fallbacks = sum((x.get("transform") or {}).get("merge_fallbacks") or 0 for r in shared + solo
                        for x in r["results"].values())
        ratios[policy] = worst / alone if worst and alone else None
        print(f"| {policy} | {fmt(worst, 1)} | {fmt(alone, 1)} | {fmt(ratios[policy], 2)} | "
              f"{fmt(heavy, scale=1e3, suffix='k')} | {fallbacks} |")
    if "fair" in ratios and "fifo" in ratios:
        held = ratios["fair"] is not None and ratios["fair"] <= 1.5 and ratios["fifo"] is not None and ratios["fifo"] > 1.5
        print(f"\n- **Z3 {'supported' if held else 'not supported'}:** fair {fmt(ratios['fair'], 2)}x, "
              f"fifo {fmt(ratios['fifo'], 2)}x (fair <= 1.5x and fifo > 1.5x).")


def total_per_second(results: list[dict]) -> dict[int, float]:
    """Writes/s summed over databases, by whole Unix second."""
    total: dict[int, float] = {}
    for result in results:
        start = result["ingest_and_compact"]["start_unix"]
        for point in result.get("timeline") or []:
            second = int(start + point["second"])
            total[second] = total.get(second, 0.0) + point["ops"]
    return total


def c3(directory: Path) -> None:
    data = records(directory / "c3-elastic.jsonl")
    if not data:
        return
    print("\n## C3: elastic (6 databases through the Kubernetes Service)\n")
    print("| Repetition | Writes/s, 60 s before scale-out | 60 s after the 2nd replica is ready | Change | "
          "Fallbacks | Retries | Recycled connections | Refused while draining | Drains complete |")
    print("|---|---|---|---|---|---|---|---|---|")
    held = True
    for record in data:
        results = list(record["results"].values())
        per_second = total_per_second(results)
        out_at = next((e["unix"] for e in record["scale_events"] if e.get("replicas") == 2), None)
        ready_at = next((e["unix"] for e in record["scale_events"] if e.get("ready_replicas") == 2), None)
        before = after = None
        if out_at and ready_at:
            before = statistics.mean(per_second.get(int(out_at) - s, 0.0) for s in range(1, 61))
            after = statistics.mean(per_second.get(int(ready_at) + s, 0.0) for s in range(1, 61))
        transforms = [r.get("transform") or {} for r in results]
        fallbacks = sum(t.get("merge_fallbacks") or 0 for t in transforms)
        retries = sum(t.get("merge_remote_retries") or 0 for t in transforms)
        recycled = sum(t.get("merge_connections_recycled") or 0 for t in transforms)
        servers = [s for s in (record.get("server_statistics") or {}).values() if s]
        refused = sum(s.get("drain_refused") or 0 for s in servers)
        complete = all(s.get("drain_complete") for s in servers)
        change = after / before - 1 if before and after else None
        held = held and fallbacks == 0 and change is not None and change >= 0.10 and \
            all(code == 0 for code in record["exit_codes"].values())
        print(f"| {record['repetition']} | {fmt(before, scale=1e3, suffix='k')} | {fmt(after, scale=1e3, suffix='k')} | "
              f"{fmt(change and change * 100, 1, suffix='%')} | {fallbacks} | {retries} | {recycled} | {refused} | "
              f"{complete} |")
    print(f"\n- **Z4 {'supported' if held else 'not supported'}** (zero fallbacks and >= 10% more writes/s after "
          f"scale-out, in every repetition).")


def c4(directory: Path, aks: dict) -> None:
    vms = load(directory / "c4-vm.jsonl")
    if not vms or not aks:
        return
    print("\n## C4: AKS against plain VMs (N = 4, backlog-inclusive writes/s per database)\n")
    print("| Design | AKS | Plain VMs | AKS / VMs |")
    print("|---|---|---|---|")
    held = True
    for (design, n), r in sorted(vms.items()):
        k = aks.get((design, n))
        ratio = k["with_backlog"] / r["with_backlog"] if k else None
        held = held and ratio is not None and abs(ratio - 1) <= 0.05
        print(f"| {design} | {fmt(k and k['with_backlog'], scale=1e3, suffix='k')} | "
              f"{fmt(r['with_backlog'], scale=1e3, suffix='k')} | {fmt(ratio, 3)} |")
    print(f"\n- **Z5 {'supported' if held else 'not supported'}** (within 5% for every design).")


def hypotheses(rows: dict) -> None:
    if not rows:
        return
    service = next((d for d, _ in rows if d.startswith("service")), None)
    print("\n## Hypotheses Z1, Z2\n")
    s1, n1 = rows.get((service, 1)), rows.get(("native", 1))
    if s1 and n1:
        ratio = s1["with_backlog"] / n1["with_backlog"]
        held = ratio >= 1.3 and all(f == 0 for f in s1["fallbacks"])
        print(f"- **Z1 {'supported' if held else 'not supported'}:** N = 1, {fmt(s1['with_backlog'], scale=1e3, suffix='k')}"
              f" against RocksDB's {fmt(n1['with_backlog'], scale=1e3, suffix='k')}, {ratio:.2f}x (threshold 1.3x).")
    n_max = max(n for d, n in rows if d == service)
    s6, n6 = rows.get((service, n_max)), rows.get(("native", n_max))
    if s6 and n6:
        ratio = s6["with_backlog"] / n6["with_backlog"]
        clean = all(f == 0 for (d, _), r in rows.items() if d == service for f in r["fallbacks"])
        held = ratio >= 1.0 and clean
        print(f"- **Z2 {'supported' if held else 'not supported'}:** N = {n_max}, {ratio:.2f}x RocksDB "
              f"(threshold 1.0x); fallbacks in every service trial zero: {clean}.")


def main() -> int:
    directory = Path(sys.argv[1]) if len(sys.argv) > 1 else DATA
    c0(directory)
    rows = c1(directory)
    hypotheses(rows)
    c2(directory)
    c3(directory)
    c4(directory, rows)
    c5(rows)
    return 0


if __name__ == "__main__":
    sys.exit(main())
