* V34 — reshape long leading-zero suffix parity. Native Stata treats inc01 as
* evidence that j=1 exists, but carries inc01 as an ordinary variable and looks
* for the canonical xij column inc1. parqit must not use inc01 as the inc value.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

local fails 0
tempfile t oracle

* ---- canonical and leading-zero columns together --------------------------
clear
input id inc1 inc01
1 10 100
2 20 200
end
preserve
reshape long inc, i(id) j(year)
sort id year
order id year inc inc01
save `"`oracle'"', replace
restore
parqit save `"`t'_rz1.parquet"', replace data
parqit use using `"`t'_rz1.parquet"'
parqit reshape long inc, i(id) j(year)
parqit collect, clear
sort id year
order id year inc inc01
capture cf _all using `"`oracle'"'
if (_rc) di as err "FAIL RZ-1: inc01 was used as a long value instead of carried"
local fails = `fails' + (_rc!=0)

* ---- only the leading-zero column exists ----------------------------------
clear
input id inc01
1 100
2 200
end
preserve
reshape long inc, i(id) j(year)
sort id year
order id year inc inc01
save `"`oracle'"', replace
restore
parqit save `"`t'_rz2.parquet"', replace data
parqit use using `"`t'_rz2.parquet"'
parqit reshape long inc, i(id) j(year)
parqit collect, clear
sort id year
order id year inc inc01
capture cf _all using `"`oracle'"'
if (_rc) di as err "FAIL RZ-2: inc01-only case did not match native missing inc"
local fails = `fails' + (_rc!=0)

* ---- multiple numeric suffixes: canonical columns drive values -------------
clear
input id inc2 inc02 inc10
1 20 200 1000
2 21 201 1001
end
preserve
reshape long inc, i(id) j(year)
sort id year
order id year inc inc02
save `"`oracle'"', replace
restore
parqit save `"`t'_rz3.parquet"', replace data
parqit use using `"`t'_rz3.parquet"'
parqit reshape long inc, i(id) j(year)
parqit collect, clear
sort id year
order id year inc inc02
capture cf _all using `"`oracle'"'
if (_rc) di as err "FAIL RZ-3: leading-zero duplicate changed j rows or values"
local fails = `fails' + (_rc!=0)

di as txt "VERDICT(V34_RESHAPE_LEADING_ZERO_SUFFIX): " ///
    cond(`fails'==0, "PASS", "FAIL - `fails' failure(s)") ///
    " - leading-zero suffixes carry like native Stata"
