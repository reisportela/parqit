* Independent repro: .a-.z identity is unavailable after a Parquet boundary.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

tempfile src
clear
set obs 3
gen long id = _n
gen double x = cond(_n == 3, ., _n)
parqit save `"`src'.parquet"', replace data

parqit use using `"`src'.parquet"'
capture noisily parqit keep if x == .a
assert _rc != 0
capture noisily parqit gen bad = (x != .z)
assert _rc != 0
quietly parqit count
assert r(N) == 3
parqit collect, clear
capture confirm variable bad
assert _rc == 111
parqit close _all

di as result "VERDICT(REPRO_EXTENDED_MISSING_LITERALS): PASS - lossy literals are rejected without mutating the view"
