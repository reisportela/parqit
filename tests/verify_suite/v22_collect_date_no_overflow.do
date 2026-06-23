* V22 — a parquet DATE column collects without integer overflow.
*
* Regression guard for the date-aware storage floor on the collect path.
* `parqit use` stores a bare parquet DATE as Stata `long` (typemap rule). The
* collect path receives the column already cast to an integer day-count; if it
* range-refined that to `int`, any date past ~2049 (> 32740 days from 1960)
* would overflow. Both paths must store `d` as `long` with the EXACT day-count
* (independent pyarrow/duckdb oracle baked in below).
clear all
set more off
set varabbrev off

args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

local f "`repo'/tests/fixtures/far_dates.parquet"
* oracle day-counts since 1960-01-01 (id order 1..4)
local o1 = 0
local o2 = 18428
local o3 = 51134
local o4 = -21914

local nfail = 0

capture program drop _check
program define _check
    args path tag
    local fail = 0
    sort id
    local t : type d
    if ("`t'" != "long") {
        di as error "    `tag': d stored as `t', expected long"
        local fail = 1
    }
    foreach pair in 1/0 2/18428 3/51134 4/-21914 {
        gettoken i exp : pair, parse("/")
        local exp : subinstr local exp "/" ""
        quietly su d if id == `i', meanonly
        if (r(mean) != `exp') {
            di as error "    `tag': id=`i' d=" r(mean) " expected `exp' (OVERFLOW/precision loss)"
            local fail = 1
        }
    }
    c_local subfail = `fail'
end

* A) direct use
clear
capture parqit close _all
quietly parqit use using `"`f'"', clear
_check "`f'" "use"
local nfail = `nfail' + `subfail'

* B) view + collect
clear
capture parqit close _all
quietly parqit use using `"`f'"'
quietly parqit collect, clear
_check "`f'" "collect"
local nfail = `nfail' + `subfail'
capture parqit close _all

if (`nfail' == 0) di as result _n "VERDICT(v22_collect_date_no_overflow): PASS"
else              di as error  _n "VERDICT(v22_collect_date_no_overflow): FAIL (`nfail')"
