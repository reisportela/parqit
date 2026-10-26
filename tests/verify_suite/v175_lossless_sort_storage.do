* Physical integer keys must preserve the public values, storage, metadata,
* null placement and stable ties; fractional keys keep their original path.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
parqit set threads 4
tempfile source native descending
set obs 262149
gen long id=_n
gen double key=mod(_n,251)-128
replace key=.a if mod(_n,199)==0
gen double second=mod(_n,127)
gen double fractional=cond(mod(_n,17),_n/7,.z)
gen int day=td(22jul2021)+mod(_n,49)
format day %td
format key %15.0g
label variable key "lossless key"
label define levels -128 "negative bound" 100 "hundred"
label values key levels
char key[proof] "keep this characteristic"
label data "physical sort test"
parqit save "`source'.parquet", data xmissing
* The existing lazy-view contract normalises extended missing companions to .;
* eager use restores .a/.z. Match that public boundary in the native oracle.
replace key=. if missing(key)
replace fractional=. if missing(fractional)
sort key second, stable
save "`native'"

parqit use using "`source'.parquet"
parqit sort key second
parqit collect, clear
cf _all using "`native'", all
assert "`: type key'"=="double" & "`: format key'"=="%15.0g"
assert "`: variable label key'"=="lossless key" & "`: value label key'"=="levels"
assert "`key[proof]'"=="keep this characteristic"
assert "`: data label'"=="physical sort test"
assert "`: sortedby'"=="key second"
parqit save "`source'_sorted.parquet", view
parqit use using "`source'_sorted.parquet", clear
cf _all using "`native'", all

* Native gsort puts extended missing values first for a descending key;
* parqit's documented contract puts all missing values last in either direction.
use "`native'", clear
gen byte miss=missing(key)
gsort miss -key second id
drop miss
save "`descending'"
parqit use using "`source'.parquet"
parqit gsort -key second
parqit collect, clear
cf _all using "`descending'", all
assert "`: sortedby'"==""

* A late fraction cannot be rounded by a narrowed key, including ties on a
* second declared key and a date whose public storage/format must stay intact.
parqit use using "`source'.parquet"
parqit sort fractional day id
parqit collect, clear
assert fractional==cond(mod(id,17),id/7,.)
assert day==td(22jul2021)+mod(id,49)
assert "`: type fractional'"=="double" & "`: format day'"=="%td"
sort id
assert id==_n

* Raw SQL and dependent stages are excluded from any repeated proof scan.
parqit sql "SELECT i::DOUBLE k, random() x FROM range(10000) t(i)"
parqit sort k
parqit gen double same=x
parqit collect, clear
assert x==same & k==_n-1
parqit close _all
di "VERDICT(V175_LOSSLESS_SORT_STORAGE): PASS - exact keys, missing placement, ties, fractional fallback, types, metadata, dates and single volatile evaluation"
