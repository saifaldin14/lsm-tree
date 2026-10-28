#!/usr/bin/env python3
"""Adds YCSB's maxexecutiontime to gParaKV's ycsbc driver (applied by setup.sh).

With maxexecutiontime=S in the workload file, the transaction phase stops after S
seconds, and throughput is completed transactions / elapsed time (the original divided the
configured operation count). Loading is unchanged. Idempotent.
"""
import sys
from pathlib import Path

path = Path(sys.argv[1])
source = path.read_text()
if "maxexecutiontime" in source:
    sys.exit(0)

replacements = [
    ("int DelegateClient(ycsbc::DB *db, ycsbc::CoreWorkload *wl, size_t num_ops,\n                   bool is_loading) {",
     "int DelegateClient(ycsbc::DB *db, ycsbc::CoreWorkload *wl, size_t num_ops,\n                   bool is_loading, double max_seconds = 0) {\n"
     "  const auto deadline = std::chrono::steady_clock::now() +\n"
     "                        std::chrono::duration_cast<std::chrono::steady_clock::duration>(\n"
     "                            std::chrono::duration<double>(max_seconds));"),
    ("  for (size_t i = 0; i < num_ops; ++i) {\n",
     "  for (size_t i = 0; i < num_ops; ++i) {\n"
     "    // maxexecutiontime: check the clock every 1024 operations.\n"
     "    if (max_seconds > 0 && (i & 1023) == 0 && std::chrono::steady_clock::now() >= deadline)\n"
     "      break;\n"),
    ("  size_t sum_transaction = DelegateClient(db, &wl, num_ops, false);",
     "  const double max_seconds = std::stod(props.GetProperty(\"maxexecutiontime\", \"0\"));\n"
     "  size_t sum_transaction = DelegateClient(db, &wl, num_ops, false, max_seconds);"),
    ("       << static_cast<double>(num_ops) / duration / 1000 << endl;",
     "       << static_cast<double>(sum_transaction) / duration / 1000 << endl;"),
]
for old, new in replacements:
    if source.count(old) != 1:
        sys.exit(f"patch anchor not found exactly once: {old[:60]!r}")
    source = source.replace(old, new)
if "#include <chrono>" not in source:
    source = "#include <chrono>\n" + source
path.write_text(source)
