* parqit vs pq benchmark runner; Python/DuckDB validation is done outside Stata.
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
    local outdir `"`repo'/benchmarks/_out/parqit_pq_python_canonical"'
}
if `"`reps'"' == "" local reps 3
local reps = floor(real("`reps'"))
if missing(`reps') | `reps' <= 0 {
    di as err "reps must be a positive integer"
    exit 198
}

local workers `"`datadir'/workers_perf.parquet"'
local firms   `"`datadir'/firms_perf.parquet"'
local patents `"`datadir'/patents_perf.parquet"'

confirm file `"`plugin'"'
confirm file `"`workers'"'
confirm file `"`firms'"'
confirm file `"`patents'"'

capture mkdir `"`repo'/benchmarks"'
capture mkdir `"`repo'/benchmarks/_out"'
capture mkdir `"`outdir'"'
capture mkdir `"`outdir'/artifacts"'
capture mkdir `"`outdir'/artifacts/pq"'
capture mkdir `"`outdir'/artifacts/parqit"'

log using `"`outdir'/stata_parqit_pq.log"', text replace

di as txt "benchmark: parqit vs pq, Python canonical validation external"
di as txt "repo:    " as res `"`repo'"'
di as txt "plugin:  " as res `"`plugin'"'
di as txt "datadir: " as res `"`datadir'"'
di as txt "outdir:  " as res `"`outdir'"'
di as txt "reps:    " as res "`reps'"
di as txt "started: " as res "`c(current_date)' `c(current_time)'"
di as txt "Stata:   " as res "`c(stata_version)' `c(os)' `c(machine_type)'"

adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

which pq
which parqit
parqit version
capture noisily shell sha256sum "`plugin'"
capture noisily shell uname -a
capture noisily shell uptime

tempname rawpost
tempfile raw
postfile `rawpost' str32 workflow str8 method int iteration int sequence ///
    double seconds int rc double rows int cols str244 artifact using `"`raw'"', replace

local workflows "describe path use save merge append workflow_filter_gen workflow_collapse"
local methods "pq parqit"
local nwork : word count `workflows'
local nmethods : word count `methods'
local sequence = 0

forvalues i = 1/`reps' {
    local wshift = mod(`i' - 1, `nwork')
    local mshift = mod(`i' - 1, `nmethods')
    local worder ""
    forvalues j = 1/`nwork' {
        local idx = mod(`j' - 1 + `wshift', `nwork') + 1
        local w : word `idx' of `workflows'
        local worder "`worder' `w'"
    }
    local morder ""
    forvalues j = 1/`nmethods' {
        local idx = mod(`j' - 1 + `mshift', `nmethods') + 1
        local m : word `idx' of `methods'
        local morder "`morder' `m'"
    }
    di as txt "iteration `i' workflow order:`worder' method order:`morder'"

    foreach workflow of local worder {
        foreach method of local morder {
            local ++sequence
            local artifact `"`outdir'/artifacts/`method'/`workflow'_`i'.parquet"'
            local rows = .
            local cols = .

            clear
            capture parqit close _all
            timer clear 1
            timer on 1

            if "`method'" == "pq" {
                if "`workflow'" == "describe" {
                    local artifact `"`workers'"'
                    capture noisily pq describe using `"`workers'"', quietly
                    local rc = _rc
                    if (`rc' == 0) {
                        local rows = real("`r(n_rows)'")
                        local cols = real("`r(n_columns)'")
                    }
                }
                else if "`workflow'" == "path" {
                    capture noisily pq path `"`workers'"'
                    local rc = _rc
                    if (`rc' == 0) local artifact `"`r(fullpath)'"'
                }
                else if "`workflow'" == "use" {
                    capture noisily pq use using `"`workers'"', clear
                    local rc = _rc
                    if (`rc' == 0) {
                        local rows = _N
                        local cols = c(k)
                    }
                }
                else if "`workflow'" == "save" {
                    timer off 1
                    capture noisily parqit use using `"`workers'"', clear
                    local rc = _rc
                    timer clear 1
                    timer on 1
                    if (`rc' == 0) {
                        capture noisily pq save using `"`artifact'"', replace
                        local rc = _rc
                    }
                }
                else if "`workflow'" == "merge" {
                    capture noisily pq use using `"`workers'"', clear
                    local rc = _rc
                    if (`rc' == 0) {
                        capture noisily keep if year == 2022
                        local rc = _rc
                    }
                    if (`rc' == 0) {
                        capture noisily pq merge m:1 firm_id using `"`firms'"', keep(match) keepusing(tfp capital industry) nogenerate
                        local rc = _rc
                    }
                    if (`rc' == 0) {
                        capture noisily pq save using `"`artifact'"', replace
                        local rc = _rc
                    }
                }
                else if "`workflow'" == "append" {
                    capture noisily pq use using `"`patents'"', clear
                    local rc = _rc
                    if (`rc' == 0) {
                        capture noisily pq append using `"`patents'"'
                        local rc = _rc
                    }
                    if (`rc' == 0) {
                        capture noisily pq save using `"`artifact'"', replace
                        local rc = _rc
                    }
                }
                else if "`workflow'" == "workflow_filter_gen" {
                    capture noisily pq use using `"`workers'"', clear
                    local rc = _rc
                    if (`rc' == 0) {
                        capture noisily keep if !missing(wage) & year >= 2020 & wage > 0
                        local rc = _rc
                    }
                    if (`rc' == 0) {
                        capture noisily gen double lwage = log(wage)
                        local rc = _rc
                    }
                    if (`rc' == 0) {
                        capture noisily pq save using `"`artifact'"', replace
                        local rc = _rc
                    }
                }
                else if "`workflow'" == "workflow_collapse" {
                    capture noisily pq use using `"`workers'"', clear
                    local rc = _rc
                    if (`rc' == 0) {
                        capture noisily collapse (mean) wage (sd) sd_wage = wage (count) n = wage, by(firm_id year)
                        local rc = _rc
                    }
                    if (`rc' == 0) {
                        capture noisily pq save using `"`artifact'"', replace
                        local rc = _rc
                    }
                }
                else local rc = 198
            }
            else if "`method'" == "parqit" {
                if "`workflow'" == "describe" {
                    local artifact `"`workers'"'
                    capture noisily parqit describe `"`workers'"'
                    local rc = _rc
                    if (`rc' == 0) {
                        local rows = r(n_rows)
                        local cols = r(n_cols)
                    }
                }
                else if "`workflow'" == "path" {
                    capture noisily parqit path `"`workers'"'
                    local rc = _rc
                    if (`rc' == 0) local artifact `"`r(path)'"'
                }
                else if "`workflow'" == "use" {
                    capture noisily parqit use using `"`workers'"', clear
                    local rc = _rc
                    if (`rc' == 0) {
                        local rows = _N
                        local cols = c(k)
                    }
                }
                else if "`workflow'" == "save" {
                    timer off 1
                    capture noisily parqit use using `"`workers'"', clear
                    local rc = _rc
                    timer clear 1
                    timer on 1
                    if (`rc' == 0) {
                        capture noisily parqit save `"`artifact'"', replace data
                        local rc = _rc
                    }
                }
                else if "`workflow'" == "merge" {
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
                }
                else if "`workflow'" == "append" {
                    capture noisily parqit use using `"`patents'"'
                    local rc = _rc
                    if (`rc' == 0) {
                        capture noisily parqit append using `"`patents'"'
                        local rc = _rc
                    }
                    if (`rc' == 0) {
                        capture noisily parqit save `"`artifact'"', replace
                        local rc = _rc
                    }
                }
                else if "`workflow'" == "workflow_filter_gen" {
                    capture noisily parqit use using `"`workers'"'
                    local rc = _rc
                    if (`rc' == 0) {
                        capture noisily parqit keep if !missing(wage) & year >= 2020 & wage > 0
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
                }
                else if "`workflow'" == "workflow_collapse" {
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
                }
                else local rc = 198
            }
            else local rc = 198

            timer off 1
            quietly timer list 1
            local seconds = r(t1)

            if (`rc' == 0 & inlist("`workflow'", "use")) {
                timer off 1
                if "`method'" == "pq" {
                    capture noisily pq save using `"`artifact'"', replace
                    local dump_rc = _rc
                }
                else {
                    capture noisily parqit save `"`artifact'"', replace data
                    local dump_rc = _rc
                }
                if (`dump_rc' != 0) local rc = `dump_rc'
            }

            if (`rc' == 0 & !inlist("`workflow'", "describe", "path")) {
                capture noisily parqit describe `"`artifact'"'
                if (_rc == 0) {
                    local rows = r(n_rows)
                    local cols = r(n_cols)
                }
            }

            di as txt "RESULT workflow=`workflow' method=`method' iteration=`i' sequence=`sequence' rc=`rc' seconds=" ///
                as res %12.3f `seconds' as txt " rows=" as res %18.0fc `rows' as txt " cols=" as res `cols'
            post `rawpost' ("`workflow'") ("`method'") (`i') (`sequence') (`seconds') ///
                (`rc') (`rows') (`cols') (`"`artifact'"')

            capture parqit close _all
            clear
        }
    }
}
postclose `rawpost'

use `"`raw'"', clear
gen byte ok = (rc == 0)
gen byte failed = (rc != 0)
order workflow method iteration sequence seconds rc ok failed rows cols artifact
save `"`outdir'/stata_raw.dta"', replace
export delimited using `"`outdir'/stata_raw.csv"', replace

preserve
collapse (count) runs=seconds (sum) failures=failed ///
    (mean) mean_seconds=seconds (sd) sd_seconds=seconds ///
    (min) min_seconds=seconds (p50) p50_seconds=seconds ///
    (max) max_seconds=seconds, by(workflow method)
sort workflow method
save `"`outdir'/stata_summary.dta"', replace
export delimited using `"`outdir'/stata_summary.csv"', replace
di as txt "summary:"
list workflow method runs failures mean_seconds sd_seconds min_seconds p50_seconds ///
    max_seconds, noobs abbreviate(24)
restore

di as txt "raw results:     " as res `"`outdir'/stata_raw.csv"'
di as txt "summary results: " as res `"`outdir'/stata_summary.csv"'
di as txt "finished:        " as res "`c(current_date)' `c(current_time)'"
log close
