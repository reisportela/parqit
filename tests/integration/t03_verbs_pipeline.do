* M2: the lazy-view grammar end-to-end in Stata — open, verbs, show, count,
* collect, save — checked against native Stata doing the same pipeline in
* memory (the strongest oracle for verb semantics).
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

* ---------- fixture: worker-year panel written by parqit ----------
set obs 200
set seed 42
gen long id    = ceil(_n / 4)
gen int  year  = 2016 + mod(_n - 1, 4)
gen double wage = exp(rnormal(2, 0.5)) * 10
replace wage = . in 7
replace wage = . in 100
gen str8 firm  = "f" + string(mod(id, 7))
gen long d     = td(01jan2018) + _n
format d %td
label variable wage "hourly wage"

tempfile fbase
local panel `"`fbase'.parquet"'
parqit save `"`panel'"', replace

* native-Stata oracle of the pipeline result
preserve
keep if wage > 50 & !missing(wage)
gen double lwage = ln(wage)
collapse (mean) mw = lwage (count) n = lwage (p25) q1 = lwage, by(firm)
sort firm
tempfile oracle
qui save `"`oracle'"'
restore

* ---------- the same pipeline through the lazy view ----------
clear
parqit use using `"`panel'"'
assert r(k) == 5

* verbs are lazy: nothing in memory yet
assert _N == 0 & c(k) == 0

parqit keep if wage > 50 & !missing(wage)
parqit gen double lwage = ln(wage)
parqit count
local nkept = r(N)
parqit collapse (mean) mw=lwage (count) n=lwage (p25) q1=lwage, by(firm)
parqit sort firm
parqit show
parqit describe
assert r(n_cols) == 4

parqit collect, clear
assert c(k) == 4

* compare against the native oracle, cell by cell
rename mw mw_s
rename n n_s
rename q1 q1_s
qui merge 1:1 firm using `"`oracle'"', assert(match) nogenerate
assert _N == 7
gen double dmw = reldif(mw_s, mw)
gen double dq1 = reldif(q1_s, q1)
assert n_s == n
summ dmw, meanonly
assert r(max) < 1e-12
summ dq1, meanonly
assert r(max) < 1e-12

* ---------- pipeline → parquet without touching memory ----------
clear
parqit use using `"`panel'"'
parqit keep if year >= 2018
parqit gen byte recent = 1
parqit keep id year wage recent
tempfile obase
local out2 `"`obase'.parquet"'
set obs 3
gen marker = _n          // memory content that must survive parqit save
parqit save `"`out2'"', replace
assert _N == 3 & marker[3] == 3   // untouched ✓
parqit close

parqit use using `"`out2'"', clear
assert c(k) == 4
qui count if recent != 1
assert r(N) == 0
qui count if year < 2018
assert r(N) == 0

* ---------- more verbs: rename/order/keep in/duplicates/egen ----------
clear
parqit use using `"`panel'"'
parqit rename wage w
parqit egen tw = total(w), by(firm)
parqit sort id year
parqit keep in 5/24
parqit count
assert r(N) == 20
parqit duplicates drop id, force
parqit count
assert r(N) == 5
parqit collect, clear
assert _N == 5
confirm variable tw

* keep in beyond the data is loud (charter 6.13)
clear
parqit use using `"`panel'"'
parqit keep in 100/9999
capture parqit count
assert _rc != 0
capture parqit collect, clear
assert _rc != 0
parqit close

* ---------- statamissing mode ----------
clear
parqit use using `"`panel'"'
parqit set statamissing on
parqit keep if wage > 50      // now missings count as > 50, like native Stata
parqit count
local n_stm = r(N)
parqit set statamissing off
parqit close

clear
parqit use using `"`panel'"'
parqit keep if wage > 50
parqit count
local n_sql = r(N)
parqit close
assert `n_stm' == `n_sql' + 2   // exactly the two missing wages

* ---------- parqit open _data ----------
clear
set obs 10
gen x = _n
gen g = mod(_n, 2)
parqit open _data
parqit collapse (sum) sx = x, by(g)
parqit collect, clear
assert _N == 2
qui summ sx
assert r(sum) == 55

di "VERDICT(T03_VERBS): PASS - lazy pipeline matches native Stata; save bypasses memory; modes work"
