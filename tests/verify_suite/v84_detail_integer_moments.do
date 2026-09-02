* V84 — DETAIL-DECIMAL-1: `parqit summarize, detail` on an integer variable
*   whose values run far above its mean. The central moments were computed as
*   x - <mean literal>; DuckDB types the literal DECIMAL(p, scale) and drags an
*   int/long column into DECIMAL(18, scale), which overflows once a value
*   reaches 10^(18-scale): with a mean such as 255.48894316712128 any value
*   >= 10,000 aborted the command ("Could not cast value 99999 to
*   DECIMAL(18,14)", rc 920). The moments are now double arithmetic on both
*   sides, so byte/int/long/float columns give native's numbers.
* Oracle: native summarize, detail on the same data (every r() scalar).
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

tempfile stem
local src `"`stem'_src.parquet"'
set seed 20260902
clear
set obs 1301
gen long   nuest = floor(runiform() * 300)      /* mean ~150 (3 digits) ...   */
replace nuest = 99999 in 1                        /* ... one value of 5 digits   */
gen int    small = floor(runiform() * 20)         /* mean ~10, one 4-digit value */
replace small = 9999 in 1
gen byte   b = runiform() < 0.3                   /* mean ~0.3, 127 twice        */
replace b = 127 in 1/2
gen float  f = round(runiform() * 300, 0.5)       /* float moments in double     */
replace f = 99999 in 1
gen double d = runiform() * 300
replace d = 99999 in 1
replace nuest = . in 7
replace small = . in 8
replace b = . in 9
replace f = . in 10
replace d = . in 11
tempfile ref
save `"`ref'"', replace
parqit save `"`src'"', replace data

local nfail 0
foreach v in nuest small b f d {
    use `"`ref'"', clear
    summarize `v', detail
    foreach s in N mean sd Var skewness kurtosis min max p1 p5 p10 p25 p50 p75 p90 p95 p99 {
        local nat_`s' = r(`s')
    }
    parqit use using `"`src'"'
    capture noisily parqit summarize `v', detail
    if (_rc) {
        di as err "FAIL: parqit summarize `v', detail returned rc " _rc
        local ++nfail
        parqit close _all
        continue
    }
    if (r(N) != `nat_N') {
        di as err "FAIL: `v' r(N) = " r(N) " native " `nat_N'
        local ++nfail
    }
    * ulp-level tolerance: a percentile that averages two order statistics
    * may differ from native by one ulp (native does not round (a+b)/2 the
    * IEEE way), and min/max/mean travel as the engine's decimal text of the
    * double; integer data are exact either way
    foreach s in min max p1 p5 p10 p25 p50 p75 p90 p95 p99 {
        if (reldif(r(`s'), `nat_`s'') > 1e-15) {
            di as err "FAIL: `v' r(`s') = " %21.17g r(`s') " native " %21.17g `nat_`s''
            local ++nfail
        }
    }
    foreach s in mean sd Var {
        if (reldif(r(`s'), `nat_`s'') > 1e-12) {
            di as err "FAIL: `v' r(`s') = " %21.17g r(`s') " native " %21.17g `nat_`s''
            local ++nfail
        }
    }
    foreach s in skewness kurtosis {
        if (reldif(r(`s'), `nat_`s'') > 1e-9) {
            di as err "FAIL: `v' r(`s') = " %21.17g r(`s') " native " %21.17g `nat_`s''
            local ++nfail
        }
    }
    parqit close _all
}

* several variables in one call take the same path
parqit use using `"`src'"'
capture noisily parqit summarize nuest small b f d, detail
if (_rc | r(N) != 1300) {
    di as err "FAIL: multi-variable detail returned rc " _rc
    local ++nfail
}
parqit close _all

if (`nfail') {
    di "VERDICT(V84_DETAIL_INTEGER_MOMENTS): FAIL - `nfail' mismatch(es) against native summarize, detail"
    exit 9
}
di "VERDICT(V84_DETAIL_INTEGER_MOMENTS): PASS - summarize, detail on skewed byte/int/long/float/double columns returns native's N/mean/sd/Var/skewness/kurtosis/percentiles (no DECIMAL overflow)"
