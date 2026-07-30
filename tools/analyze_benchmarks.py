#!/usr/bin/env python3
"""Prints a compact, human-readable summary of Google Benchmark JSON
output produced by tools/run_benchmarks.sh — the one place Python
appears in this project, exactly as scoped (analysis only, never in the
matching/risk/market-data path itself).

Usage: python3 tools/analyze_benchmarks.py artifacts/*.json
"""

import json
import sys

# Fields Google Benchmark always includes that aren't benchmark-specific
# results (custom counters, on the other hand, vary per bench_*.cpp file
# and are exactly what's interesting to print).
_STANDARD_FIELDS = {
    "name", "family_index", "per_family_instance_index", "run_name",
    "run_type", "repetitions", "repetition_index", "threads", "iterations",
    "real_time", "cpu_time", "time_unit", "aggregate_name",
}


def format_number(value: float) -> str:
    if value == int(value) and abs(value) < 1e15:
        return f"{int(value):,}"
    return f"{value:,.3f}"


def main() -> int:
    paths = sys.argv[1:]
    if not paths:
        print("usage: analyze_benchmarks.py artifacts/*.json", file=sys.stderr)
        return 1

    for path in paths:
        with open(path) as f:
            data = json.load(f)

        context = data.get("context", {})
        print(f"\n=== {path} ===")
        if "host_name" in context:
            print(f"  host: {context['host_name']}  cpus: {context.get('num_cpus', '?')}")

        for bench in data.get("benchmarks", []):
            if bench.get("run_type") == "aggregate":
                continue  # skip mean/median/stddev rows; the "iteration" rows have the real data
            name = bench["name"]
            time_unit = bench.get("time_unit", "ns")
            real_time = bench.get("real_time")
            line = f"  {name:<55}"
            if real_time is not None:
                line += f" {real_time:>10.3f} {time_unit}"

            counters = {k: v for k, v in bench.items() if k not in _STANDARD_FIELDS}
            if counters:
                parts = [f"{k}={format_number(v)}" for k, v in counters.items()]
                line += "  |  " + "  ".join(parts)
            print(line)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
