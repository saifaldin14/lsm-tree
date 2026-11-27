#!/usr/bin/env python3
"""One-row-per-cell compact table from wa_medians.json (run wa_decompose.py first)."""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
data = json.load(open(os.path.join(HERE, "wa_medians.json")))
COLS = [("writes/s (M)", "Mw/s"), ("WA window", "WA"), ("WA ingest+drain", "WAid"), ("flush/user", "fl/u"),
        ("compact/user", "cw/u"), ("in records/user write", "rec/w"), ("comp out/in", "o/i"),
        ("B/in record", "B/rec"), ("live SST B/entry", "B/ent"), ("jobs per GB user", "jobs/GB"),
        ("MB in/job", "MBin/j"), ("L0 files mean", "L0avg"), ("L0 files p90", "L0p90"),
        ("running comp mean", "runC"), ("pending comp GB mean", "pendGB"), ("imm memtables mean", "imm"),
        ("stall frac", "stall"), ("MB/file end (L>0)", "MB/f"), ("files end", "files"), ("n", "n")]
print("campaign | cell | " + " | ".join(c[1] for c in COLS))
for camp, cells in data.items():
    for cell, m in cells.items():
        vals = []
        for k, _ in COLS:
            v = m.get(k)
            vals.append("-" if v is None else (f"{v:.0f}" if k in ("n", "files end") else f"{v:.2f}" if abs(v) >= 1 or v == 0 else f"{v:.3f}"))
        print(f"{camp} | {cell} | " + " | ".join(vals))
