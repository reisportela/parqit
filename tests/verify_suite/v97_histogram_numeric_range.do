* N7: range errors precede SQL; valid extreme bin centers remain finite.
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
set obs 2
gen double x = cond(_n==1,-1e-323,1e-323)
parqit open _data
capture noisily parqit histogram x, bins(1000) nodraw
assert _rc==198
parqit histogram x, bins(2) nodraw
assert r(N)==2 & r(width)>0 & r(width)<.
parqit close _all
replace x = cond(_n==1,-8e307,8e307)
parqit open _data
capture noisily parqit histogram x, bins(1) nodraw
assert _rc==198
parqit histogram x, bins(2) nodraw
assert r(N)==2 & r(width)==8e307
parqit close _all
di "VERDICT(V97_HISTOGRAM_NUMERIC_RANGE): PASS"
