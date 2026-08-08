* V63 — explicit float gen/egen follows native assignment-range semantics.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

tempfile stem
local src `"`stem'_source.parquet"'
local out `"`stem'_lazy.parquet"'

clear
set obs 3
gen long id = _n
gen byte g = 1
gen double huge = 1e38
gen float native1 = 1e300
gen float native2 = exp(700)
gen float native3 = -1e39
gen float native4 = 1e38
egen float native5 = total(huge), by(g)
assert missing(native1) & missing(native2) & missing(native3)
assert !missing(native4) & missing(native5)
parqit save `"`src'"', replace data

program define _v63_add
    parqit gen float got1 = 1e300
    parqit gen float got2 = exp(700)
    parqit gen float got3 = -1e39
    parqit gen float got4 = 1e38
    parqit egen float got5 = total(huge), by(g)
end

parqit use using `"`src'"'
_v63_add
parqit collect, clear
forvalues i = 1/5 {
    assert got`i' == native`i'
    assert `"`: type got`i''"' == "float"
}
parqit close _all

* Exercise the Parquet-to-Parquet materialiser independently of collect.
parqit use using `"`src'"'
_v63_add
parqit save `"`out'"', replace
parqit close _all
parqit use using `"`out'"', clear
forvalues i = 1/5 {
    assert got`i' == native`i'
    assert `"`: type got`i''"' == "float"
}

di as result "VERDICT(V63_GEN_FLOAT_RANGE): PASS - gen/egen float overflow becomes missing on collect and lazy save"
