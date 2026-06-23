* Repro: parqit gsort -x should match Stata's missing-value order.
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

clear
input double x long id
1 1
. 2
3 3
4 4
2 5
end

tempfile src oracle base
qui save `"`src'"'
local pq `"`base'.parquet"'
parqit save `"`pq'"', replace

use `"`src'"', clear
gsort -x
gen long native_order = _n
keep id native_order
qui save `"`oracle'"'

clear
parqit use using `"`pq'"'
parqit gsort -x
parqit collect, clear
gen long parqit_order = _n
keep id parqit_order
merge 1:1 id using `"`oracle'"'
assert _merge == 3
drop _merge

count if parqit_order != native_order
if (r(N) > 0) {
    list, noobs
    di "VERDICT(REPRO_GSORT_MISSING_ORDER): FAIL - parqit gsort -x order differs from native Stata"
    exit 9
}

di "VERDICT(REPRO_GSORT_MISSING_ORDER): PASS - parqit gsort -x order matches native Stata"
