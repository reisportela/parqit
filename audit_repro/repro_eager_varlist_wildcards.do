* Independent repro: eager projection expands Stata wildcards by Unicode character.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

tempfile src oracle
clear
set obs 2
gen double café = _n
gen double cafx = 10 * _n
gen double caféz = 100 * _n
gen double z = 1000 * _n
save `"`src'"', replace
parqit save `"`src'.parquet"', replace data

use caf? using `"`src'"', clear
save `"`oracle'"', replace
parqit use caf? using `"`src'.parquet"', clear
cf _all using `"`oracle'"'
capture confirm variable caféz
assert _rc == 111

di as result "VERDICT(REPRO_EAGER_VARLIST_WILDCARDS): PASS - '?' matches one Unicode codepoint"
