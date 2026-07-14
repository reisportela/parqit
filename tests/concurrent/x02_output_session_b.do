clear all
set more off
set varabbrev off
args repo plugin root
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

local dest `"`root'/shared_output.parquet"'
clear
set obs 2
gen long owner = 222
gen long id = _n
capture noisily parqit save `"`dest'"', replace data
local saverc = _rc
assert `saverc' != 0
assert _N == 2 & owner[1] == 222 & owner[2] == 222
di "VERDICT(X02_OUTPUT_SESSION_B): PASS"
