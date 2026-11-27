#!/usr/bin/env python3
"""N6: device-level write MB/s and %util from iostat (busiest nvme device), aligned to each trial's
measured window by sample index, compared with the benchmark's WA x user MiB/s."""
import json
import os
DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data")
import statistics

D = os.path.join(DATA, "malphite-nvme/followup/n6")
rows = [json.loads(l) for l in open(os.path.join(D, "results.jsonl"))]
for row in rows:
    idx = row["index"]
    path = os.path.join(D, f"trial-{idx:04d}", "iostat.txt")
    if not os.path.exists(path):
        continue
    samples = {}
    header = None
    for line in open(path):
        parts = line.split()
        if not parts:
            continue
        if parts[0] == "Device":
            header = parts
            continue
        if header and parts[0].startswith("nvme"):
            rec = dict(zip(header, parts))
            samples.setdefault(parts[0], []).append((float(rec["wMB/s"]), float(rec["rMB/s"]), float(rec["%util"])))
    dev = max(samples, key=lambda d: sum(s[0] for s in samples[d]))
    s = samples[dev]
    # iostat runs 1 s samples from trial start; first sample is since boot. Take the samples whose
    # write rate is non-trivial and use the middle 300 (window ~ seconds 121..420).
    s = s[1:]
    win = s[121:421] if len(s) >= 421 else s
    r = row["result"]
    user = r["throughput"]["user_mib_per_second"]
    w_med = statistics.median(x[0] for x in win)
    print(f"trial {idx} {row['trial']['design']:6s} dev={dev} n={len(s)} window wMB/s median={w_med:.0f} "
          f"mean={statistics.mean(x[0] for x in win):.0f} util median={statistics.median(x[2] for x in win):.0f}% "
          f"rMB/s={statistics.median(x[1] for x in win):.0f} | user MiB/s={user:.1f} WA={r['write_amplification']:.2f} "
          f"WA*user(MB/s)={r['write_amplification']*user*1.048576:.0f} device/user={statistics.mean(x[0] for x in win)/(user*1.048576):.2f}")
