* Probe: does the view-collect path pay a redundant sizing scan that the
* direct `parqit use ..., clear` path (F2 metadata sizing) avoids?
* Compares, same-session, min-of-N:
*   A) parqit use FILE, clear              (direct read path; gets F2)
*   B) parqit use FILE  +  parqit collect    (view materialise; F5 gap — no F2)
* on an all-numeric file (F2 should help B a lot) and a string-heavy file
* (F2 should help B little).  Pure measurement: no code changes.
clear all
set more off
set varabbrev off

local repo : env PARQIT_REPO
if (`"`repo'"' == "") local repo `"`c(pwd)'"'
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`repo'/build/dev/parqit.plugin"'

local ref : env PARQIT_BENCH_REF
if (`"`ref'"' == "") local ref "parqit_benchmark_ref.parquet"
local wrk  "`repo'/benchmarks/_out/synthetic_medium_data/workers_perf.parquet"
local reps 6

which parqit
parqit version

foreach pair in ref wrk {
    if "`pair'" == "ref" local f "`ref'"
    else                 local f "`wrk'"

    di as txt _n "================ FILE: `pair' = `f' ================"
    * warm the OS cache once
    capture parqit close _all
    quietly parqit use using `"`f'"', clear

    * ---- A) direct use, clear (F2 fast path) ----
    local bestA = .
    forvalues i = 1/`reps' {
        clear
        capture parqit close _all
        timer clear 1
        timer on 1
        quietly parqit use using `"`f'"', clear
        timer off 1
        quietly timer list 1
        local t = r(t1)
        if (`t' < `bestA') local bestA = `t'
        di as txt "  A use,clear  rep `i' = " as res %6.3f `t'
    }

    * ---- B) build view, then collect, clear (no F2) ----
    local bestB = .
    forvalues i = 1/`reps' {
        clear
        capture parqit close _all
        timer clear 2
        timer on 2
        quietly parqit use using `"`f'"'
        quietly parqit collect, clear
        timer off 2
        quietly timer list 2
        local t = r(t2)
        if (`t' < `bestB') local bestB = `t'
        di as txt "  B use+collect rep `i' = " as res %6.3f `t'
    }

    di as res _n "  RESULT[`pair']  A(use,clear)=" %6.3f `bestA' ///
        "   B(use+collect)=" %6.3f `bestB' ///
        "   B-A=" %6.3f (`bestB'-`bestA') " s (min of `reps')"
}

di as txt _n "PROBE DONE"
