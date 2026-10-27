* Shared menu registry/schema and explicit targets against native/PyArrow oracles.
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

clear
set obs 7
gen long id = _n
gen double aa = _n
parqit open _data, name(alpha)
clear
set obs 3
gen long id = _n
gen double bb = _n*10
gen str1 text = "x"
quietly count if bb>=20
local selected_n = r(N)
parqit open _data, name(beta)
parqit view alpha

quietly parqit _dlgviews no_dialog, view(beta) report
assert "`r(current)'"=="alpha" & "`r(selected)'"=="beta"
assert "`r(varlist)'"=="id bb text" & "`r(numeric)'"=="id bb"
assert "`r(kind)'"=="view"
parqit view beta: keep if bb>=20
parqit view beta: gen double q = bb/10
parqit view beta: format bb %9.2f
parqit view beta: count
assert r(N)==`selected_n'
parqit count
assert r(N)==7

* Save and collect must name beta, independently of alpha being current.
tempfile output
parqit view beta: save `"`output'"'
python:
import pyarrow.parquet as pq
from sfi import Macro
t = pq.read_table(Macro.getLocal("output")).sort_by("id")
assert t.column_names == ["id", "bb", "text", "q"]
assert t.column("id").to_pylist() == [2, 3]
assert t.column("bb").to_pylist() == [20.0, 30.0]
assert t.column("q").to_pylist() == [2.0, 3.0]
end
parqit view beta: collect, clear
assert _N==`selected_n'
assert id==_n+1 & bb==10*id & q==id
parqit count
assert r(N)==7

* Two selected views: using metadata/data belong to alpha, master to beta.
parqit view beta: merge 1:1 id using view:alpha, keep(match) keepusing(aa) generate(g)
parqit view beta: collect, clear
assert _N==`selected_n' & g==3 & aa==id & bb==10*id
quietly parqit _dlgviews no_dialog, view(beta) report
assert "`r(current)'"=="alpha" & "`r(selected)'"=="beta"
assert strpos(" `r(varlist)' "," aa ")>0

* Copy and time declarations are scoped to the selected view too.
parqit view beta: use id bb using view:beta, name(gamma)
quietly parqit _dlgviews no_dialog, view(gamma) report
assert "`r(current)'"=="alpha" & "`r(varlist)'"=="id bb"
parqit view gamma: tsset id
quietly parqit _dlgviews no_dialog, initial report
assert "`r(current)'"=="alpha" & "`r(selected)'"=="alpha"
capture noisily parqit view beta: count if absent_variable>0
assert _rc!=0
parqit count
assert r(N)==7

quietly correlate bb aa
matrix C=r(C)
scalar N=r(N)
scalar rho=r(rho)
quietly parqit _dlgviews no_dialog, view(beta)
assert r(N)==N & r(rho)==rho
mata: assert(mreldif(st_matrix("C"),st_matrix("r(C)"))==0)

* A closed selection cannot redirect collect to alpha or change memory.
quietly datasignature
local signature `"`r(datasignature)'"'
parqit close beta
quietly parqit _dlgviews no_dialog, view(beta) report
assert "`r(selected)'"=="" & "`r(current)'"=="alpha" & "`r(varlist)'"==""
capture noisily parqit view beta: collect, clear
assert _rc!=0
quietly datasignature
assert `"`r(datasignature)'"'==`"`signature'"'
parqit close _all
display "VERDICT(V194_ALL_DIALOG_VIEW_TARGETS): PASS"
