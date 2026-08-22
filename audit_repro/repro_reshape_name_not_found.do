* Minimal repro for RESHAPE-NAME-1. Before the 09aug2026 fix, reshape long
* i(nosuchvar) and reshape wide i()/j(nosuchvar) ran validation SQL first and
* leaked DuckDB Binder Error text plus __parqit_s0 with rc 920. The final audit
* also pinned unmatched long/wide stubs to native variable-not-found rc 111.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/ado/plus/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

tempfile stem
local src `"`stem'_src.parquet"'
clear
set obs 4
gen long id = _n
gen double x = _n
gen byte j = mod(_n, 2) + 1
gen double x1 = _n * 10
gen double x2 = _n * 20
parqit save `"`src'"', replace data

parqit use using `"`src'"'
capture noisily parqit reshape long nosuch, i(id) j(k)
assert _rc == 111
parqit count
assert r(N) == 4

capture noisily parqit reshape long x, i(nosuchvar) j(k)
assert _rc == 111
parqit count
assert r(N) == 4

capture noisily parqit reshape wide nosuchvar, i(id) j(j)
assert _rc == 111
parqit count
assert r(N) == 4

capture noisily parqit reshape wide x, i(id) j(nosuchvar)
assert _rc == 111
parqit count
assert r(N) == 4

capture noisily parqit reshape wide x, i(nosuchvar) j(j)
assert _rc == 111
parqit count
assert r(N) == 4
parqit close _all

di as result "VERDICT(REPRO_RESHAPE_NAME_NOT_FOUND): PASS - reshape validates stubs and i()/j() names before querying"
