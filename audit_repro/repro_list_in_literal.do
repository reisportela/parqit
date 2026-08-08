* Independent repro: literal " in " text must not be parsed as an in qualifier.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

tempfile src
clear
set obs 5
gen long id = _n
gen str12 s = cond(_n == 3, "a in 3/4", "plain")
parqit save `"`src'.parquet"', replace data

parqit use using `"`src'.parquet"'
parqit list if s == "a in 3/4"
parqit list if strpos(s, " in ") > 0
parqit list in 2/4
parqit list
quietly parqit count
assert r(N) == 5
parqit close _all

di as result "VERDICT(REPRO_LIST_IN_LITERAL): PASS - literals, qualifiers and short bare previews are distinct"
