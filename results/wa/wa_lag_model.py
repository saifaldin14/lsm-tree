#!/usr/bin/env python3
"""Per-trial table and pooled regressions: does compaction lag (L0 depth, pending bytes) explain WA,
and what design residual remains? Read-only over the repository."""
import json
import math
import os
DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data")
import statistics

import numpy as np

REPO = DATA
CAMPAIGNS = {
    "N6": "malphite-nvme/followup/n6/results.jsonl",
    "N1": "malphite-nvme/followup/n1/results.jsonl",
    "confirm": "malphite-nvme/retune/confirm/results.jsonl",
    "tune": "malphite-nvme/retune/tune/results.jsonl",
    "x8": "malphite-nvme/x8/out/results.jsonl",
    "h31": "malphite-nvme/h31/results.jsonl",
    "ssd": "v3/v3-ssd/results.jsonl",
    "trig": "wa-trigger/run/results.jsonl",
    "trig16": "wa-trigger/run16/results.jsonl",
    "W": "wa-trigger/w/results.jsonl",
}


def l0_drops(tl):
    """From 1 s samples: count of seconds where L0 fell and mean files removed per such second."""
    drops = []
    for a, b in zip(tl, tl[1:]):
        d = b["l0_files"] - a["l0_files"]
        if d < 0:
            drops.append(-d)
    return len(drops), (statistics.mean(drops) if drops else 0.0)


rows = []
for camp, path in CAMPAIGNS.items():
    for line in open(os.path.join(REPO, path)):
        row = json.loads(line)
        r = row.get("result")
        if row.get("exit_code") != 0 or not r:
            continue
        c = r["config"]
        tl = [s for s in r["timeline"] if s["phase"] == "measurement"]
        w = r["window_counters"]
        writes = r["throughput"]["writes_per_second"] * r["window"]["seconds"]
        ndrop, per_drop = l0_drops(tl)
        rows.append({
            "camp": camp,
            "design": row["trial"]["design"],
            "cell": row["trial"].get("cell", ""),
            "cores": row["trial"].get("cores"),
            "trigger": c.get("l0_trigger") or 4,
            "sub": c["subcompactions"],
            "dio": c["use_direct_io"],
            "wps": r["throughput"]["writes_per_second"] / 1e6,
            "WA": r["write_amplification"],
            "cw_u": w["compact_write_bytes"] / w["user_bytes_written"],
            "rec_w": w["compactions"]["input_records"] / writes,
            "L0avg": statistics.mean(s["l0_files"] for s in tl),
            "pendGB": statistics.mean(s["pending_compaction_bytes"] for s in tl) / 1e9,
            "l0_drop_secs_per_min": ndrop / (len(tl) / 60),
            "l0_files_per_drop": per_drop,
            "bm": c.get("bottommost_compression", ""),
        })

json.dump(rows, open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "wa_trials.json"), "w"), indent=1)


def ols(X, y):
    X = np.asarray(X, float)
    y = np.asarray(y, float)
    beta, res, rank, _ = np.linalg.lstsq(X, y, rcond=None)
    pred = X @ beta
    ss_res = float(((y - pred) ** 2).sum())
    ss_tot = float(((y - y.mean()) ** 2).sum())
    n, k = X.shape
    sigma2 = ss_res / max(1, n - k)
    cov = sigma2 * np.linalg.pinv(X.T @ X)
    se = np.sqrt(np.diag(cov))
    return beta, se, 1 - ss_res / ss_tot, n


def report(name, sel):
    d = [r for r in rows if sel(r)]
    if len(d) < 5:
        return
    y = [math.log(r["cw_u"]) for r in d]
    full = [1.0 if r["design"] == "full" else 0.0 for r in d]
    # Model 0: design only
    b0, s0, r20, n = ols([[1, f] for f in full], y)
    # Model 1: design + log(L0avg)
    b1, s1, r21, _ = ols([[1, f, math.log(r["L0avg"])] for f, r in zip(full, d)], y)
    # Model 2: design + log(L0avg) + log(pendGB)
    b2, s2, r22, _ = ols([[1, f, math.log(r["L0avg"]), math.log(max(r["pendGB"], 0.05))] for f, r in zip(full, d)], y)
    # Model 3: design + log(L0avg/trigger) (L0 depth relative to trigger) + log(trigger)
    b3, s3, r23, _ = ols([[1, f, math.log(r["L0avg"]), math.log(r["trigger"])] for f, r in zip(full, d)], y)
    pct = lambda b: (math.exp(b) - 1) * 100
    print(f"\n## {name}: n={n} trials")
    print(f"  M0 design only:            full effect {pct(b0[1]):+.1f}% (se {s0[1]*100:.1f}%), R2={r20:.2f}")
    print(f"  M1 + log L0avg:            full effect {pct(b1[1]):+.1f}% (se {s1[1]*100:.1f}%), L0 elasticity {b1[2]:+.2f} (se {s1[2]:.2f}), R2={r21:.2f}")
    print(f"  M2 + log L0avg + log pend: full effect {pct(b2[1]):+.1f}% (se {s2[1]*100:.1f}%), L0 {b2[2]:+.2f}, pend {b2[3]:+.2f} (se {s2[3]:.2f}), R2={r22:.2f}")
    print(f"  M3 + log L0avg + log trig: full effect {pct(b3[1]):+.1f}% (se {s3[1]*100:.1f}%), L0 {b3[2]:+.2f}, trig {b3[3]:+.2f}, R2={r23:.2f}")


if __name__ == "__main__":
    print("camp design cell cores trig sub dio | Mw/s WA cw/u rec/w L0avg pendGB dropsec/min files/drop")
    for r in sorted(rows, key=lambda r: (r["camp"], r["design"], r["trigger"], r["cell"], r["sub"])):
        print(f"{r['camp']} {r['design']} {r['cell'] or '-'} {r['cores']} {r['trigger']} {r['sub']} {int(r['dio'])} | "
              f"{r['wps']:.3f} {r['WA']:.2f} {r['cw_u']:.2f} {r['rec_w']:.2f} {r['L0avg']:.1f} {r['pendGB']:.2f} "
              f"{r['l0_drop_secs_per_min']:.1f} {r['l0_files_per_drop']:.2f}")
    nvme8_default = lambda r: r["camp"] in ("N6", "confirm", "tune", "x8") or (r["camp"] == "h31" and r["cell"] == "BZ")
    report("malphite NVMe 8 cores, default trigger (N6, confirm, tune, X8, H31-BZ)", nvme8_default)
    report("malphite all (incl. N1 tmpfs, H31 all cells)", lambda r: r["camp"] in ("N6", "N1", "confirm", "tune", "x8", "h31"))
    report("zyra trigger grids (run, run16, W)", lambda r: r["camp"] in ("trig", "trig16", "W"))
    report("SATA v3-ssd", lambda r: r["camp"] == "ssd")
    report("everything", lambda r: True)
