* Public-surface coverage gaps: spelling aliases, every percentile, and the
* historical egen sum() alias. Independent oracle: native Stata on synthetic
* values spanning vector boundaries, ties, invalid domains and missing groups.
clear all
set more off
set linesize 255
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

set obs 4097
gen long id = _n
gen double x = cond(mod(id,17)==0,.,(id-2049)/16)
gen str30 s = cond(mod(id,3)==0,"  AbC  ",cond(mod(id,3)==1," ÁÉ ß  ",""))
gen double expected_log = log(x)
gen double expected_log10 = log10(x)
gen str30 expected_ltrim = ltrim(s)
gen str30 expected_rtrim = rtrim(s)
gen str30 expected_strlower = strlower(s)
parqit open _data
parqit gen double actual_log = log(x)
parqit gen double actual_log10 = log10(x)
parqit gen str30 actual_ltrim = ltrim(s)
parqit gen str30 actual_rtrim = rtrim(s)
parqit gen str30 actual_strlower = strlower(s)
parqit sort id
parqit collect, clear
assert _N==4097 & id==_n
assert missing(actual_log)==missing(expected_log)
assert missing(actual_log10)==missing(expected_log10)
assert reldif(actual_log,expected_log)<1e-15 if !missing(expected_log)
assert reldif(actual_log10,expected_log10)<1e-15 if !missing(expected_log10)
assert actual_ltrim==expected_ltrim
assert actual_rtrim==expected_rtrim
assert actual_strlower==expected_strlower
parqit close _all
di "VERDICT(V170_EXPRESSION_ALIASES): PASS"

clear
tempfile fixture expected original
set obs 4097
gen long id=_n
gen byte g=mod(id,7)
gen double x=(mod(id*67,211)-105)/8
replace x=. if mod(id,13)==0 | g==0
gen double y=cond(mod(id,11)==0,.,mod(id,17)/4)
gen double z=cond(mod(id,5)==0,.,-mod(id,19)/2)
save "`original'"
parqit save "`fixture'.parquet", data
local stats
forvalues p=1/99 {
    local stats "`stats' (p`p') p`p'=x"
}
collapse `stats', by(g)
recast double p*
save "`expected'"
parqit use using "`fixture'.parquet"
parqit collapse `stats', by(g)
parqit sort g
parqit collect, clear
cf _all using "`expected'"
parqit close _all
use "`original'", clear
forvalues p=1/99 {
    egen double p`p'=rowpctile(x y z), p(`p')
}
save "`expected'", replace
parqit use using "`fixture'.parquet"
forvalues p=1/99 {
    parqit egen double p`p'=rowpctile(x y z), p(`p')
}
parqit sort id
parqit collect, clear
cf _all using "`expected'"
parqit close _all
di "VERDICT(V170_PERCENTILE_GRID): PASS"

use "`original'", clear
egen double total=total(x), by(g)
egen double totalmiss=total(x), by(g) missing
egen double conditional=total(x) if mod(id,3)==0, by(g) missing
save "`expected'", replace
parqit use using "`fixture'.parquet"
parqit egen double total=sum(x), by(g)
parqit egen double totalmiss=sum(x), by(g) missing
parqit egen double conditional=sum(x) if mod(id,3)==0, by(g) missing
parqit sort id
parqit collect, clear
cf _all using "`expected'"
parqit close _all
di "VERDICT(V170_EGEN_SUM_ALIAS): PASS"
