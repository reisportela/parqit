* CHARTER 13 (pq finding 13): in-ranges are validated — negative, inverted
* and beyond-EOF forms are explicit errors (pq returned 0 rows or all rows
* with rc 0); valid ranges return exactly the named slice.
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

clear
set obs 5
gen x = _n * 11
tempfile fbase
local f `"`fbase'.parquet"'
parqit save `"`f'"', replace

* valid forms
parqit use using `"`f'"'
parqit sort x
parqit keep in 2/4
parqit collect, clear
assert _N == 3 & x[1] == 22 & x[3] == 44

clear
parqit use using `"`f'"'
parqit sort x
parqit keep in 3/3
parqit count
assert r(N) == 1
parqit close

* a single number is that observation alone (native semantics)
clear
parqit use using `"`f'"'
parqit sort x
parqit keep in 2
parqit collect, clear
assert _N == 1 & x[1] == 22

* invalid forms are loud at the verb (form) or at materialisation (EOF)
clear
parqit use using `"`f'"'
capture parqit keep in -2/-1
assert _rc != 0
capture parqit keep in 4/2
assert _rc != 0
capture parqit keep in 0/3
assert _rc != 0
parqit keep in 2/100
capture parqit count
assert _rc != 0
capture parqit collect, clear
assert _rc != 0
parqit close

di "VERDICT(V13_IN_RANGES): PASS - ranges validated; invalid forms error, never silent empty/full results"
