#!/usr/bin/env bash
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/parqit_runner_repro.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT
: > "$TMP/parqit.plugin"

FAKE="$TMP/fake-stata"
printf '%s\n' '#!/usr/bin/env bash' \
    'wrapper="${3}"' \
    'log="${wrapper%.do}.log"' \
    'printf "%s\n" "VERDICT(FAKE): PASS" "r(9);" > "$log"' \
    'exit 0' > "$FAKE"
chmod +x "$FAKE"

set +e
STATA="$FAKE" BUILD_DIR="$TMP" bash "$REPO/tests/run_stata.sh" m0_smoke \
    > "$TMP/output.txt" 2>&1
rc=$?
set -e

if [ "$rc" -eq 0 ]; then
    echo "VERDICT(REPRO_RUNNER_PASS_THEN_ABORT): FAIL - PASS before a later r(9) was accepted"
    exit 1
fi
echo "VERDICT(REPRO_RUNNER_PASS_THEN_ABORT): PASS - runner rejected the aborted log"
