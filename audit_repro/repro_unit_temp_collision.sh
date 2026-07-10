#!/usr/bin/env bash
# Repro: independent parqit_tests processes must not share fixed scratch files.
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${1:-$REPO/build/dev/parqit_tests}"

if bash "$REPO/tests/test_unit_concurrent.sh" "$BIN"; then
    echo "VERDICT(REPRO_UNIT_TEMP_COLLISION): PASS - concurrent unit processes own distinct scratch paths"
    exit 0
fi

echo "VERDICT(REPRO_UNIT_TEMP_COLLISION): FAIL - concurrent unit processes collided in shared scratch state"
exit 1
