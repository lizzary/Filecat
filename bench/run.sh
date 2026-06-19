#!/usr/bin/env bash
# Run every Filecat benchmark in sequence and dump the environment for
# reproducibility. Assumes the binaries are already built into ./build
# (configure with -DFILECAT_BUILD_BENCH=ON).
#
# Usage:
#   bench/run.sh                       # uses ./build
#   BENCH_BUILD_DIR=build-foo bench/run.sh

set -u

BUILD_DIR="${BENCH_BUILD_DIR:-build}"

# .exe on Windows (git-bash); empty everywhere else.
EXE=""
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) EXE=".exe" ;;
esac

bins=(bench_throughput bench_latency bench_rss bench_open)

# Sanity: refuse to silently report nothing if the build is missing.
for b in "${bins[@]}"; do
    if [[ ! -x "${BUILD_DIR}/${b}${EXE}" ]]; then
        echo "missing: ${BUILD_DIR}/${b}${EXE}" >&2
        echo "  configure with: cmake -B ${BUILD_DIR} -DFILECAT_BUILD_BENCH=ON -DCMAKE_BUILD_TYPE=Release" >&2
        echo "  build with:     cmake --build ${BUILD_DIR} -j" >&2
        exit 1
    fi
done

echo "=== Filecat benchmark run: $(date -u +%Y-%m-%dT%H:%M:%SZ) ==="
echo "uname: $(uname -srm)"
if command -v lscpu >/dev/null 2>&1; then
    cpu="$(lscpu | awk -F: '/^Model name/ {gsub(/^[ \t]+/, "", $2); print $2; exit}')"
elif command -v sysctl >/dev/null 2>&1; then
    cpu="$(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo unknown)"
else
    cpu="unknown"
fi
echo "cpu:   ${cpu}"
echo

for b in "${bins[@]}"; do
    echo "--- ${b} ---"
    "${BUILD_DIR}/${b}${EXE}"
    echo
done
