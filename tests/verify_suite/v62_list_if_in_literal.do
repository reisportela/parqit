* V62 — list parser: literal " in " text is not an in-qualifier.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

tempfile stem
local src `"`stem'_source.parquet"'

clear
set obs 5
gen long id = _n
gen double x = 6 - _n
gen str20 s = "first"
replace s = "x in y" in 2
replace s = "a in 3" in 3
replace s = "a in 3/4" in 4
replace s = "" in 5
quietly count if s == "a in 3"
local want_literal = r(N)
quietly count if strpos(s, " in ") > 0
local want_strpos = r(N)
quietly count if x == 5 in 1/2
local want_slice = r(N)
quietly count if s == "a in 3/4"
local want_slash_literal = r(N)
parqit save `"`src'"', replace data

parqit use using `"`src'"'
quietly parqit list if s == "a in 3"
assert r(N) == `want_literal'
quietly parqit list if strpos(s, " in ") > 0
assert r(N) == `want_strpos'
quietly parqit list if x == 5 in 1/2
assert r(N) == `want_slice'
quietly parqit list if s == "a in 3/4"
assert r(N) == `want_slash_literal'

* A default 20-row preview must not masquerade as an explicit in 1/20.
quietly parqit list
assert r(N) == 5
parqit close _all

di as result "VERDICT(V62_LIST_IF_IN_LITERAL): PASS - literals, real ranges and short bare previews are parsed safely"
