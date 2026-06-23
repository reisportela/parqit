* Benchmark full reads of a large Parquet dataset through pq, parqit collect,
* and native Stata use on a .dta generated from the same source.
*
* Default run:
*   stata-mp -b do benchmarks/benchmark_big_read_pq_parqit_use.do
*
* Optional args:
*   repo plugin parquet dta outdir reps
clear all
set more off
set varabbrev off

args repo plugin parquet dta outdir reps

* Paths come from args, else env vars, else portable defaults (no private paths):
*   PARQIT_REPO       repo root        (default: current directory)
*   PARQIT_BENCH_REF  reference Parquet (default: ./parqit_benchmark_ref.parquet)
if `"`repo'"' == "" {
    local repo : env PARQIT_REPO
    if (`"`repo'"' == "") local repo `"`c(pwd)'"'
}
if `"`plugin'"' == "" {
    local plugin `"`repo'/build/dev/parqit.plugin"'
}
if `"`parquet'"' == "" {
    local parquet : env PARQIT_BENCH_REF
    if (`"`parquet'"' == "") local parquet "parqit_benchmark_ref.parquet"
}
if `"`dta'"' == "" {
    local dta = subinstr(`"`parquet'"', ".parquet", ".dta", .)
}
if `"`outdir'"' == "" {
    local outdir `"`repo'/benchmarks/_out/big_read_ready_pq_parqit_use"'
}
if `"`reps'"' == "" {
    local reps 10
}
local reps = real("`reps'")
if missing(`reps') | `reps' <= 0 {
    di as err "reps must be a positive integer"
    exit 198
}
local reps = floor(`reps')

capture mkdir `"`repo'/benchmarks"'
capture mkdir `"`repo'/benchmarks/_out"'
capture mkdir `"`outdir'"'

log using `"`outdir'/benchmark_big_read_pq_parqit_use.log"', text replace

di as txt "benchmark: pq vs parqit collect vs native use"
di as txt "repo:    " as res `"`repo'"'
di as txt "plugin:  " as res `"`plugin'"'
di as txt "parquet: " as res `"`parquet'"'
di as txt "dta:     " as res `"`dta'"'
di as txt "outdir:  " as res `"`outdir'"'
di as txt "reps:    " as res "`reps'"
di as txt "started: " as res "`c(current_date)' `c(current_time)'"

confirm file `"`parquet'"'
confirm file `"`plugin'"'

adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

which pq
which parqit
parqit version

parqit describe `"`parquet'"'
local parquet_N = r(n_rows)
local parquet_k = r(n_cols)
di as txt "parquet rows/cols: " as res %18.0fc `parquet_N' as txt " / " as res `parquet_k'

local dta_source "provided_or_existing"
capture describe using `"`dta'"'
local dta_rc = _rc
if (`dta_rc' == 0) {
    local dta_N = r(N)
    local dta_k = r(k)
}
else {
    local dta_N = .
    local dta_k = .
}

if (`dta_rc' | `dta_N' != `parquet_N' | `dta_k' != `parquet_k') {
    local dta `"`outdir'/main_95_21_ready_from_parquet_for_use_benchmark.dta"'
    local dta_source "generated_from_parquet"
    capture describe using `"`dta'"'
    local scratch_rc = _rc
    if (`scratch_rc' == 0) {
        local scratch_N = r(N)
        local scratch_k = r(k)
    }
    else {
        local scratch_N = .
        local scratch_k = .
    }

    if (`scratch_rc' | `scratch_N' != `parquet_N' | `scratch_k' != `parquet_k') {
        di as txt "preparing .dta for native use benchmark from parquet..."
        clear
        timer clear 90
        timer on 90
        pq use using `"`parquet'"', clear
        timer off 90
        quietly timer list 90
        local prep_seconds = r(t90)
        assert _N == `parquet_N'
        assert c(k) == `parquet_k'
        save `"`dta'"', replace
        di as txt ".dta preparation seconds (not included in benchmark): " ///
            as res %12.3f `prep_seconds'
    }
}

describe using `"`dta'"'
local dta_N = r(N)
local dta_k = r(k)
if (`dta_N' != `parquet_N' | `dta_k' != `parquet_k') {
    di as err "native use .dta dimensions do not match parquet dimensions"
    di as err "parquet: N=`parquet_N' k=`parquet_k'; dta: N=`dta_N' k=`dta_k'"
    exit 459
}
di as txt "native use .dta source: " as res "`dta_source'"

tempname rawpost
tempfile raw
postfile `rawpost' str12 method int iteration int sequence double seconds ///
    double N int k int rc str40 dta_source using `"`raw'"', replace

local sequence = 0
forvalues i = 1/`reps' {
    local phase = mod(`i' - 1, 3)
    if (`phase' == 0) local order "pq parqit stata_use"
    if (`phase' == 1) local order "parqit stata_use pq"
    if (`phase' == 2) local order "stata_use pq parqit"

    di as txt "iteration `i' order: `order'"
    foreach method of local order {
        local ++sequence
        clear
        capture parqit close _all

        timer clear 1
        timer on 1

        if "`method'" == "pq" {
            capture noisily pq use using `"`parquet'"', clear
            local rc = _rc
        }
        else if "`method'" == "parqit" {
            capture noisily parqit use using `"`parquet'"'
            local rc = _rc
            if (`rc' == 0) {
                capture noisily parqit collect, clear
                local rc = _rc
            }
        }
        else if "`method'" == "stata_use" {
            capture noisily use `"`dta'"', clear
            local rc = _rc
        }
        else {
            local rc = 198
        }

        timer off 1
        quietly timer list 1
        local seconds = r(t1)

        if (`rc' == 0) {
            local obs = _N
            local vars = c(k)
        }
        else {
            local obs = .
            local vars = .
        }

        di as txt "RESULT method=`method' iteration=`i' sequence=`sequence' rc=`rc' seconds=" ///
            as res %12.3f `seconds' as txt " N=" as res %18.0fc `obs' as txt " k=" as res `vars'

        post `rawpost' ("`method'") (`i') (`sequence') (`seconds') ///
            (`obs') (`vars') (`rc') ("`dta_source'")

        capture parqit close _all
        clear
    }
}
postclose `rawpost'

use `"`raw'"', clear
gen byte ok = (rc == 0)
gen byte failed = (rc != 0)
label variable seconds "Wall-clock seconds from Stata timer"
label variable sequence "Overall run sequence; method order rotates by iteration"
label variable dta_source "Source of .dta used by native Stata use"
order method iteration sequence seconds N k rc ok failed dta_source

save `"`outdir'/big_read_raw.dta"', replace
export delimited using `"`outdir'/big_read_raw.csv"', replace

preserve
collapse (count) runs=seconds (sum) failures=failed ///
    (mean) mean_seconds=seconds (sd) sd_seconds=seconds ///
    (min) min_seconds=seconds (p50) p50_seconds=seconds ///
    (max) max_seconds=seconds, by(method)
gen double parquet_rows = `parquet_N'
gen int parquet_cols = `parquet_k'
gen str244 parquet_path = `"`parquet'"'
gen str244 dta_path = `"`dta'"'
gen str40 dta_source = "`dta_source'"
sort method
order method runs failures mean_seconds sd_seconds min_seconds p50_seconds ///
    max_seconds parquet_rows parquet_cols dta_source parquet_path dta_path
save `"`outdir'/big_read_summary.dta"', replace
export delimited using `"`outdir'/big_read_summary.csv"', replace

di as txt "summary:"
list method runs failures mean_seconds sd_seconds min_seconds p50_seconds ///
    max_seconds, noobs abbreviate(24)
restore

di as txt "raw results:     " as res `"`outdir'/big_read_raw.csv"'
di as txt "summary results: " as res `"`outdir'/big_read_summary.csv"'
di as txt "finished:        " as res "`c(current_date)' `c(current_time)'"
log close
