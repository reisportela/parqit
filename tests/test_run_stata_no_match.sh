#!/usr/bin/env bash
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/parqit_runner_test.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT
: > "$TMP/parqit.plugin"
TRUE_BIN="$(command -v true)"

set +e
STATA="$TRUE_BIN" BUILD_DIR="$TMP" \
    bash "$REPO/tests/run_stata.sh" __parqit_test_name_that_cannot_exist__ \
    > "$TMP/output.txt" 2>&1
rc=$?
set -e

if [ "$rc" -ne 2 ]; then
    sed -n '1,160p' "$TMP/output.txt"
    echo "expected run_stata.sh to return 2 for an unmatched filter; got $rc" >&2
    exit 1
fi
grep -q "no Stata tests matched" "$TMP/output.txt"

# A stale/early PASS must not mask a later uncaptured Stata abort. The fake
# executable writes exactly the log shape batch Stata produces in that case.
FAKE="$TMP/fake-stata"
printf '%s\n' '#!/usr/bin/env bash' \
    'wrapper="${3}"' \
    'log="${wrapper%.do}.log"' \
    'printf "%s\n" "VERDICT(FAKE): PASS" "r(9);" > "$log"' \
    'exit 0' > "$FAKE"
chmod +x "$FAKE"

set +e
STATA="$FAKE" BUILD_DIR="$TMP" \
    bash "$REPO/tests/run_stata.sh" m0_smoke > "$TMP/aborted.txt" 2>&1
rc=$?
set -e

if [ "$rc" -eq 0 ]; then
    sed -n '1,160p' "$TMP/aborted.txt"
    echo "run_stata.sh accepted a PASS followed by an uncaptured r(9)" >&2
    exit 1
fi
grep -q "SCRIPT ABORTED AFTER VERDICT" "$TMP/aborted.txt"

# Each selected Stata invocation receives a private runner-owned TMPDIR, which
# must be removed afterwards even when tests create persistent fixture state.
printf '%s\n' '#!/usr/bin/env bash' \
    'wrapper="${3}"' \
    'log="${wrapper%.do}.log"' \
    'printf "%s\n" "$TMPDIR" > "$PARQIT_TMP_RECORD"' \
    'printf "%s\n" fixture > "$TMPDIR/persistent-fixture"' \
    'printf "%s\n" "VERDICT(FAKE): PASS" > "$log"' \
    'exit 0' > "$FAKE"
chmod +x "$FAKE"

set +e
PARQIT_TMP_RECORD="$TMP/tmp_record.txt" STATA="$FAKE" BUILD_DIR="$TMP" \
    bash "$REPO/tests/run_stata.sh" m0_smoke > "$TMP/isolated.txt" 2>&1
rc=$?
set -e

if [ "$rc" -ne 0 ]; then
    sed -n '1,160p' "$TMP/isolated.txt"
    echo "runner temp-isolation probe did not finish successfully" >&2
    exit 1
fi
test_tmp="$(sed -n '1p' "$TMP/tmp_record.txt")"
if [ -z "$test_tmp" ] || [ -e "$test_tmp" ]; then
    echo "runner did not remove its per-test TMPDIR: $test_tmp" >&2
    exit 1
fi

# A process crash can leave a PASS in the log without a Stata r(#) footer.
printf '%s\n' '#!/usr/bin/env bash' \
    'wrapper="${3}"' \
    'log="${wrapper%.do}.log"' \
    'printf "%s\n" "VERDICT(FAKE): PASS" > "$log"' \
    'exit 139' > "$FAKE"

set +e
STATA="$FAKE" BUILD_DIR="$TMP" \
    bash "$REPO/tests/run_stata.sh" m0_smoke > "$TMP/crashed.txt" 2>&1
rc=$?
set -e
if [ "$rc" -eq 0 ]; then
    cat "$TMP/crashed.txt"
    echo "runner accepted PASS followed by process exit 139" >&2
    exit 1
fi
grep -q "PROCESS EXIT 139" "$TMP/crashed.txt"

# Exercise both concurrent dispatches with synthetic wrappers: no Stata license
# or real subprocess contention is needed to test the runner's status handling.
mkdir -p "$TMP/concurrent_repo/tests/concurrent"
cp "$REPO/tests/run_stata.sh" "$TMP/concurrent_repo/tests/run_stata.sh"
for base in x01_bridge_xproc x02_output_xproc; do
    printf '%s\n' '#!/usr/bin/env bash' \
        'printf "%s\n" "VERDICT(FAKE_CONCURRENT): PASS"' \
        'exit 139' > "$TMP/concurrent_repo/tests/concurrent/$base.sh"
    set +e
    STATA="$TRUE_BIN" BUILD_DIR="$TMP" \
        bash "$TMP/concurrent_repo/tests/run_stata.sh" "$base" > "$TMP/$base.txt" 2>&1
    rc=$?
    set -e
    if [ "$rc" -eq 0 ]; then
        cat "$TMP/$base.txt"
        echo "runner accepted $base PASS followed by process exit 139" >&2
        exit 1
    fi
    grep -q "PROCESS EXIT 139" "$TMP/$base.txt"
done
