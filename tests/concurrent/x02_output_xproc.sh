#!/usr/bin/env bash
# Two Stata processes contend for the same destination. Session A holds the
# real production lock through a bounded test hook; B must fail loudly without
# touching A's staging, and A's complete payload must be the only publication.
set -u

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
STATA="${STATA:-stata-mp}"
BUILD_DIR="${BUILD_DIR:-$REPO/build/dev}"
PLUGIN="$BUILD_DIR/parqit.plugin"
ROOT="$(mktemp -d "${TMPDIR:-/tmp}/parqit_output_xproc.XXXXXX")"
A_DIR="$ROOT/session_a"
B_DIR="$ROOT/session_b"
DEST="$ROOT/shared_output.parquet"
PID_A=""
PID_B=""

cleanup() {
    [ -z "$PID_A" ] || kill "$PID_A" 2>/dev/null || true
    [ -z "$PID_B" ] || kill "$PID_B" 2>/dev/null || true
    rm -rf -- "$ROOT"
}
trap cleanup EXIT HUP INT TERM

fail() {
    printf 'VERDICT(X02_OUTPUT_XPROC): FAIL - %s\n' "$1"
    for log in "$A_DIR/session_a.log" "$B_DIR/session_b.log"; do
        if [ -f "$log" ]; then
            printf '\n===== %s =====\n' "$(basename "$log")"
            sed -n '1,240p' "$log"
        fi
    done
    exit 1
}

if [ ! -x "$STATA" ] && ! command -v "$STATA" >/dev/null 2>&1; then
    fail "Stata not found at $STATA"
fi
[ -f "$PLUGIN" ] || fail "plugin not found at $PLUGIN"
mkdir -p "$A_DIR" "$B_DIR"

printf 'do "%s" "%s" "%s" "%s"\n' \
    "$REPO/tests/concurrent/x02_output_session_a.do" "$REPO" "$PLUGIN" "$ROOT" \
    >"$A_DIR/session_a.do"
printf 'do "%s" "%s" "%s" "%s"\n' \
    "$REPO/tests/concurrent/x02_output_session_b.do" "$REPO" "$PLUGIN" "$ROOT" \
    >"$B_DIR/session_b.do"

(
    cd "$A_DIR" || exit 1
    PARQIT_TEST_HOLD_OUTPUT_LOCK_MS=3000 "$STATA" -b do session_a.do
) >"$A_DIR/stdout.txt" 2>&1 &
PID_A=$!

lock_seen=0
for _ in $(seq 1 400); do
    if [ -d "$DEST.parqit_lock" ]; then
        lock_seen=1
        break
    fi
    kill -0 "$PID_A" 2>/dev/null || break
    sleep 0.01
done
[ "$lock_seen" -eq 1 ] || fail "session A never exposed its owned output lock"

(
    cd "$B_DIR" || exit 1
    "$STATA" -b do session_b.do
) >"$B_DIR/stdout.txt" 2>&1 &
PID_B=$!

timed_out=1
for _ in $(seq 1 800); do
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

inspect_log "$A_DIR/session_a.log" X02_OUTPUT_SESSION_A || fail "session A failed"
inspect_log "$B_DIR/session_b.log" X02_OUTPUT_SESSION_B || fail "session B failed"
[ -f "$DEST" ] || fail "winning output was not published"
[ ! -e "$DEST.parqit_lock" ] || fail "owned lock was not cleaned after success"
compgen -G "$DEST.parqit_txn_*" >/dev/null && fail "transaction staging leaked"

grep '^VERDICT' "$A_DIR/session_a.log"
grep '^VERDICT' "$B_DIR/session_b.log"
printf 'VERDICT(X02_OUTPUT_XPROC): PASS\n'
