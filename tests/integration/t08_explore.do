* Exploration kit on lazy views: misstable, summarize detail (Stata's exact
* moment and percentile definitions), two-way tabulate, levelsof — all
* push-down, nothing materialised, memory untouched. Native Stata computes
* the same statistics on the same data as the oracle.
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

* ---------- fixture ----------
clear
set obs 1000
set seed 7
gen long id = _n
gen str1 g = char(97 + mod(_n, 3))     // a b c
gen str2 h = "h" + string(mod(_n, 4))  // h0..h3
gen double y = exp(rnormal(0, 1)) * (1 + mod(_n, 5))
replace y = . if mod(_n, 11) == 0
gen double z = _n / 7
replace z = . if mod(_n, 100) == 0
tempfile fb
local fix `"`fb'.parquet"'
parqit save `"`fix'"', replace
tempfile fdta
qui save `"`fdta'"'

* native oracles
qui count if missing(y)
local o_missy = r(N)
qui count if missing(z)
local o_missz = r(N)
qui summ y, detail
local o_n = r(N)
local o_mean = r(mean)
local o_sd = r(sd)
local o_var = r(Var)
local o_skew = r(skewness)
local o_kurt = r(kurtosis)
local o_p1 = r(p1)
local o_p25 = r(p25)
local o_p50 = r(p50)
local o_p75 = r(p75)
local o_p99 = r(p99)
qui count if g == "a" & h == "h1"
local o_cell = r(N)
qui levelsof g, local(o_lv)
qui levelsof id if id <= 4, local(o_lvnum)

* sentinel dataset that must survive every exploration call
clear
set obs 2
gen sentinel = _n
datasignature set, reset

* ---------- the same through the lazy view ----------
parqit use using `"`fix'"', name(x)

* misstable
parqit misstable
assert r(N) == 1000
parqit misstable y z
assert r(N) == 1000

* summarize, detail — every r() against native to tight tolerance
parqit summarize y, detail
assert r(N) == `o_n'
assert reldif(r(mean), `o_mean') < 1e-12
assert reldif(r(sd), `o_sd') < 1e-12
assert reldif(r(Var), `o_var') < 1e-12
assert reldif(r(skewness), `o_skew') < 1e-10
assert reldif(r(kurtosis), `o_kurt') < 1e-10
assert reldif(r(p1),  `o_p1')  < 1e-12
assert reldif(r(p25), `o_p25') < 1e-12
assert reldif(r(p50), `o_p50') < 1e-12
assert reldif(r(p75), `o_p75') < 1e-12
assert reldif(r(p99), `o_p99') < 1e-12

* two-way tabulate
parqit tabulate g h
assert r(N) == 1000
assert r(r) == 3
assert r(c) == 4

* levelsof: strings (quoted) and numerics (plain), with a filtered view
parqit levelsof g
assert `"`r(levels)'"' == `"`o_lv'"'
assert r(r) == 3
parqit keep if id <= 4
parqit levelsof id
assert "`r(levels)'" == "`o_lvnum'"
local lvls `"`r(levels)'"'
foreach v of local lvls {
    assert `v' >= 1 & `v' <= 4
}

* limit() is loud
parqit close
parqit use using `"`fix'"', name(x)
capture parqit levelsof id, limit(10)
assert _rc != 0

* errors are loud
capture parqit summarize g, detail
assert _rc != 0
capture parqit tabulate g h y
assert _rc != 0
capture parqit misstable nope
assert _rc != 0

* memory untouched by everything above
datasignature confirm
assert sentinel[2] == 2
parqit close _all

di "VERDICT(T08_EXPLORE): PASS - misstable/detail/tab2/levelsof match native; pushdown only; memory untouched"
