#!/usr/bin/env bash
# Deterministic two-process regression for BRIDGE-XPROC-1.  Both Stata
# processes share one TMPDIR (whose path contains spaces and Unicode), while
# marker files force B to create each bridge before A materialises its view.
set -u

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
STATA="${STATA:-stata-mp}"
BUILD_DIR="${BUILD_DIR:-$REPO/build/dev}"
PLUGIN="$BUILD_DIR/parqit.plugin"
ROOT="$(mktemp -d "${TMPDIR:-/tmp}/parqit_bridge_xproc.XXXXXX")"
SHARED_TMP="$ROOT/shared tmp ü"
A_DIR="$ROOT/session_a"
B_DIR="$ROOT/session_b"
PID_A=""
PID_B=""

cleanup() {
    [ -z "$PID_A" ] || kill "$PID_A" 2>/dev/null || true
    [ -z "$PID_B" ] || kill "$PID_B" 2>/dev/null || true
    rm -rf -- "$ROOT"
}
trap cleanup EXIT HUP INT TERM

fail() {
    printf 'VERDICT(X01_BRIDGE_XPROC): FAIL - %s\n' "$1"
    for log in "$A_DIR/session_a.log" "$B_DIR/session_b.log"; do
        if [ -f "$log" ]; then
            printf '\n===== %s =====\n' "$(basename "$log")"
            sed -n '1,240p' "$log"
        else
            printf '\n===== missing %s =====\n' "$(basename "$log")"
        fi
    done
    exit 1
}

if [ ! -x "$STATA" ] && ! command -v "$STATA" >/dev/null 2>&1; then
    fail "Stata not found at $STATA"
fi
[ -f "$PLUGIN" ] || fail "plugin not found at $PLUGIN"
mkdir -p "$SHARED_TMP" "$A_DIR" "$B_DIR"

printf 'do "%s" "%s" "%s" "%s"\n' \
    "$REPO/tests/concurrent/x01_bridge_session_a.do" "$REPO" "$PLUGIN" "$ROOT" \
    >"$A_DIR/session_a.do"
printf 'do "%s" "%s" "%s" "%s"\n' \
    "$REPO/tests/concurrent/x01_bridge_session_b.do" "$REPO" "$PLUGIN" "$ROOT" \
    >"$B_DIR/session_b.do"

(
    cd "$A_DIR" || exit 1
    TMPDIR="$SHARED_TMP" "$STATA" -b do session_a.do
) >"$A_DIR/stdout.txt" 2>&1 &
PID_A=$!
(
    cd "$B_DIR" || exit 1
    TMPDIR="$SHARED_TMP" "$STATA" -b do session_b.do
) >"$B_DIR/stdout.txt" 2>&1 &
PID_B=$!

# Stata batch often exits 0 after a do-file abort, so the timeout only proves
# termination; the logs and final verdicts below decide correctness.
timed_out=1
for _ in $(seq 1 600); do
    if ! kill -0 "$PID_A" 2>/dev/null && ! kill -0 "$PID_B" 2>/dev/null; then
        timed_out=0
        break
    fi
    sleep 0.05
done
[ "$timed_out" -eq 0 ] || fail "timeout waiting for the two Stata sessions"
wait "$PID_A" 2>/dev/null || true
wait "$PID_B" 2>/dev/null || true
PID_A=""
PID_B=""

inspect_log() {
    local log="$1" verdict="$2" last_verdict last_abort
    [ -f "$log" ] || return 1
    grep -q "^VERDICT($verdict): PASS" "$log" || return 1
    ! grep -qE '^VERDICT\(.*\): *FAIL' "$log" || return 1
    last_verdict="$(grep -n '^VERDICT' "$log" | tail -n 1 | cut -d: -f1)"
    last_abort="$(grep -nE '^[[:space:]]*r\([0-9]+\);[[:space:]]*$' "$log" | tail -n 1 | cut -d: -f1 || true)"
    [ -z "$last_abort" ] || [ "$last_abort" -lt "$last_verdict" ]
}

inspect_log "$A_DIR/session_a.log" X01_BRIDGE_SESSION_A || fail "session A log/verdict failed"
inspect_log "$B_DIR/session_b.log" X01_BRIDGE_SESSION_B || fail "session B log/verdict failed"

for marker in a_open b_open a_adapter b_adapter a_done b_done; do
    [ -s "$ROOT/$marker.marker" ] || fail "missing marker $marker"
done

a_open="$(sed -n '1p' "$ROOT/a_open.marker")"
b_open="$(sed -n '1p' "$ROOT/b_open.marker")"
a_adapter="$(sed -n '1p' "$ROOT/a_adapter.marker")"
b_adapter="$(sed -n '1p' "$ROOT/b_adapter.marker")"
[ -n "$a_open" ] && [ -n "$b_open" ] && [ "$a_open" != "$b_open" ] || \
    fail "open _data bridge paths are not distinct"
[ -n "$a_adapter" ] && [ -n "$b_adapter" ] && [ "$a_adapter" != "$b_adapter" ] || \
    fail ".dta adapter bridge paths are not distinct"

grep '^VERDICT' "$A_DIR/session_a.log"
grep '^VERDICT' "$B_DIR/session_b.log"
printf 'BRIDGE_PATH(open,A)=%s\n' "$a_open"
printf 'BRIDGE_PATH(open,B)=%s\n' "$b_open"
printf 'BRIDGE_PATH(adapter,A)=%s\n' "$a_adapter"
printf 'BRIDGE_PATH(adapter,B)=%s\n' "$b_adapter"
printf 'VERDICT(X01_BRIDGE_XPROC): PASS\n'
