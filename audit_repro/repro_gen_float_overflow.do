* Independent repro: explicit float assignment overflows become native missing.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

tempfile src
clear
set obs 2
gen long id = _n
gen float native_hi = 1e300
gen float native_exp = exp(700)
gen float native_lo = -1e39
gen float native_ok = 1e38
parqit save `"`src'.parquet"', replace data

parqit use using `"`src'.parquet"'
parqit gen float got_hi = 1e300
parqit gen float got_exp = exp(700)
parqit gen float got_lo = -1e39
parqit gen float got_ok = 1e38
parqit collect, clear
assert got_hi == native_hi & got_exp == native_exp
assert got_lo == native_lo & got_ok == native_ok
assert `"`: type got_hi'"' == "float"
parqit close _all

di as result "VERDICT(REPRO_GEN_FLOAT_OVERFLOW): PASS - explicit float range follows native Stata"
