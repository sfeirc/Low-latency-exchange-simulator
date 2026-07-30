#!/usr/bin/env bash
# Builds in Release and runs every bench_* binary pinned to a single
# isolated physical core, writing JSON results to artifacts/. This is
# the actual command that produced every number in docs/benchmarks.md —
# running it again is how to check they still hold on different hardware
# or after a change, not just take them on faith.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build-bench"
ARTIFACTS_DIR="${ROOT_DIR}/artifacts"

# A physical core with no hyperthread sibling contention from the rest of
# this script/shell — see docs/tradeoffs.md's SPSC section for why that
# distinction mattered enough to change a benchmark's conclusion once
# already. Override with `TASKSET_CORE=N tools/run_benchmarks.sh` on a
# machine with a different topology (check `lscpu -e` first).
CORE="${TASKSET_CORE:-5}"

echo "==> Configuring Release build (native arch, no sanitizers) in ${BUILD_DIR}"
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DJANE_NATIVE_ARCH=ON \
  -DJANE_ENABLE_LTO=ON \
  -DJANE_BUILD_TESTS=OFF \
  -DJANE_BUILD_BENCH=ON >/dev/null

echo "==> Building"
cmake --build "${BUILD_DIR}" -j"$(nproc)" >/dev/null

mkdir -p "${ARTIFACTS_DIR}"

for bin in "${BUILD_DIR}"/bench/bench_*; do
  [ -x "${bin}" ] || continue
  [[ "${bin}" == *.cmake ]] && continue
  name="$(basename "${bin}")"
  echo "==> Running ${name} (pinned to core ${CORE})"
  # --benchmark_out_format (the *file*) is json; --benchmark_format (the
  # console) is left at its human-readable default — otherwise both the
  # file and this script's own terminal output end up as a JSON dump.
  taskset -c "${CORE}" "${bin}" \
    --benchmark_out="${ARTIFACTS_DIR}/${name}.json" \
    --benchmark_out_format=json
done

echo "==> Done. Results in ${ARTIFACTS_DIR}/*.json"
echo "    Human-readable summary: python3 ${ROOT_DIR}/tools/analyze_benchmarks.py ${ARTIFACTS_DIR}/*.json"
