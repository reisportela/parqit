* V64 — extended-missing literals are rejected after the Parquet boundary.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

tempfile stem
local src `"`stem'_source.parquet"'

clear
set obs 4
gen long id = _n
gen double x = _n - 2
replace x = . in 4
gen byte native_eqdot = (x == .)
gen byte native_ltdot = (x < .)
gen byte native_gedot = (x >= .)
parqit save `"`src'"', replace data

parqit use using `"`src'"'
quietly parqit count
local n0 = r(N)

capture noisily parqit keep if x == .a
assert _rc != 0
quietly parqit count
assert r(N) == `n0'

capture noisily parqit gen byte bad = (x != .b)
assert _rc != 0
quietly parqit count
assert r(N) == `n0'

* Ordinary missing remains representable and follows the selected mode.
parqit gen byte got_eqdot = (x == .)
parqit gen byte got_ltdot = (x < .)
parqit gen byte got_gedot = (x >= .)
parqit collect, clear
assert got_eqdot == native_eqdot
assert got_ltdot == native_ltdot
assert got_gedot == native_gedot
capture confirm variable bad
assert _rc == 111
parqit close _all

di as result "VERDICT(V64_EXTENDED_MISSING_LITERALS): PASS - .a-.z are loud and atomic; bare . remains faithful"
