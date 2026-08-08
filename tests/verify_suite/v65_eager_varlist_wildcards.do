* V65 — Stata varlist wildcards cover eager/lazy reads and native bridges.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

tempfile stem source expected master
local parquet `"`stem'_source.parquet"'

clear
set obs 4
gen long id = _n
gen double x1 = 10 * _n
gen double x2 = 100 * _n
gen double café = 1000 * _n
gen double cafx = 2000 * _n
gen double caféz = 3000 * _n
gen double zed = 4000 * _n
save `"`source'"', replace
parqit save `"`parquet'"', replace data

* Eager and lazy use share native wildcard projection and column order.
use id x* using `"`source'"', clear
save `"`expected'"', replace
parqit use id x* using `"`parquet'"', clear
cf _all using `"`expected'"'

parqit use id x* using `"`parquet'"'
parqit collect, clear
cf _all using `"`expected'"'
parqit close _all

* '?' consumes one Unicode codepoint: caf? matches café and cafx, not caféz.
use caf? using `"`source'"', clear
save `"`expected'"', replace
parqit use caf? using `"`parquet'"', clear
cf _all using `"`expected'"'

parqit use using `"`parquet'"'
parqit keep caf?
parqit collect, clear
cf _all using `"`expected'"'
parqit close _all

* Projection pushdown used by mergein keepusing() accepts wildcards.
clear
set obs 4
gen long id = _n
gen double master_value = -_n
save `"`master'"', replace
merge 1:1 id using `"`source'"', keepusing(x*) nogenerate
save `"`expected'"', replace
use `"`master'"', clear
parqit mergein 1:1 id using `"`parquet'"', keepusing(x*) nogenerate
cf _all using `"`expected'"'

* Projection pushdown used by appendin keep() accepts wildcards too.
clear
set obs 1
gen double master_value = 99
save `"`master'"', replace
append using `"`source'"', keep(x*)
save `"`expected'"', replace
use `"`master'"', clear
parqit appendin using `"`parquet'"', keep(x*)
cf _all using `"`expected'"'

* A pattern with no match is loud and leaves the in-memory dataset intact.
clear
set obs 2
gen long sentinel = 40 + _n
capture noisily parqit use zzz* using `"`parquet'"', clear
assert _rc == 111
assert _N == 2 & sentinel[1] == 41 & sentinel[2] == 42

di as result "VERDICT(V65_EAGER_VARLIST_WILDCARDS): PASS - eager/lazy/use/mergein/appendin match native Unicode wildcards"
