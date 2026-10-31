#!/usr/bin/env python3
"""Summarize per-run medians; never pool percentiles across runs."""

import collections
import csv
import statistics
import sys

groups = collections.defaultdict(list)
with open(sys.argv[1] if len(sys.argv) > 1 else "docs/benchmark-results.csv", newline="") as source:
    for row in csv.DictReader(source):
        groups[(row["scenario"], row["mode"], int(row["threads"]))].append(row)
print(
    "| Scenario | Mode | Threads | Runs | Median ops/s | Min–max ops/s | p50 ns | p95 ns | p99 ns |"
)
print("|---|---|---:|---:|---:|---:|---:|---:|---:|")
for (scenario, mode, threads), rows in sorted(groups.items()):
    rates = [float(row["ops_per_second"]) for row in rows]
    percentiles = [
        statistics.median(float(row[key]) for row in rows) for key in ("p50_ns", "p95_ns", "p99_ns")
    ]
    print(
        f"| {scenario} | {mode} | {threads} | {len(rows)} | {statistics.median(rates):,.0f} | "
        f"{min(rates):,.0f}–{max(rates):,.0f} | "
        + " | ".join(f"{p:,.0f}" for p in percentiles)
        + " |"
    )
