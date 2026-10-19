#!/usr/bin/env python3
"""Summarize `perf script` output into CPU time per process, thread name, and category.

Usage: perf script -i perf.data -F pid,tid,comm,dso,sym | profile_summary.py PID[,PID...] FREQ_HZ > out.json
Each sample approximates 1/FREQ_HZ seconds of on-CPU time.
"""

from __future__ import annotations

import json
import re
import sys
from collections import Counter, defaultdict

CATEGORIES = [
    ("lz4", re.compile(r"LZ4", re.I)),
    ("crc32c", re.compile(r"crc32c", re.I)),
    ("cuda_runtime_driver", re.compile(r"libcuda|libcudart|nvcomp|libnvidia", re.I)),
    ("memcpy", re.compile(r"memcpy|memmove|__memset|memset", re.I)),
    ("kernel", re.compile(r"\[kernel|kallsyms", re.I)),
    ("grpc_protobuf", re.compile(r"grpc|protobuf|absl", re.I)),
    ("rocksdb", re.compile(r"rocksdb|lsm_lab", re.I)),
]


def category(dso: str, sym: str) -> str:
    text = f"{dso} {sym}"
    for name, pattern in CATEGORIES:
        if pattern.search(text):
            return name
    return "other"


def main() -> None:
    pids = {int(p) for p in sys.argv[1].split(",")}
    hz = float(sys.argv[2])
    threads: dict = defaultdict(Counter)
    categories: dict = defaultdict(Counter)
    thread_categories: dict = defaultdict(lambda: defaultdict(Counter))
    symbols: dict = defaultdict(Counter)
    thread_ids: dict = defaultdict(lambda: defaultdict(set))
    line_pattern = re.compile(r"^\s*(?P<comm>.+?)\s+(?P<pid>\d+)/(?P<tid>\d+)\s+(?P<rest>.*)$")
    for line in sys.stdin:
        match = line_pattern.match(line)
        if not match:
            continue
        pid = int(match["pid"])
        if pid not in pids:
            continue
        rest = match["rest"].strip()
        dso_match = re.search(r"\(([^)]*)\)\s*$", rest)
        dso = dso_match.group(1) if dso_match else ""
        sym = rest[: dso_match.start()].strip() if dso_match else rest
        sym = re.sub(r"^[0-9a-f]+\s+", "", sym)
        sym = re.sub(r"\+0x[0-9a-f]+$", "", sym)
        comm = re.sub(r"\d+$", "", match["comm"].strip())
        cat = category(dso, sym)
        threads[pid][comm] += 1
        thread_ids[pid][comm].add(int(match["tid"]))
        categories[pid][cat] += 1
        thread_categories[pid][comm][cat] += 1
        symbols[pid][f"{comm} | {sym} ({dso.split('/')[-1]})"] += 1
    out = {}
    for pid in pids:
        total = sum(threads[pid].values())
        out[str(pid)] = {
            "samples": total,
            "cpu_seconds": total / hz,
            "threads_cpu_s": {k: v / hz for k, v in threads[pid].most_common()},
            "categories_cpu_s": {k: v / hz for k, v in categories[pid].most_common()},
            "thread_categories_cpu_s": {t: {k: v / hz for k, v in c.most_common()}
                                        for t, c in thread_categories[pid].items()},
            "top_symbols_cpu_s": {k: v / hz for k, v in symbols[pid].most_common(40)},
            "distinct_thread_ids": {k: len(v) for k, v in thread_ids[pid].items()},
        }
    json.dump(out, sys.stdout, indent=1)


if __name__ == "__main__":
    main()
