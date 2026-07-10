#!/usr/bin/env bash
# Repro: a passing Stata test must not leave a directory derived from tempfile.
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
PLUGIN="${1:-$REPO/build/dev/parqit.plugin}"
ROOT="$(mktemp -d "${TMPDIR:-/tmp}/parqit_t02_fixture_repro.XXXXXX")"
WORK="$ROOT/work"
STATA_TMP="$ROOT/stata_tmp"
mkdir -p "$WORK" "$STATA_TMP"
trap 'rm -rf "$ROOT"' EXIT

(
    cd "$WORK" || exit 1
    TMPDIR="$STATA_TMP" stata-mp -b do \
        "$REPO/tests/integration/t02_use_options.do" "$REPO" "$PLUGIN"
)

if ! rg -q '^VERDICT\(T02_USE_OPTIONS\): PASS' "$WORK"/*.log; then
    echo "VERDICT(REPRO_T02_FIXTURE_LEAK): FAIL - t02 did not finish" >&2
    exit 1
fi

leak="$(find "$STATA_TMP" -mindepth 1 -maxdepth 1 -type d -name '*_d' -print -quit)"
if [ -n "$leak" ]; then
    echo "leaked fixture directory: $leak" >&2
    echo "VERDICT(REPRO_T02_FIXTURE_LEAK): FAIL - passing t02 left persistent scratch state"
    exit 1
fi

echo "VERDICT(REPRO_T02_FIXTURE_LEAK): PASS - passing t02 cleans its fixture directory"
