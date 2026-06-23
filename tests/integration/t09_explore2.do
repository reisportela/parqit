* Extended exploration kit: count if, list previews, codebook, distinct,
* duplicates report/list, misstable patterns, tab options, tabstat,
* correlate/pwcorr, histogram — push-down only, oracles native where cheap.
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

* ---------- fixture ----------
clear
set obs 600
set seed 11
gen long id   = ceil(_n / 3)          // 200 ids ×3
gen int  year = 2018 + mod(_n, 3)
gen str1 g    = char(97 + mod(_n, 2))
gen double wage = exp(rnormal(2, .5)) * 10
replace wage = . if mod(_n, 13) == 0
gen double age = 18 + mod(_n, 50)
replace age = . if mod(_n, 100) == 0
tempfile fb
local fix `"`fb'.parquet"'
parqit save `"`fix'"', replace
tempfile fdta
qui save `"`fdta'"'

* native oracles
qui count if missing(wage)
local o_misswage = r(N)
qui count if age >= 30 & age <= 40 & !missing(wage)
local o_band = r(N)
qui duplicates report id
local o_uniq = r(unique_value)
local o_surp = r(N) - r(unique_value)
qui correlate wage age
local o_rho = r(rho)
local o_corrN = r(N)
qui count if g == "a" & year == 2019
local o_cell = r(N)
qui count if g == "a"
local o_rowa = r(N)
bysort g: egen double o_mw = mean(wage)
qui summ o_mw if g == "b", meanonly
local o_mw_b = r(mean)
drop o_mw

* sentinel
clear
set obs 2
gen sentinel = _n
datasignature set, reset

parqit use using `"`fix'"', name(x)

* ---------- count if (non-mutating) ----------
parqit count if missing(wage)
assert r(N) == `o_misswage'
parqit count if age >= 30 & age <= 40 & !missing(wage)
assert r(N) == `o_band'
parqit count
assert r(N) == 600          // the view itself was never filtered

* ---------- list previews ----------
parqit list id year wage in 1/5
assert r(N) == 5
* Stata semantics: `in' slices the rows first, `if' filters within them —
* rows 1..26 contain exactly two missing wages (13 and 26)
parqit list id wage if missing(wage) in 1/26
assert r(N) == 2
parqit list id wage if missing(wage)
assert r(N) == `o_misswage'

* ---------- ds / lookfor / codebook / distinct ----------
parqit ds
assert "`r(varlist)'" == "id year g wage age"
parqit lookfor wage salary
assert "`r(varlist)'" == "wage"
parqit codebook
parqit distinct id
assert r(ndistinct) == 200
parqit distinct id year, joint
assert r(ndistinct) == 600

* ---------- duplicates ----------
parqit duplicates report id
assert r(unique_value) == `o_uniq'
assert r(surplus) == `o_surp'
parqit duplicates list id year, limit(5)

* ---------- misstable patterns ----------
parqit misstable patterns wage age
assert r(r) >= 3            // (+,+), (.,+), (+,.) at least

* ---------- tabulate options ----------
parqit tabulate g year
assert r(N) == 600 & r(r) == 2 & r(c) == 3
parqit tabulate g year, row col
parqit tabulate g, missing
assert r(N) == 600

* ---------- tabstat ----------
parqit tabstat wage age, statistics(n mean sd p50 max)
parqit tabstat wage, statistics(mean) by(g)

* ---------- correlate / pwcorr ----------
parqit correlate wage age
assert reldif(r(rho), `o_rho') < 1e-12
assert r(N) == `o_corrN'
parqit pwcorr wage age, obs sig
assert reldif(r(rho), `o_rho') < 1e-12

* ---------- histogram (bins must account for every nonmissing value) ----------
parqit histogram wage, bins(20) nodraw
assert r(bins) == 20
assert r(N) == 600 - `o_misswage'

* ---------- loud errors ----------
capture parqit count if frobnicate(wage)
assert _rc != 0
capture parqit tabstat g, statistics(mean)
assert _rc != 0
capture parqit correlate wage
assert _rc != 0
capture parqit histogram g, nodraw
assert _rc != 0

* memory untouched by everything above
datasignature confirm
assert sentinel[2] == 2
parqit close _all

di "VERDICT(T09_EXPLORE2): PASS - count-if/list/codebook/distinct/dups/patterns/tab-opts/tabstat/corr/hist all pushdown"
