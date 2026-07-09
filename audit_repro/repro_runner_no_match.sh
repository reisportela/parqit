#!/usr/bin/env bash
# Repro: the Stata test runner must fail when a typo selects zero tests.
set -u
repo="$(cd "$(dirname "$0")/.." && pwd)"

set +e
STATA="${STATA:-stata-mp}" BUILD_DIR="${BUILD_DIR:-$repo/build/dev}" \
    bash "$repo/tests/run_stata.sh" __CODEX_FILTER_THAT_MATCHES_NO_TEST__
rc=$?
set -e

if [ "$rc" -eq 0 ]; then
    echo "VERDICT(REPRO_RUNNER_NO_MATCH): FAIL - zero selected tests returned success"
    exit 1
fi
echo "VERDICT(REPRO_RUNNER_NO_MATCH): PASS - zero selected tests failed loudly (rc=$rc)"
