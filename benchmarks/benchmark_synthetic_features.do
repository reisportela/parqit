* Benchmark parqit feature workflows on deterministic synthetic Parquet data.
*
* Prerequisite:
*   python3 benchmarks/make_synthetic_data.py
*
* Default run:
*   stata-mp -b do benchmarks/benchmark_synthetic_features.do
*
* Optional args:
*   repo plugin datadir outdir reps
clear all
set more off
set varabbrev off

args repo plugin datadir outdir reps

if `"`repo'"' == "" {
    local repo : env PARQIT_REPO
    if (`"`repo'"' == "") local repo `"`c(pwd)'"'
}
if `"`plugin'"' == "" {
    local plugin `"`repo'/build/dev/parqit.plugin"'
}
if `"`datadir'"' == "" {
    local datadir `"`repo'/benchmarks/_out/synthetic_medium_data"'
}
if `"`outdir'"' == "" {
    local outdir `"`repo'/benchmarks/_out/synthetic_feature_benchmark"'
}
if `"`reps'"' == "" {
    local reps 5
}
local reps = real("`reps'")
if missing(`reps') | `reps' <= 0 {
    di as err "reps must be a positive integer"
    exit 198
}
local reps = floor(`reps')

local workers `"`datadir'/workers_perf.parquet"'
local firms   `"`datadir'/firms_perf.parquet"'
local patents `"`datadir'/patents_perf.parquet"'
local wide    `"`datadir'/wide_income_perf.parquet"'
local manifest `"`datadir'/manifest.json"'

confirm file `"`plugin'"'
confirm file `"`workers'"'
confirm file `"`firms'"'
confirm file `"`patents'"'
confirm file `"`wide'"'

capture mkdir `"`repo'/benchmarks"'
capture mkdir `"`repo'/benchmarks/_out"'
capture mkdir `"`outdir'"'
capture mkdir `"`outdir'/scratch"'

log using `"`outdir'/benchmark_synthetic_features.log"', text replace

di as txt "benchmark: synthetic parqit feature workflows"
di as txt "repo:    " as res `"`repo'"'
di as txt "plugin:  " as res `"`plugin'"'
di as txt "datadir: " as res `"`datadir'"'
di as txt "outdir:  " as res `"`outdir'"'
di as txt "reps:    " as res "`reps'"
di as txt "started: " as res "`c(current_date)' `c(current_time)'"
di as txt "Stata:   " as res "`c(stata_version)' `c(os)' `c(machine_type)'"

capture confirm file `"`manifest'"'
if (_rc == 0) {
    copy `"`manifest'"' `"`outdir'/data_manifest.json"', replace
}

adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

which parqit
parqit version
capture noisily shell sha256sum "`plugin'"
capture noisily shell uname -a
capture noisily shell uptime

parqit describe `"`workers'"'
local worker_N = r(n_rows)
local worker_k = r(n_cols)
parqit describe `"`firms'"'
local firm_N = r(n_rows)
parqit describe `"`patents'"'
local patent_N = r(n_rows)
parqit describe `"`wide'"'
local wide_N = r(n_rows)
local wide_k = r(n_cols)

tempname rawpost
tempfile raw
postfile `rawpost' str24 workflow int iteration int sequence double seconds ///
    int rc double rows int cols str244 artifact using `"`raw'"', replace

local workflows "collect_full filter_gen_save collapse_save sort_save merge_save joinby_save reshape_long_save"
local nwork : word count `workflows'
local sequence = 0

forvalues i = 1/`reps' {
    local shift = mod(`i' - 1, `nwork')
    local order ""
    forvalues j = 1/`nwork' {
        local idx = mod(`j' - 1 + `shift', `nwork') + 1
        local w : word `idx' of `workflows'
        local order "`order' `w'"
    }

    di as txt "iteration `i' order:`order'"
    foreach workflow of local order {
        local ++sequence
        local artifact ""
        local rows = .
        local cols = .

        clear
        capture parqit close _all

        timer clear 1
        timer on 1

        if "`workflow'" == "collect_full" {
            capture noisily parqit use using `"`workers'"'
            local rc = _rc
            if (`rc' == 0) {
                capture noisily parqit collect, clear
                local rc = _rc
            }
            if (`rc' == 0) {
                local rows = _N
                local cols = c(k)
            }
        }
        else if "`workflow'" == "filter_gen_save" {
            local artifact `"`outdir'/scratch/filter_gen_save_`i'.parquet"'
            capture noisily parqit use using `"`workers'"'
            local rc = _rc
            if (`rc' == 0) {
                capture noisily parqit keep if year >= 2020 & wage > 0
                local rc = _rc
            }
            if (`rc' == 0) {
                capture noisily parqit gen double lwage = log(wage)
                local rc = _rc
            }
            if (`rc' == 0) {
                capture noisily parqit save `"`artifact'"', replace
                local rc = _rc
            }
            if (`rc' == 0) {
                quietly parqit describe `"`artifact'"'
                local rows = r(n_rows)
                local cols = r(n_cols)
            }
        }
        else if "`workflow'" == "collapse_save" {
            local artifact `"`outdir'/scratch/collapse_save_`i'.parquet"'
            capture noisily parqit use using `"`workers'"'
            local rc = _rc
            if (`rc' == 0) {
                capture noisily parqit collapse (mean) wage (sd) sd_wage = wage (count) n = wage, by(firm_id year)
                local rc = _rc
            }
            if (`rc' == 0) {
                capture noisily parqit save `"`artifact'"', replace
                local rc = _rc
            }
            if (`rc' == 0) {
                quietly parqit describe `"`artifact'"'
                local rows = r(n_rows)
                local cols = r(n_cols)
            }
        }
        else if "`workflow'" == "merge_save" {
            local artifact `"`outdir'/scratch/merge_save_`i'.parquet"'
            capture noisily parqit use using `"`workers'"'
            local rc = _rc
            if (`rc' == 0) {
                capture noisily parqit keep if year == 2022
                local rc = _rc
            }
            if (`rc' == 0) {
                capture noisily parqit merge m:1 firm_id using `"`firms'"', keep(match) keepusing(tfp capital industry) nogenerate
                local rc = _rc
            }
            if (`rc' == 0) {
                capture noisily parqit save `"`artifact'"', replace
                local rc = _rc
            }
            if (`rc' == 0) {
                quietly parqit describe `"`artifact'"'
                local rows = r(n_rows)
                local cols = r(n_cols)
            }
        }
        else if "`workflow'" == "sort_save" {
            local artifact `"`outdir'/scratch/sort_save_`i'.parquet"'
            capture noisily parqit use using `"`workers'"'
            local rc = _rc
            if (`rc' == 0) {
                capture noisily parqit sort firm_id year wage
                local rc = _rc
            }
            if (`rc' == 0) {
                capture noisily parqit save `"`artifact'"', replace
                local rc = _rc
            }
            if (`rc' == 0) {
                quietly parqit describe `"`artifact'"'
                local rows = r(n_rows)
                local cols = r(n_cols)
            }
        }
        else if "`workflow'" == "joinby_save" {
            local artifact `"`outdir'/scratch/joinby_save_`i'.parquet"'
            capture noisily parqit use using `"`workers'"'
            local rc = _rc
            if (`rc' == 0) {
                capture noisily parqit keep if year == 2022
                local rc = _rc
            }
            if (`rc' == 0) {
                capture noisily parqit joinby firm_id using `"`patents'"'
                local rc = _rc
            }
            if (`rc' == 0) {
                capture noisily parqit save `"`artifact'"', replace
                local rc = _rc
            }
            if (`rc' == 0) {
                quietly parqit describe `"`artifact'"'
                local rows = r(n_rows)
                local cols = r(n_cols)
            }
        }
        else if "`workflow'" == "reshape_long_save" {
            local artifact `"`outdir'/scratch/reshape_long_save_`i'.parquet"'
            capture noisily parqit use using `"`wide'"'
            local rc = _rc
            if (`rc' == 0) {
                capture noisily parqit reshape long inc, i(pid grp) j(year)
                local rc = _rc
            }
            if (`rc' == 0) {
                capture noisily parqit save `"`artifact'"', replace
                local rc = _rc
            }
            if (`rc' == 0) {
                quietly parqit describe `"`artifact'"'
                local rows = r(n_rows)
                local cols = r(n_cols)
            }
        }
        else {
            local rc = 198
        }

        timer off 1
        quietly timer list 1
        local seconds = r(t1)

        di as txt "RESULT workflow=`workflow' iteration=`i' sequence=`sequence' rc=`rc' seconds=" ///
            as res %12.3f `seconds' as txt " rows=" as res %18.0fc `rows' as txt " cols=" as res `cols'

        post `rawpost' ("`workflow'") (`i') (`sequence') (`seconds') ///
            (`rc') (`rows') (`cols') (`"`artifact'"')

        capture parqit close _all
        clear
    }
}
postclose `rawpost'

use `"`raw'"', clear
gen byte ok = (rc == 0)
gen byte failed = (rc != 0)
gen double worker_rows = `worker_N'
gen int worker_cols = `worker_k'
gen double firm_rows = `firm_N'
gen double patent_rows = `patent_N'
gen double wide_rows = `wide_N'
gen int wide_cols = `wide_k'
label variable seconds "Wall-clock seconds from Stata timer"
label variable sequence "Overall run sequence; workflow order rotates by iteration"
order workflow iteration sequence seconds rc ok failed rows cols artifact

save `"`outdir'/synthetic_features_raw.dta"', replace
export delimited using `"`outdir'/synthetic_features_raw.csv"', replace

preserve
collapse (count) runs=seconds (sum) failures=failed ///
    (mean) mean_seconds=seconds (sd) sd_seconds=seconds ///
    (min) min_seconds=seconds (p50) p50_seconds=seconds ///
    (max) max_seconds=seconds, by(workflow)
sort workflow
order workflow runs failures mean_seconds sd_seconds min_seconds p50_seconds max_seconds
save `"`outdir'/synthetic_features_summary.dta"', replace
export delimited using `"`outdir'/synthetic_features_summary.csv"', replace

di as txt "summary:"
list workflow runs failures mean_seconds sd_seconds min_seconds p50_seconds ///
    max_seconds, noobs abbreviate(24)
restore

di as txt "raw results:     " as res `"`outdir'/synthetic_features_raw.csv"'
di as txt "summary results: " as res `"`outdir'/synthetic_features_summary.csv"'
di as txt "finished:        " as res "`c(current_date)' `c(current_time)'"
log close
