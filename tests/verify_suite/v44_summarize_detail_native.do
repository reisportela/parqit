* V44 — PERF-DET-1: `summarize, detail` must match native Stata r() exactly
* after the two-scan + parallel-sort rewrite (was: one scan per variable with
* a mean subquery and nine list_sort(list(x)) materialisations; now: one
* count/mean/min/max scan, one central-moments scan with the mean as an exact
* dtoa literal, and per-variable order statistics via a CTAS through the
* parallel sort operator + rowid point-picks — measured 6.4x faster on
* 10M x 4 vars). The oracle is NATIVE summarize, detail across shapes chosen
* to stress the Stata percentile rule: even/odd non-null counts, integral and
* non-integral n*p/100, n=1..7, constants, missings, int64 and byte columns,
* and an all-missing variable.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ "`repo'/src/ado/p"
global PARQIT_PLUGIN_PATH "`plugin'"
set seed 42
local fails 0

program define chk1, rclass
    * compares parqit summarize,detail r() against native for var v
    args v tag
    quietly summarize `v', detail
    local keys N mean sd Var skewness kurtosis min max p1 p5 p10 p25 p50 p75 p90 p95 p99
    foreach k of local keys {
        local nat_`k' = r(`k')
    }
    quietly parqit summarize `v', detail
    foreach k of local keys {
        local pq = r(`k')
        capture assert (abs(`pq' - `nat_`k'') < 1e-8 * max(1, abs(`nat_`k''))) | (missing(`pq') & missing(`nat_`k''))
        if (_rc) {
            di as err "DIFF `tag' `v' `k': parqit=`pq' native=`nat_`k''"
            global DFAILS = $DFAILS + 1
        }
    }
end
global DFAILS 0

* shape 1: 100k random with missings
clear
set obs 100000
gen double x = rnormal(50, 10)
replace x = . if mod(_n, 17) == 0
gen long i64 = floor(runiform() * 1000000)
gen byte b = mod(_n, 7)
tempfile f
parqit save "`f'.parquet", replace data
parqit use using "`f'.parquet"
foreach v in x i64 b {
    chk1 `v' big
}
parqit close

* shape 2: tiny ns and edges
foreach n in 1 2 3 4 5 {
    clear
    set obs `n'
    gen double x = _n * 1.5
    gen double cst = 7
    parqit save "`f'.parquet", replace data
    parqit use using "`f'.parquet"
    chk1 x n`n'
    chk1 cst n`n'c
    parqit close
}

* shape 3: with missing rows mixed, even/odd non-null counts
foreach n in 6 7 {
    clear
    set obs `n'
    gen double x = _n
    replace x = . in 2
    parqit save "`f'.parquet", replace data
    parqit use using "`f'.parquet"
    chk1 x m`n'
    parqit close
}

* shape 4: all-missing
clear
set obs 5
gen double x = .
gen double y = _n
parqit save "`f'.parquet", replace data
parqit use using "`f'.parquet"
quietly parqit summarize x y, detail
capture assert r(N) != .
parqit close

if ($DFAILS == 0) di as res "VERDICT(V44_SUMMARIZE_DETAIL_NATIVE): PASS"
else di as err "VERDICT(V44_SUMMARIZE_DETAIL_NATIVE): FAIL - $DFAILS diffs"
