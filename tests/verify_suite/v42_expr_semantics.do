* V42 — INF-1/DATE-2/SUBINSTR-NULL-1: expression semantics adjudicated against
* native Stata (2026-07-02). A generated IEEE Inf must behave as MISSING
* everywhere (exp(710)=. natively; unguarded DuckDB gave +Inf which passed
* `< .`, inverted filters and poisoned collapse aggregates); fractional day
* counts FLOOR (day(-0.5)=31 natively); out-of-range date args are row-local
* missing, never a query abort; subinstr with an empty needle returns the
* string unchanged; malformed syntax that native Stata rejects (uppercase
* extended missings, 1.2.3, ||, &&, string args to numeric fns, mixed cond
* branches) is rejected loudly, never silently mistranslated.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

local fails 0
tempfile f

* fixture
clear
set obs 3
gen long id = _n
gen double x = cond(_n == 1, 800, _n)   /* x = 800, 2, 3 */
gen str4 s = "abc"
parqit save `"`f'.parquet"', replace data

* native oracle for the collapse
preserve
gen double z = exp(x)                    /* row 1: . (native overflow->missing) */
quietly summarize z
local nmean = r(mean)
local nN = r(N)
restore

* ---- CASE 1 : exp overflow is missing; aggregates match native --------------
parqit use using `"`f'.parquet"'
parqit gen z = exp(x)
parqit collect, clear
capture assert missing(z) in 1
if (_rc) di as err "FAIL c1a: exp(800) not missing after collect"
local fails = `fails' + (_rc!=0)
quietly summarize z
capture assert abs(r(mean) - `nmean') < 1e-6 & r(N) == `nN'
if (_rc) di as err "FAIL c1b: mean/N of exp() column differ from native (Inf poisoning)"
local fails = `fails' + (_rc!=0)
parqit close

* ---- CASE 2 : Inf must not pass `< .` filters --------------------------------
parqit use using `"`f'.parquet"'
parqit gen z = exp(x)
parqit keep if z < .
parqit collect, clear
capture assert _N == 2 & !missing(z)
if (_rc) di as err "FAIL c2: exp(800) survived `"z < ."' (native drops it)"
local fails = `fails' + (_rc!=0)
parqit close

* ---- CASE 3 : collapse over the generated column matches native --------------
parqit use using `"`f'.parquet"'
parqit gen z = exp(x)
parqit collapse (mean) mz=z (max) xz=z (sum) sz=z
parqit collect, clear
capture assert abs(mz - `nmean') < 1e-6 & !missing(xz) & !missing(sz)
if (_rc) di as err "FAIL c3: collapse stats poisoned by Inf"
local fails = `fails' + (_rc!=0)
parqit close

* ---- CASE 4 : overflowing literals and arithmetic are missing ---------------
parqit use using `"`f'.parquet"'
parqit gen big = 1e300 * 1e300
parqit gen lit = 1e309
parqit collect, clear
capture assert missing(big) & missing(lit)
if (_rc) di as err "FAIL c4: 1e300*1e300 / 1e309 not missing"
local fails = `fails' + (_rc!=0)
parqit close

* ---- CASE 5 : date functions floor and never abort ---------------------------
parqit use using `"`f'.parquet"'
parqit gen d1 = day(-0.5)
parqit gen d2 = day(21915.9)
parqit gen y1 = year(3e9)
parqit collect, clear
capture assert d1 == 31 & d2 == 1 & missing(y1)
if (_rc) di as err "FAIL c5: date floor/out-of-range semantics wrong (d1=`=d1[1]' d2=`=d2[1]')"
local fails = `fails' + (_rc!=0)
parqit close

* ---- CASE 6 : subinstr empty needle returns the string unchanged -------------
parqit use using `"`f'.parquet"'
parqit gen s2 = subinstr(s, "", "X", .)
parqit collect, clear
capture assert s2 == "abc"
if (_rc) di as err "FAIL c6: subinstr empty-needle wiped the value"
local fails = `fails' + (_rc!=0)
parqit close

* ---- CASE 7 : loud rejections (native r(198)/r(109) equivalents) -------------
parqit use using `"`f'.parquet"'
foreach bad in "x == .A" "x < 1.2.3" "x == 1 || x == 2" "x == 1 && x == 2" ///
               "dow(s)" "mod(s, 2)" `"cond(x, "a", "b", 9)"' {
    capture parqit keep if `bad'
    if (_rc == 0) {
        di as err `"FAIL c7: expression [`bad'] accepted (native rejects)"'
        local fails = `fails' + 1
    }
}
* mod(7,0) is missing natively (manual claim mod(x,0)=x is stale)
parqit gen m0 = mod(7, 0)
parqit collect, clear
capture assert missing(m0)
if (_rc) di as err "FAIL c7b: mod(7,0) not missing"
local fails = `fails' + (_rc!=0)
parqit close

if (`fails' == 0) di as res "VERDICT(V42_EXPR_SEMANTICS): PASS - Inf==missing everywhere; dates floor; loud rejections match native"
else di as err "VERDICT(V42_EXPR_SEMANTICS): FAIL - `fails' case(s)"
