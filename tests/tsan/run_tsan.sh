#!/bin/bash
# B9 (audit of 25 Oct 2026): ThreadSanitizer scenarios of the reader workers on the committed fixtures, with the
# fault and delay hooks of A4/A5. Usage: run_tsan.sh <parqit_tsan_readers> <repo>. Fails on any TSan report, any
# non-zero exit, a scenario over 300 seconds, or an outcome other than the expected one in any of the binary's
# three repetitions; a complete read's checksum must be the same in all three and equal to the format's first
# complete read (G10, audit of the execution). Not covered here: the save's SPI pass and a COPY the real engine
# cancels.
set -u
bin=$1
repo=$2
survey=$repo/tests/fixtures/spss/survey.sav
frame=$repo/tests/fixtures/r/frame.rds
work=$(mktemp -d "${TMPDIR:-/tmp}/parqit_tsan.XXXXXX")
export TMPDIR=$work
export TSAN_OPTIONS="halt_on_error=0 report_signal_unsafe=0 second_deadlock_stack=1"
fails=0
declare -A reference=()
run() { # name, expected text in each repetition (two lines: drained, finish), env..., args...
    local name=$1 want=$2
    shift 2
    timeout 300 env "$@" > "$work/$name.out" 2> "$work/$name.tsan"
    local rc=$? warnings lines reps sums mode
    warnings=$(grep -c 'WARNING: ThreadSanitizer' "$work/$name.tsan")
    lines=$(wc -l < "$work/$name.out")
    reps=$(awk -v w="$want" 'index($0, w) { hit[int((NR - 1) / 2)] = 1 } END { n = 0; for (k in hit) n++; print n }' "$work/$name.out")
    local bad=""
    [ "$rc" -ne 0 ] && bad="$bad exit $rc"
    [ "$warnings" -ne 0 ] && bad="$bad $warnings TSan reports"
    [ "$lines" -ne 6 ] || [ "$reps" -ne 3 ] && bad="$bad $reps of 3 repetitions as expected"
    case "$want" in
    *checksum*)
        sums=$(grep -o 'checksum=[0-9]*' "$work/$name.out" | sort -u)
        mode=${name%%_*}
        [ "$(printf '%s\n' "$sums" | wc -l)" -ne 1 ] && bad="$bad checksums differ between repetitions"
        if [ -z "${reference[$mode]:-}" ]; then reference[$mode]=$sums
        elif [ "$sums" != "${reference[$mode]}" ]; then bad="$bad checksum $sums, not ${reference[$mode]}"; fi
        ;;
    esac
    if [ -n "$bad" ]; then
        echo "FAIL $name:$bad"; fails=$((fails + 1))
    else
        echo "ok   $name"
    fi
}
run spss_default "rows=60 checksum" "$bin" spss "$survey" 8 7 4 0 0 0
run spss_one_block_at_a_time "rows=60 checksum" "$bin" spss "$survey" 8 7 4 1 1 0
run spss_block0_held_back "rows=60 checksum" PARQIT_TEST_DELAY=spss_convert:0:200 "$bin" spss "$survey" 8 7 4 64 64 0
run spss_abort "rows=14 " PARQIT_TEST_DELAY=spss_convert:2:100 "$bin" spss "$survey" 8 7 4 0 0 2
run spss_fault_batch "err=std::bad_alloc" PARQIT_TEST_FAULT=spss_batch:3 "$bin" spss "$survey" 8 7 4 0 0 0
run spss_fault_ready "err=std::bad_alloc" PARQIT_TEST_FAULT=spss_ready:3 "$bin" spss "$survey" 8 7 4 0 0 0
run spss_fault_convert "err=std::bad_alloc" PARQIT_TEST_FAULT=spss_convert:3 "$bin" spss "$survey" 8 7 4 64 64 0
run spss_fault_driver "could not start the SPSS conversion" PARQIT_TEST_FAULT=spss_driver:0 "$bin" spss "$survey" 8 7 4 0 0 0
run spss_fault_thread "rows=60 checksum" PARQIT_TEST_FAULT=spss_thread:2 "$bin" spss "$survey" 8 7 4 0 0 0
# B2: the .zsav fixture's cases in ZLIB blocks of 97 bytes, inflated ahead on threads
"$bin" zsav-split "$repo/tests/fixtures/spss/survey.zsav" "$work/many.zsav" 97 > "$work/split.out" || fails=$((fails + 1))
run zsav_default "rows=60 checksum" "$bin" spss "$work/many.zsav" 8 7 4 0 0 0
run zsav_one_block_at_a_time "rows=60 checksum" "$bin" spss "$work/many.zsav" 8 7 4 1 1 0
run zsav_abort "rows=14 " PARQIT_TEST_DELAY=spss_convert:2:100 "$bin" spss "$work/many.zsav" 8 7 4 0 0 2
# B1: bytecode walked by the producer and expanded by the workers
run bytecode_default "rows=60 checksum" "$bin" spss "$repo/tests/fixtures/spss/survey_bc.sav" 8 7 4 0 0 0
run bytecode_one_block_at_a_time "rows=60 checksum" "$bin" spss "$repo/tests/fixtures/spss/survey_bc.sav" 8 7 4 1 1 0
run bytecode_fault_convert "err=std::bad_alloc" PARQIT_TEST_FAULT=spss_convert:3 "$bin" spss "$repo/tests/fixtures/spss/survey_bc.sav" 8 7 4 64 64 0
# Codex audit, finding 1: a worker fails expanding its block while the producer waits for read-ahead room
# (the fault in the conversion only: make_plan's profile expands the same blocks)
run bytecode_fault_expand "err=std::bad_alloc" PARQIT_TSAN_CONVERT_FAULT=spss_expand:0 "$bin" spss "$repo/tests/fixtures/spss/survey_bc.sav" 8 7 4 1 1 0
run zsav_fault_expand "err=std::bad_alloc" PARQIT_TSAN_CONVERT_FAULT=spss_expand:3 "$bin" spss "$work/many.zsav" 8 7 4 1 1 0
run rdata_batches_of_one "rows=5 checksum" "$bin" rdata "$frame" 4 1 4 0
run rdata_abort "rows=2 " "$bin" rdata "$frame" 4 1 4 2
run rdata_fault_batch "conversion failed" PARQIT_TEST_FAULT=rdata_batch:1 "$bin" rdata "$frame" 4 1 4 0
run rdata_fault_ready "conversion failed" PARQIT_TEST_FAULT=rdata_ready:1 "$bin" rdata "$frame" 4 1 4 0
run rdata_fault_convert "err=std::bad_alloc" PARQIT_TEST_FAULT=rdata_convert:1 "$bin" rdata "$frame" 4 1 4 0
run rdata_fault_thread "could not start the R conversion workers" PARQIT_TEST_FAULT=rdata_thread:2 "$bin" rdata "$frame" 4 1 4 0
rm -rf -- "$work"
[ "$fails" -eq 0 ] && echo "TSAN_READERS_PASS" || echo "TSAN_READERS_FAIL $fails"
exit "$fails"
