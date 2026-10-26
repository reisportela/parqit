* DEDUP-SORT-1: compact sort keys over a whole-row duplicates drop keep the
* native result, order, storage, missing placement and metadata on both
* materialisers; signed zero and fractional columns keep their own paths.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
parqit set threads 4
tempfile source native descending
set obs 300000
gen double key=mod(_n,251)-128
replace key=. if mod(_n,199)==0
gen double second=mod(_n,127)
gen double fraction=cond(mod(_n,17),mod(_n,1000)/8,.)
gen double big=cond(mod(_n,3),2^40+mod(_n,97),-2^40)
gen double signed=cond(mod(_n,5)==0,-0,mod(_n,7))
gen int day=td(22jul2021)+mod(_n,49)
format day %td
format key %15.0g
label variable key "dedup key"
* every row appears twice: duplicates drop must keep one of each
expand 2
parqit save "`source'.parquet", data
duplicates drop
sort key second fraction big signed day
save "`native'"

parqit use using "`source'.parquet"
parqit duplicates drop
parqit sort key second fraction big signed day
parqit collect, clear
assert _N==300000
cf _all using "`native'", all
assert "`: type key'"=="double" & "`: type day'"=="int" & "`: format key'"=="%15.0g"
assert "`: variable label key'"=="dedup key" & "`: format day'"=="%td"
assert "`: sortedby'"=="key second fraction big signed day"
parqit use using "`source'.parquet"
parqit duplicates drop
parqit sort key second fraction big signed day
parqit save "`source'_view.parquet", view
parqit use using "`source'_view.parquet", clear
cf _all using "`native'", all

* Descending first key: parqit places missing values last in either direction.
use "`native'", clear
gen byte miss=missing(key)
gsort miss -key second fraction big signed day
drop miss
save "`descending'"
parqit use using "`source'.parquet"
parqit duplicates drop
parqit gsort -key second fraction big signed day
parqit collect, clear
cf _all using "`descending'", all
parqit close _all
di "VERDICT(V178_DEDUP_SORT_STORAGE): PASS - whole-row dedup then sort/gsort equals native order, values, storage and metadata on collect and view save"
