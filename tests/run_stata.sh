#!/usr/bin/env bash
# Runs parqit's Stata test suites in clean batch processes and prints a verdict
# summary. CI cannot run these (no Stata license on runners); run locally:
#
#   bash tests/run_stata.sh                 # all suites
#   bash tests/run_stata.sh m0_smoke        # by name fragment
#   bash tests/run_stata.sh x01_bridge_xproc # two concurrent Stata processes
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
selected=0
for suite in integration verify_suite roundtrip; do
    for f in "$REPO/tests/$suite"/*.do; do
        [ -e "$f" ] || continue
        base="$(basename "$f" .do)"
        case "$base" in
            _*) continue ;; # helper do-files, not standalone tests
        esac
        if [ -n "$FILTER" ] && [[ "$base" != *"$FILTER"* ]]; then continue; fi
        selected=$((selected + 1))
        echo "running $suite/$base ..."
        # Batch Stata names its log after the last token on the command line
        # and exits 0 regardless of errors: run through a same-named wrapper
        # so the log is <test>.log, and judge by VERDICT lines only.
        printf 'do "%s" "%s" "%s"\n' "$f" "$REPO" "$PLUGIN" > "$RUNDIR/$base.do"
        # Test do-files legitimately use tempfile and some create directories
        # beside it. Give every invocation a runner-owned temp root, then remove
        # it even if batch Stata returns 0 after an internal abort. This prevents
        # PID reuse or concurrent suites from inheriting another run's fixture.
        test_tmp="$RUNDIR/.tmp_$base"
        if ! mkdir "$test_tmp"; then
            echo "error: could not create private temp root for $suite/$base" >&2
            exit 2
        fi
        TMPDIR="$test_tmp" "$STATA" -b do "$RUNDIR/$base.do"
        rm -rf -- "$test_tmp"
        logs+=("$RUNDIR/$base.log")
    done
done

# Product-level bridge names must also survive two Stata processes sharing one
# TMPDIR.  This cannot be expressed as one .do-file: the purpose-built wrapper
# coordinates two licensed batch sessions, inspects both logs and cleans its
# runner-owned scratch.  GitHub-hosted CI never calls this Stata runner, so the
# gate does not introduce a license requirement there.
xproc_base="x01_bridge_xproc"
if [ -z "$FILTER" ] || [[ "$xproc_base" == *"$FILTER"* ]]; then
    selected=$((selected + 1))
    echo "running concurrent/$xproc_base ..."
    xproc_log="$RUNDIR/$xproc_base.log"
    TMPDIR="$RUNDIR" STATA="$STATA" BUILD_DIR="$BUILD_DIR" \
        bash "$REPO/tests/concurrent/$xproc_base.sh" >"$xproc_log" 2>&1 || true
    logs+=("$xproc_log")
fi

# Output publication has a separate cross-process ownership contract: exactly
# one writer may own a destination, and the loser must not touch the winner's
# staging or payload. The purpose-built regression deterministically holds the
# real production lock while the second licensed Stata process contends.
xproc_base="x02_output_xproc"
if [ -z "$FILTER" ] || [[ "$xproc_base" == *"$FILTER"* ]]; then
    selected=$((selected + 1))
    echo "running concurrent/$xproc_base ..."
    xproc_log="$RUNDIR/$xproc_base.log"
    TMPDIR="$RUNDIR" STATA="$STATA" BUILD_DIR="$BUILD_DIR" \
        bash "$REPO/tests/concurrent/$xproc_base.sh" >"$xproc_log" 2>&1 || true
    logs+=("$xproc_log")
fi

if [ "$selected" -eq 0 ]; then
    echo "error: no Stata tests matched filter '$FILTER'" >&2
    echo "logs in $RUNDIR"
    exit 2
fi

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
            # Nor may an early/stale PASS mask an uncaptured Stata abort later
            # in the log. Expected captured errors can print r(#), but their
            # test continues and its final verdict comes afterwards.
            last_verdict_line="$(grep -n "^VERDICT" "$log" | tail -n 1 | cut -d: -f1)"
            last_abort_line="$(grep -nE '^[[:space:]]*r\([0-9]+\);[[:space:]]*$' "$log" | tail -n 1 | cut -d: -f1 || true)"
            if [ -n "$last_abort_line" ] && [ "$last_abort_line" -gt "$last_verdict_line" ]; then
                echo "VERDICT($name): *** SCRIPT ABORTED AFTER VERDICT - inspect $log ***" | tee -a VERDICTS_SUMMARY.txt
                fail=1
            elif grep -qE "^VERDICT\(.*\): *FAIL" "$log"; then
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
