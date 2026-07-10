#!/usr/bin/env bash
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${1:-$REPO/build/dev/parqit_tests}"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/parqit_unit_concurrent.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

if [ ! -x "$BIN" ]; then
    echo "unit binary not executable: $BIN" >&2
    exit 2
fi

# Independent CTest jobs (or two local agents) may run this binary at the same
# time. Stress the test that writes Parquet so every process must own its scratch
# path; a shared /tmp/parqit_arrowcap.parquet makes at least one COPY fail.
pids=()
for i in 1 2 3 4 5 6 7 8; do
    "$BIN" --test-case="arrow ingestion capability (save path)" \
        > "$TMP/$i.log" 2>&1 &
    pids+=("$!")
done

fail=0
for i in 1 2 3 4 5 6 7 8; do
    if ! wait "${pids[$((i - 1))]}"; then
        echo "concurrent unit process $i failed:" >&2
        sed -n '1,160p' "$TMP/$i.log" >&2
        fail=1
    fi
done
exit "$fail"
