* V148 — BPLIM feedback, third round (2026-10-13): the by and bysort prefixes
*   (BY-PREFIX-1) on gen, replace, egen, keep if and drop if; _n and _N as
*   windows within the groups; the explicit subscripts x[_n-k], x[_n+k], x[k],
*   x[_N], x[_N-k] (ROWCTX-2, positional); and the time-series declaration
*   with the operators L. F. D. S. by TIME (TSSET-1: a gap gives missing, as
*   native, where a positional lag would return the wrong period), tin() and
*   twithin(). Refusals: by without the sort, a replace that reads its own
*   earlier rows (sequential in native Stata), tsset with a noninteger or
*   repeated time, a period below delta or a noninteger delta, the operators
*   without a declaration; a missing time is accepted as native accepts it.
* Oracles: native Stata — bysort/by with gen, replace, egen, keep if; xtset
*   with L. L2. F. D. D2. S2. LD., tin(), twithin().
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
tempfile stem nat
parqit set statamissing off

sysuse auto, clear
gen int id = _n
parqit save `"`stem'_auto.parquet"', data replace

* ---------- by / bysort with subscripts, replace, egen, keep if -------------
sysuse auto, clear
keep in 1/12
gen int id = _n
bysort foreign (price): gen nobs = _n
bysort foreign (price): gen ngrp = _N
bysort foreign (price): gen lagp = price[_n-1]
bysort foreign (price): gen leadp = price[_n+1]
bysort foreign (price): gen firstp = price[1]
bysort foreign (price): gen lastp = price[_N]
bysort foreign (price): gen lag2 = price[_n-2]
bysort foreign (price): gen p3 = price[3]
bysort foreign (price): gen pNm1 = price[_N-1]
bysort foreign (price): gen out = price[0]
bysort foreign (price): gen out2 = price[_N+1]
bysort foreign (price): replace mpg = mpg + 1000 if _n == 1
by foreign: egen tp = total(price)
bysort foreign (price): keep if _n <= 11
recast double nobs ngrp lagp leadp firstp lastp lag2 p3 pNm1 out out2 tp, force
save `"`nat'"', replace
parqit use using `"`stem'_auto.parquet"'
parqit keep if id <= 12
parqit bysort foreign (price): gen nobs = _n
parqit bysort foreign (price): gen ngrp = _N
parqit bys foreign (price): gen lagp = price[_n-1]
parqit bysort foreign (price): gen leadp = price[_n+1]
parqit bysort foreign (price): gen firstp = price[1]
parqit bysort foreign (price): gen lastp = price[_N]
parqit bysort foreign (price): gen lag2 = price[_n-2]
parqit bysort foreign (price): gen p3 = price[3]
parqit bysort foreign (price): gen pNm1 = price[_N-1]
parqit bysort foreign (price): gen out = price[0]
parqit bysort foreign (price): gen out2 = price[_N+1]
parqit bysort foreign (price): replace mpg = mpg + 1000 if _n == 1
parqit by foreign: egen tp = total(price)
parqit bysort foreign (price): keep if _n <= 11
parqit collect, clear
sort foreign price
cf _all using `"`nat'"', verbose
* the bysort left the view sorted, as native leaves the data
assert "`: sortedby'" == "foreign price"
* by without the sort is refused, as native r(5) "not sorted"
parqit sort id
capture noisily parqit by foreign: gen z = _n
assert _rc == 198
parqit sort foreign
capture noisily parqit by foreign (price): gen z = _n
assert _rc == 198
parqit sort foreign price
parqit by foreign (price): gen z = _n
* a replace that reads its own earlier rows is sequential in native Stata:
* refused, the view unchanged
parqit describe
local k = r(n_cols)
capture noisily parqit bysort foreign (price): replace mpg = mpg[_n-1] if missing(mpg)
assert _rc == 198
capture noisily parqit replace price = price[1]
assert _rc == 198
parqit describe
assert r(n_cols) == `k'
* the prefix applies to gen, replace, egen, keep if and drop if only
capture noisily parqit bysort foreign: sort price
assert _rc == 198
capture noisily parqit by foreign: keep price
assert _rc == 198
parqit close _all

* ---------- drop if under by, _n in the if of gen and in replace ------------
sysuse auto, clear
sort foreign price
by foreign: drop if _n > 3
by foreign: gen byte last = _n == _N
by foreign: replace price = price * 2 if _n == _N
recast double last, force
keep foreign price last
save `"`nat'"', replace
parqit use foreign price using `"`stem'_auto.parquet"'
parqit bysort foreign (price): drop if _n > 3
parqit by foreign: gen byte last = _n == _N
parqit by foreign: replace price = price * 2 if _n == _N
parqit collect, clear
sort foreign price
cf _all using `"`nat'"', verbose
parqit close _all

* ---------- xtset and the time-series operators ---------------------------
clear
input int id int year double x
1 2000 10
1 2001 11
1 2003 13
1 2004 14
2 2000 20
2 2002 22
2 2003 23
end
tempfile panel
parqit save `"`panel'.parquet"', data replace
xtset id year
gen lx = L.x
gen l2x = L2.x
gen fx = F.x
gen dx = D.x
gen d2x = D2.x
gen s2x = S2.x
gen ldx = LD.x
gen f2x = F2.x
gen tinx = tin(2001, 2003)
gen twx = twithin(2001, 2003)
recast double lx l2x fx dx d2x s2x ldx f2x tinx twx, force
save `"`nat'"', replace
parqit use using `"`panel'.parquet"'
* the operators need the declaration
capture noisily parqit gen lx = L.x
assert _rc == 198
parqit xtset id year
assert "`r(panelvar)'" == "id" & "`r(timevar)'" == "year" & r(delta) == 1 & r(gaps) == 1
parqit gen lx = L.x
parqit gen l2x = L2.x
parqit gen fx = F.x
parqit gen dx = D.x
parqit gen d2x = D2.x
parqit gen s2x = S2.x
parqit gen ldx = LD.x
parqit gen f2x = F2.x
parqit gen tinx = tin(2001, 2003)
parqit gen twx = twithin(2001, 2003)
parqit collect, clear
* the view declares no sort, so the collected order is the engine's after
* the windows; native xtset sorted by (id, year)
sort id year
cf _all using `"`nat'"', verbose
* the declaration is shown and survives a rename, and L. of the time variable
* itself is refused
parqit tsset
assert "`r(timevar)'" == "year"
parqit rename year yr
parqit tsset
assert "`r(timevar)'" == "yr"
capture noisily parqit gen bad = L.yr
assert _rc == 198
parqit close _all
* delta: every second period. Native refuses a period below delta (r(451)
* "time values with period less than delta() found"): the yearly panel has
* 2000-2001; a panel on even years declares, with a gap at 2004
parqit use using `"`panel'.parquet"'
capture noisily parqit xtset id year, delta(2)
assert _rc == 198
parqit close _all
clear
input int id int year double x
1 2000 10
1 2002 11
1 2006 13
2 2000 20
2 2004 22
end
tempfile even
parqit save `"`even'.parquet"', data replace
xtset id year, delta(2)
gen lx = L.x
gen fx = F.x
recast double lx fx, force
save `"`nat'"', replace
parqit use using `"`even'.parquet"'
parqit xtset id year, delta(2)
assert r(gaps) == 1
parqit gen lx = L.x
parqit gen fx = F.x
parqit collect, clear
sort id year
cf _all using `"`nat'"', verbose
assert lx[2] == 10 & missing(lx[3]) & missing(lx[5])
parqit close _all
* refusals: a noninteger delta, a repeated time; a missing time is accepted,
* as native accepts it, and the operators are missing on that row
capture noisily parqit xtset id year, delta(0.5)
assert _rc == 198
clear
input int id double t double x
1 1 10
1 1 11
2 . 20
end
tempfile bad
parqit save `"`bad'.parquet"', data replace
parqit use using `"`bad'.parquet"'
capture noisily parqit xtset id t
assert _rc == 198
parqit keep if id == 2
parqit tsset t
assert _rc == 0
parqit close _all
clear
input int id double t double x
1 1 10
1 2 11
1 . 12
1 3 13
end
tempfile miss
parqit save `"`miss'.parquet"', data replace
xtset id t
gen lx = L.x
gen dx = D.x
gen tinx = tin(1, 2)
recast double lx dx tinx, force
save `"`nat'"', replace
parqit use using `"`miss'.parquet"'
parqit xtset id t
parqit gen lx = L.x
parqit gen dx = D.x
parqit gen tinx = tin(1, 2)
parqit collect, clear
sort id t
cf _all using `"`nat'"', verbose
assert missing(lx[4]) & tinx[4] == 0
parqit close _all
* a fractional time: native "time variable must contain only integer values"
clear
input int id double t double x
1 1 10
1 1.5 11
1 2 12
end
tempfile frac
parqit save `"`frac'.parquet"', data replace
parqit use using `"`frac'.parquet"'
capture noisily parqit xtset id t
assert _rc == 198
capture noisily parqit tsset t
assert _rc == 198
parqit close _all
* tin() with the format of the time variable
clear
input int id double d double x
1 21915 1
1 21916 2
1 21917 3
end
format d %td
tempfile dd
parqit save `"`dd'.parquet"', data replace
xtset id d
gen in1 = tin(02jan2020, 03jan2020)
gen in2 = twithin(01jan2020, 03jan2020)
recast double in1 in2, force
save `"`nat'"', replace
parqit use using `"`dd'.parquet"'
parqit xtset id d
parqit gen in1 = tin(02jan2020, 03jan2020)
parqit gen in2 = twithin(01jan2020, 03jan2020)
parqit collect, clear
sort id d
cf _all using `"`nat'"', verbose
parqit close _all

di "VERDICT(V148_BY_PREFIX_SUBSCRIPTS_TSSET): PASS - by/bysort on gen, replace, egen, keep if and drop if equal native, with _n, _N and the positional subscripts; by without the sort, a self-reading replace and the prefix on other verbs are refused; xtset/tsset declare, check and show the panel and time (a period below delta refused as native, a missing time accepted as native), and L. F. D. S. (chains too), tin() and twithin() equal native by time, gaps and missing times giving missing"
