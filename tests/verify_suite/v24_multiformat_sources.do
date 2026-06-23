* V24 — parqit reads non-Parquet inputs: CSV (out-of-core via read_csv_auto) and
* .dta / .xlsx (imported to a Parquet bridge), as a `use` source and as a
* merge/joinby/append using side. Oracle: the same logical data written by
* Stata to every format, compared back after a parqit read.
clear all
set more off
set varabbrev off

args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

tempfile d
local D "`d'_mf"
shell mkdir -p "`D'"

* one logical table, written to every format
clear
input id str4 name value
1 "alfa" 100
2 "beta" 200
3 "gama" 300
end
export delimited "`D'/t.csv", replace
save             "`D'/t.dta", replace
export excel     "`D'/t.xlsx", firstrow(variables) replace
clear

local nfail = 0

* helper: read a source via parqit use,clear and check the 3 oracle rows
capture program drop _mf_check
program define _mf_check
    args tag
    local bad = 0
    if (_N != 3) local bad = 1
    capture confirm variable id
    if (_rc) local bad = 1
    else if (id[1]!=1 | id[3]!=3) local bad = 1
    capture confirm variable value
    if (_rc) local bad = 1
    else if (value[2]!=200) local bad = 1
    capture confirm string variable name
    if (_rc==0 & name[1]!="alfa") local bad = 1
    c_local subbad = `bad'
end

* --- 1) CSV source (out-of-core) ---
parqit use using "`D'/t.csv", clear
_mf_check "csv"
if (`subbad') {
    di as error "  FAIL: CSV source"
    local ++nfail
}
else di as result "  PASS: CSV source (read_csv_auto, out-of-core)"

* --- 2) DTA source (bridge) ---
parqit use using "`D'/t.dta", clear
_mf_check "dta"
if (`subbad') {
    di as error "  FAIL: DTA source"
    local ++nfail
}
else di as result "  PASS: DTA source (Parquet bridge)"

* --- 3) XLSX source (bridge) ---
parqit use using "`D'/t.xlsx", clear
_mf_check "xlsx"
if (`subbad') {
    di as error "  FAIL: XLSX source"
    local ++nfail
}
else di as result "  PASS: XLSX source (Parquet bridge)"

* --- 4) the headline workflow: lazy Parquet master + merge with .dta + collect
clear
input id str4 name owner
1 "alfa" 7
2 "beta" 8
3 "gama" 9
end
parqit open _data
parqit save "`D'/master.parquet", replace
clear

parqit use using "`D'/master.parquet"            // master view, NOT in Stata yet
local mem_after_use = _N                        // working dataset still empty
parqit merge 1:1 id using "`D'/t.dta", keepusing(value) nogenerate
parqit collect, clear
local ok4 = (_N==3) & (`mem_after_use'==0)
sort id
if (`ok4') if (value[3]!=300) local ok4 = 0       // value came from the .dta
if (`ok4') di as result "  PASS: lazy master + merge(.dta) + collect (master stayed out of memory)"
else {
    di as error "  FAIL: merge-with-dta workflow (N=`=_N' mem_after_use=`mem_after_use')"
    local ++nfail
}

* --- 5) joinby with a CSV using side ---
parqit use using "`D'/master.parquet"
parqit joinby id using "`D'/t.csv"
parqit collect, clear
if (_N==3) di as result "  PASS: joinby with CSV using side"
else {
    di as error "  FAIL: joinby with CSV (N=`=_N')"
    local ++nfail
}

parqit close _all

if (`nfail' == 0) di as result _n "VERDICT(v24_multiformat_sources): PASS"
else              di as error  _n "VERDICT(v24_multiformat_sources): FAIL (`nfail')"
