clear all
set more off
set varabbrev off
args repo plugin root
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

local dest `"`root'/shared_output.parquet"'
clear
set obs 2
gen long owner = 111
gen long id = _n
capture noisily parqit save `"`dest'"', replace data
assert _rc == 0
clear
parqit use using `"`dest'"', clear
assert _N == 2 & owner[1] == 111 & owner[2] == 111
di "VERDICT(X02_OUTPUT_SESSION_A): PASS"
