* V92: dialog metadata, numeric pickers and explicit materialisation targets.
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

sysuse auto, clear
parqit open _data, name(autos)
quietly parqit _dlgvars no_dialog vv_list, numeric(numeric_list)
assert r(k)==12
assert strpos(" `r(varlist)' "," make ")>0
assert strpos(" `r(numeric)' "," make ")==0
assert strpos(" `r(numeric)' "," price ")>0

quietly correlate price mpg weight
matrix C=r(C)
scalar N=r(N)
scalar rho=r(rho)
quietly parqit _dlgcontext no_dialog
assert r(N)==N & r(rho)==rho
mata: assert(mreldif(st_matrix("C"),st_matrix("r(C)"))==0)
quietly parqit _dlgsource no_dialog using "input.CsV"
assert r(N)==N & r(rho)==rho
mata: assert(mreldif(st_matrix("C"),st_matrix("r(C)"))==0)
quietly parqit _dlgcontext no_dialog, report
assert "`r(view)'"=="autos"

foreach ext in csv CsV TSV txt tab DTA XLS xLsX {
    quietly parqit _dlgsource no_dialog using "input.`ext'", report
    assert r(footer)==0
}
foreach path in "input.parquet" "data_*.parquet" "directory" {
    quietly parqit _dlgsource no_dialog using `"`path'"', report
    assert r(footer)==1
}
quietly parqit _dlgsource no_dialog, report
assert r(footer)==0

* Memory-save selection must use memory variables, independently of an open view.
clear
set obs 2
gen double memory_num=_n
gen str1 memory_str="x"
quietly parqit _dlgvars no_dialog vv_list, data numeric(numeric_list)
assert "`r(varlist)'"=="memory_num memory_str"
assert "`r(numeric)'"=="memory_num"
quietly parqit _dlgcontext no_dialog, data report
assert "`r(context)'"=="Source: dataset in Stata memory"

tempfile output selected
parqit save `"`output'"', data
assert r(N)==2 & r(k)==2
parqit view autos: save `"`selected'"'
assert r(N)==74 & r(k)==12
parqit close autos
capture parqit view autos: save `"`selected'"', replace
assert _rc!=0
parqit describe `"`selected'"'
assert r(n_rows)==74
quietly parqit _dlgvars no_dialog vv_list, numeric(numeric_list)
assert r(k)==0 & "`r(varlist)'"==""
quietly parqit _dlgcontext no_dialog, report
assert "`r(view)'"==""
di "VERDICT(V92_DIALOG_CONTEXT_CONTRACTS): PASS"
