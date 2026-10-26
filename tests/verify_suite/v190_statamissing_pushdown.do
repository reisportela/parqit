* V190 — STATAMISS-PUSHDOWN-1 (25 Oct 2026). Under statamissing on, a comparison
* with a value that cannot be missing (a number, a date literal, strpos()) needs
* no missing-value branch where only its TRUE rows are kept — keep if, count if,
* the if of parqit use — so the condition reaches the Parquet scan as a filter.
* inrange() and inlist() with bounds and values that cannot be missing too.
* The rows must be native Stata's for every comparison form, with missing
* values on both sides (count if and keep if natively as the oracle, the ids
* kept summed), and a negation, a value context and the forms a missing value
* makes true keep their branches. explain shows the filter in the scan.
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
parqit set threads 4
parqit set statamissing on
local fails 0
tempfile src nat

clear
set obs 1000
gen long id = _n
gen double x = cond(mod(_n, 7) == 0, ., mod(_n, 11))
gen double y = cond(mod(_n, 5) == 0, ., mod(_n, 13) * 2)
gen str6 s = cond(mod(_n, 4) == 0, "", cond(mod(_n, 3) == 0, "abT", "cd"))
gen d = cond(mod(_n, 9) == 0, ., td(1jan2020) + mod(_n, 30))
format d %td
quietly save `"`nat'"', replace
parqit save `"`src'.parquet"', data replace

program define _v190_check
    args nat src cond
    use `"`nat'"', clear
    quietly count if `cond'
    local n_nat = r(N)
    quietly keep if `cond'
    quietly summarize id
    local s_nat = r(sum)
    parqit use using `"`src'.parquet"'
    quietly parqit count if `cond'
    local n_cnt = r(N)
    parqit keep if `cond'
    quietly parqit collect, clear
    quietly summarize id
    local s_keep = r(sum)
    local n_keep = _N
    quietly parqit use if `cond' using `"`src'.parquet"', clear
    quietly summarize id
    local s_use = r(sum)
    if (`n_cnt' != `n_nat' | `n_keep' != `n_nat' | `s_keep' != `s_nat' | _N != `n_nat' | `s_use' != `s_nat') {
        di as err `"FAIL: `cond': native `n_nat' rows (ids `s_nat'), count `n_cnt', keep `n_keep' (`s_keep'), use `=_N' (`s_use')"'
        c_local bad 1
    }
end

local bad 0
_v190_check `"`nat'"' `"`src'"' "x == 3"
_v190_check `"`nat'"' `"`src'"' "3 == x"
_v190_check `"`nat'"' `"`src'"' "x < 4"
_v190_check `"`nat'"' `"`src'"' "x <= 4"
_v190_check `"`nat'"' `"`src'"' "4 > x"
_v190_check `"`nat'"' `"`src'"' "x > 4"
_v190_check `"`nat'"' `"`src'"' "4 < x"
_v190_check `"`nat'"' `"`src'"' "x != 3"
_v190_check `"`nat'"' `"`src'"' "!(x == 3)"
_v190_check `"`nat'"' `"`src'"' "x == 3 | y > 10"
_v190_check `"`nat'"' `"`src'"' "x == 3 & y < 10"
_v190_check `"`nat'"' `"`src'"' "x == y"
_v190_check `"`nat'"' `"`src'"' "d == td(5jan2020)"
_v190_check `"`nat'"' `"`src'"' `"strpos(s, "T") > 0"'
_v190_check `"`nat'"' `"`src'"' "inrange(x, 2, 5)"
_v190_check `"`nat'"' `"`src'"' "inrange(x, ., 5)"
_v190_check `"`nat'"' `"`src'"' "inrange(x, 5, .)"
_v190_check `"`nat'"' `"`src'"' "!inrange(x, 2, 5)"
_v190_check `"`nat'"' `"`src'"' "inrange(d, td(3jan2020), td(9jan2020))"
_v190_check `"`nat'"' `"`src'"' "inlist(x, 1, 3, 5)"
_v190_check `"`nat'"' `"`src'"' "inlist(x, ., 3)"
_v190_check `"`nat'"' `"`src'"' "!inlist(x, 1, 3)"
_v190_check `"`nat'"' `"`src'"' "inlist(x, 1, 3) & y < 10"
if (`bad') local ++fails

* the filter reaches the scan
parqit use using `"`src'.parquet"'
parqit keep if d == td(5jan2020)
capture log close _v190
tempfile plan
log using `"`plan'"', text replace name(_v190)
parqit explain
log close _v190
* the scan carries the condition (Filters:) and no filter operator sits above
* it (FILTER in 1.5.6's boxes, Filter in 2.0's); the log's continuation lines
* are joined first, since they break words
python:
import re
from sfi import Macro, Scalar
text = open(Macro.getLocal("plan"), encoding="utf-8").read().replace("\n> ", "")
above = re.search(r"\bFILTER\b|─ Filter ─", text)
Scalar.setValue("pushed", int("Filters:" in text and above is None))
end
if (scalar(pushed) != 1) {
    di as err "FAIL: keep if d == td(5jan2020) does not reach the Parquet scan as a filter"
    type `"`plan'"'
    local ++fails
}

if (`fails' == 0) di "VERDICT(V190_STATAMISSING_PUSHDOWN): PASS"
else di "VERDICT(V190_STATAMISSING_PUSHDOWN): FAIL (`fails')"
