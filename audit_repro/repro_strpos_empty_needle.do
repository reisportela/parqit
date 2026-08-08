* Independent repro: native strpos() distinguishes empty and non-empty haystacks.
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
gen str3 s = cond(_n == 1, "abc", "")
replace s = "z" in 3
gen double native = strpos(s, "")
parqit save `"`src'.parquet"', replace data

parqit use using `"`src'.parquet"'
parqit gen double got = strpos(s, "")
parqit collect, clear
assert got == native
assert got[1] == 1 & got[2] == 0 & got[3] == 1
parqit close _all

di as result "VERDICT(REPRO_STRPOS_EMPTY_NEEDLE): PASS - parqit matches the live native oracle"
