* V61 — table-driven scalar-expression parity against live native Stata.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

tempfile stem
local src `"`stem'_source.parquet"'

clear
set obs 6
gen double x = .
replace x = -2.7 in 1
replace x = 0 in 2
replace x = 1 in 3
replace x = 2.5 in 4
replace x = 1e-5 in 5
gen double d = td(29feb2020) + _n - 3 + .75
replace d = . in 6
gen strL s = ""
replace s = "abc" in 2
replace s = "éa" in 3
replace s = " a in b " in 4
replace s = "zé" in 5
replace s = "z" in 6

* One row in this table is one native/parqit expression contract.
local cases 31
local e1  `"strpos(s,"")"'
local e2  `"strpos(s,"é")"'
local e3  `"substr(s,1,4)"'
local e4  `"subinstr(s,"","X",.)"'
local e5  "mod(x,0)"
local e6  "mod(x,-3)"
local e7  "round(x)"
local e8  "round(x,-2)"
local e9  "round(x,0)"
local e10 "2^3^2"
local e11 "-x^2"
local e12 "x/0"
local e13 "exp(800)"
local e14 "ln(0)"
local e15 "sqrt(-1)"
local e16 "min(x,.)"
local e17 "max(.,.)"
local e18 "inrange(.,1,5)"
local e19 "inlist(x,1,.)"
local e20 "cond(x,1,2)"
local e21 "cond(x,1,2,9)"
local e22 "int(-2.7)"
local e23 "floor(-2.7)"
local e24 "ceil(-2.7)"
local e25 `"real("inf")"'
local e26 "string(x)"
local e27 "td(29feb2020)"
local e28 "mdy(2,30,2020)"
local e29 "dow(d)"
local e30 "doy(d)"
local e31 "mofd(d)"

forvalues i = 1/`cases' {
    local t`i' double
}
local t3 strL
local t4 strL
local t26 strL

* Materialise the native oracle into the source itself.
forvalues i = 1/`cases' {
    local expr `"`e`i''"'
    quietly gen `t`i'' native`i' = `expr'
}
parqit save `"`src'"', replace data

local fails 0
foreach mode in off on {
    parqit use using `"`src'"'
    parqit set statamissing `mode'
    forvalues i = 1/`cases' {
        local expr `"`e`i''"'
        quietly parqit gen `t`i'' got`i' = `expr'
    }
    quietly parqit collect, clear
    forvalues i = 1/`cases' {
        capture assert got`i' == native`i'
        if (_rc) {
            di as err `"DIFF mode=`mode' expression `i': `e`i''"'
            local ++fails
        }
    }
    parqit close _all
}
parqit set statamissing off

if (`fails' == 0) {
    di as result "VERDICT(V61_EXPR_NATIVE_ORACLE): PASS - 31 expressions match native Stata in SQL and statamissing modes"
}
else {
    di as err "VERDICT(V61_EXPR_NATIVE_ORACLE): FAIL - `fails' expression/mode differences"
}
