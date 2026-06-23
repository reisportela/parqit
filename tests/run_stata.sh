#!/usr/bin/env bash
# Runs parqit's Stata test suites in clean batch processes and prints a verdict
# summary. CI cannot run these (no Stata license on runners); run locally:
#
#   bash tests/run_stata.sh                 # all suites
#   bash tests/run_stata.sh m0_smoke        # by name fragment
#   STATA=... BUILD_DIR=... bash tests/run_stata.sh
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
STATA="${STATA:-/usr/local/stata/stata-mp}"
BUILD_DIR="${BUILD_DIR:-$REPO/build/dev}"
PLUGIN="$BUILD_DIR/parqit.plugin"
FILTER="${1:-}"

if [ ! -x "$STATA" ] && ! command -v "$STATA" >/dev/null 2>&1; then
    echo "error: Stata not found at '$STATA' (set STATA=...)" >&2
    exit 2
fi
if [ ! -f "$PLUGIN" ]; then
    echo "error: plugin not found at $PLUGIN (build first: cmake --preset dev && cmake --build build/dev --target parqit_plugin)" >&2
    exit 2
fi

RUNDIR="$(mktemp -d "${TMPDIR:-/tmp}/parqit_tests.XXXXXX")"
cd "$RUNDIR"
echo "running in $RUNDIR"

declare -a logs=()
for suite in integration verify_suite roundtrip; do
    for f in "$REPO/tests/$suite"/*.do; do
        [ -e "$f" ] || continue
        base="$(basename "$f" .do)"
        case "$base" in
            _*) continue ;; # helper do-files, not standalone tests
        esac
        if [ -n "$FILTER" ] && [[ "$base" != *"$FILTER"* ]]; then continue; fi
        echo "running $suite/$base ..."
        # Batch Stata names its log after the last token on the command line
        # and exits 0 regardless of errors: run through a same-named wrapper
        # so the log is <test>.log, and judge by VERDICT lines only.
        printf 'do "%s" "%s" "%s"\n' "$f" "$REPO" "$PLUGIN" > "$RUNDIR/$base.do"
        "$STATA" -b do "$RUNDIR/$base.do"
        logs+=("$RUNDIR/$base.log")
    done
done

echo
echo "================ VERDICT SUMMARY ================" | tee VERDICTS_SUMMARY.txt
fail=0
for log in "${logs[@]}"; do
    name="$(basename "$log" .log)"
    if [ -f "$log" ]; then
        verdicts="$(grep -h "^VERDICT" "$log" || true)"
        if [ -n "$verdicts" ]; then
            echo "$verdicts" | tee -a VERDICTS_SUMMARY.txt
            # A log passes only with >=1 PASS verdict AND zero FAIL verdicts.
            # (Multi-invariant do-files can print several VERDICT lines; a later
            # PASS must never mask an earlier FAIL.)
            if grep -qE "^VERDICT\(.*\): *FAIL" "$log"; then
                fail=1
            elif ! grep -qE "^VERDICT\(.*\): *PASS" "$log"; then
                fail=1
            fi
        else
            echo "VERDICT($name): *** SCRIPT DID NOT FINISH - inspect $log ***" | tee -a VERDICTS_SUMMARY.txt
            fail=1
        fi
    else
        echo "VERDICT($name): *** NO LOG PRODUCED ***" | tee -a VERDICTS_SUMMARY.txt
        fail=1
    fi
done
echo "=================================================" | tee -a VERDICTS_SUMMARY.txt
echo "logs in $RUNDIR"
exit $fail
