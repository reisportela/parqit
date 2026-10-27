* Explore selection: registry/schema only, explicit target and current restored.
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

quietly parqit _dlgexplore no_dialog, initial report
assert "`r(views)'"=="" & "`r(current)'"=="" & "`r(selected)'"==""
assert strpos("`r(context)'","none")>0

clear
set obs 7
gen double alpha_only = _n
parqit open _data, name(alpha)

clear
set obs 3
gen double beta_only = _n
gen double beta_second = _n^2
parqit open _data, name(beta)
parqit view alpha
quietly datasignature
local signature `"`r(datasignature)'"'

quietly correlate beta_only beta_second
matrix C=r(C)
scalar N=r(N)
scalar rho=r(rho)
quietly parqit _dlgexplore no_dialog, initial
assert r(N)==N & r(rho)==rho
mata: assert(mreldif(st_matrix("C"),st_matrix("r(C)"))==0)

quietly parqit _dlgexplore no_dialog, initial report
assert "`r(views)'"=="alpha beta"
assert "`r(current)'"=="alpha" & "`r(selected)'"=="alpha"
assert "`r(varlist)'"=="alpha_only"

quietly parqit _dlgexplore no_dialog, view(beta) report
assert "`r(current)'"=="alpha" & "`r(selected)'"=="beta"
assert "`r(varlist)'"=="beta_only beta_second"
assert r(N)==N & r(rho)==rho
mata: assert(mreldif(st_matrix("C"),st_matrix("r(C)"))==0)

parqit view beta: count
assert r(N)==3
parqit count
assert r(N)==7
capture noisily parqit view beta: count if absent_variable > 0
assert _rc!=0
quietly parqit _dlgexplore no_dialog, initial report
assert "`r(current)'"=="alpha" & "`r(selected)'"=="alpha"

* A stale choice is cleared, without falling back to the session's current view.
parqit close beta
quietly parqit _dlgexplore no_dialog, view(beta) report
assert "`r(views)'"=="alpha" & "`r(current)'"=="alpha"
assert "`r(selected)'"=="" & "`r(varlist)'"==""
assert strpos("`r(context)'","closed")>0

* Long names are complete, and creating a view is visible on the next refresh.
parqit open _data, name(abcdefghijklmnopqrstuvwxyz123456)
quietly parqit _dlgexplore no_dialog, view(abcdefghijklmnopqrstuvwxyz123456) report
assert strpos(" `r(views)' "," abcdefghijklmnopqrstuvwxyz123456 ")>0
assert "`r(selected)'"=="abcdefghijklmnopqrstuvwxyz123456"
assert "`r(varlist)'"=="beta_only beta_second"

parqit close _all
quietly parqit _dlgexplore no_dialog, initial report
assert "`r(views)'"=="" & "`r(current)'"=="" & "`r(selected)'"=="" & "`r(varlist)'"==""
quietly datasignature
assert `"`r(datasignature)'"'==`"`signature'"'
display "VERDICT(V193_EXPLORE_VIEW_SELECTION): PASS"
