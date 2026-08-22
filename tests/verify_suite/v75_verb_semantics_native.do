* V75 — lazy verb / expression semantics against native Stata (audit 2026-08-22, A3):
*   A3-2 real() follows Stata's literal grammar (no digit-group underscores; d exponent);
*   A3-3/A3-4 date functions are missing outside 01jan0100..31dec9999 and never abort;
*   A3-5 levelsof of a numeric variable named v (or n, cnt, value, total, freq, pat, b, g)
*        is in numeric order; tabulate/summarize/misstable/codebook unaffected by such names;
*   A3-6 levelsof / tabulate render non-integers like native (r(levels) text, tab labels);
*   A3-7 string() of a denormal double;
*   A3-8 collapse / merge / reshape wide result metadata follows native;
*   A3-9 keep in f/l accepts f, l and negative bounds;
*   A2-6 append gen() clashing only by case with a using column is refused;
*   A2-15.4 contract with an existing _freq is refused like native r(110);
*   A5-12 parqit sql reports a case-distinct alias as a note.
* Native Stata on the same data is the oracle throughout.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

local fails 0
tempfile stem

* ---------- A3-2 real(): every probed literal, native vs lazy ----------------
clear
input str12 s
"2019_01"
"12_345_678"
"1_000.5"
"1e1_0"
"1d3"
"1D3"
"1d-3"
"1.5d2"
" 2 "
"+3"
".5"
"5."
"12.e3"
"1.e3"
"1e"
"0x1A"
"1,5"
"inf"
"nan"
"1e400"
"1,000"
"$5"
"--1"
"1.5.5"
".e3"
"e3"
"+"
"."
"-.5"
"1e+3"
"  "
""
"1 2"
"1.5e-3"
"0.0"
"-0"
"1E5"
end
gen double native = real(s)
gen long i = _n
parqit save `"`stem'_real.parquet"', replace data
parqit use using `"`stem'_real.parquet"'
parqit gen double lazy = real(s)
parqit collect, clear
sort i
capture assert lazy == native
if (_rc) {
    list s native lazy if lazy != native, noobs
    di as err "FAIL: real() differs from native"
    local ++fails
}
parqit close _all

* ---------- A3-3 / A3-4 date domain ----------------------------------------
clear
input double d double m double yy double mm double dd
2936549 96479 9999 12 31
3000001 96480 10000 1 1
-679350 -22320 100 1 1
-679351 -22321 99 1 1
2147483647 123456.79 2020 2 30
3000000 -1234567 -1 6 15
21915 720 2020 1 1
1e10 0 0 1 1
end
gen long i = _n
gen double ny = year(d)
gen double nm = month(d)
gen double nd = day(d)
gen double nq = quarter(d)
gen double ndow = dow(d)
gen double ndoy = doy(d)
gen double nmofd = mofd(d)
gen double nyofd = yofd(d)
gen double ndofm = dofm(m)
gen double nmdy = mdy(mm, dd, yy)
parqit save `"`stem'_dates.parquet"', replace data
parqit use using `"`stem'_dates.parquet"'
parqit gen double ly = year(d)
parqit gen double lm = month(d)
parqit gen double ld = day(d)
parqit gen double lq = quarter(d)
parqit gen double ldow = dow(d)
parqit gen double ldoy = doy(d)
parqit gen double lmofd = mofd(d)
parqit gen double lyofd = yofd(d)
parqit gen double ldofm = dofm(m)
parqit gen double lmdy = mdy(mm, dd, yy)
capture noisily parqit collect, clear
if (_rc) {
    di as err "FAIL: date functions aborted the collect (rc `=_rc')"
    local ++fails
}
else {
    sort i
    foreach p in y m d q dow doy mofd yofd dofm mdy {
        capture assert l`p' == n`p'
        if (_rc) {
            list i n`p' l`p' if l`p' != n`p', noobs
            di as err "FAIL: `p'() differs from native"
            local ++fails
        }
    }
}
parqit close _all

* ---------- A3-5 / A3-6 levelsof, tabulate with alias-shaped names ----------
clear
input double v double n double cnt double value double total double freq double pat double b double g str3 s
0.1 1 1 1 1 1 1 1 1 "a"
0.5 2 2 2 2 2 2 2 2 "b"
1.5 3 3 3 3 3 3 3 3 "c"
2 10 10 10 10 10 10 10 10 "a"
1e-7 11 11 11 11 11 11 11 11 "b"
123456789.123 20 20 20 20 20 20 20 20 "c"
0.3 -1 -1 -1 -1 -1 -1 -1 -1 "a"
-0.25 0 0 0 0 0 0 0 0 ""
1e15 1e15 1e15 1e15 1e15 1e15 1e15 1e15 1e15 "z"
. . . . . . . . . "a"
end
gen long i = _n
format v %9.0g
format n %12.0g
tempfile lv
qui save `"`lv'.dta"'
parqit save `"`lv'.parquet"', replace data
foreach x in v n cnt value total freq pat b g {
    use `"`lv'.dta"', clear
    qui levelsof `x'
    local native `"`r(levels)'"'
    local nr = r(r)
    parqit use using `"`lv'.parquet"'
    parqit levelsof `x'
    if (`"`r(levels)'"' != `"`native'"' | r(r) != `nr') {
        di as err `"FAIL levelsof `x': parqit [`r(levels)'] native [`native']"'
        local ++fails
    }
    parqit close _all
}
use `"`lv'.dta"', clear
qui levelsof s
local native `"`r(levels)'"'
parqit use using `"`lv'.parquet"'
parqit levelsof s
if (`"`r(levels)'"' != `"`native'"') {
    di as err `"FAIL levelsof s: [`r(levels)'] vs [`native']"'
    local ++fails
}
* the read-only stats with alias-shaped names still run and agree with native
qui sum v
local nmean = r(mean)
parqit summarize v
assert reldif(r(mean), `nmean') < 1e-12
parqit tabulate n
assert r(r) == 9
parqit tabulate b g
parqit misstable summarize v n
assert r(n_complete) == 9
parqit codebook v n cnt value total freq pat b g
parqit histogram v, nodraw
parqit misstable patterns v n
parqit tabstat v n, statistics(mean sd) by(g)
parqit distinct v n
parqit duplicates report v
parqit close _all
* tabulate renders numeric levels with the variable's format, like native
* (%9.0g: .3, 1.00e-07; %12.0g keeps more digits)
parqit use using `"`lv'.parquet"'
capture erase v75tab.log
log using v75tab.log, text replace name(v75tab)
parqit tabulate v
parqit tabulate n
log close v75tab
parqit close _all
python:
from sfi import Macro
txt = open("v75tab.log", encoding="utf-8", errors="replace").read().replace("\n> ", "")
ok = (" .3 " in txt or " .3\n" in txt or ".3 " in txt) and "1.00e-07" in txt and "0.30000000000000004" not in txt
Macro.setLocal("oktab", "1" if ok else "0")
end
if ("`oktab'" != "1") {
    di as err "FAIL: tabulate rendering differs from native (%9.0g)"
    local ++fails
}

* ---------- A3-7 string() of denormals -------------------------------------
clear
input double x
5e-324
1e-323
1e-310
1e-308
2.2250738585072014e-308
1e300
0
1.5
-2.5e-7
end
gen str20 native = string(x)
gen long i = _n
parqit save `"`stem'_str.parquet"', replace data
parqit use using `"`stem'_str.parquet"'
parqit gen str20 lazy = string(x)
parqit collect, clear
sort i
capture assert lazy == native
if (_rc) {
    list x native lazy if lazy != native, noobs
    di as err "FAIL: string() differs from native"
    local ++fails
}
parqit close _all

* ---------- A3-8 collapse / merge / reshape metadata parity ------------------
clear
input byte b double v str2 hs
1 1.5 "a"
1 2.5 "b"
2 3.5 "c"
end
label var v "value"
label var b "bee"
format v %9.2f
format b %3.0f
gen double c = b * 2
label var c "cee"
format c %12.3f
tempfile cbase
qui save `"`cbase'.dta"'
parqit save `"`cbase'.parquet"', replace data
collapse (mean) v (sum) s=v (count) n=v (max) mc=c (first) hs, by(b)
local nsig
foreach x in b v s n mc hs {
    local nsig `"`nsig' `x'|`: type `x''|`: format `x''|`: var label `x''"'
}
parqit use using `"`cbase'.parquet"'
parqit collapse (mean) v (sum) s=v (count) n=v (max) mc=c (first) hs, by(b)
parqit collect, clear
local psig
foreach x in b v s n mc hs {
    local psig `"`psig' `x'|`: type `x''|`: format `x''|`: var label `x''"'
}
if (`"`psig'"' != `"`nsig'"') {
    di as err `"FAIL collapse metadata: parqit [`psig'] native [`nsig']"'
    local ++fails
}
parqit close _all
* _merge: byte %23.0g, label, value label
clear
input double k double a
1 1
2 2
end
tempfile mu
qui save `"`mu'.dta"'
parqit save `"`mu'.parquet"', replace data
clear
input double k double c
1 5
3 7
end
tempfile mm
qui save `"`mm'.dta"'
parqit save `"`mm'.parquet"', replace data
qui merge 1:1 k using `"`mu'.dta"'
local nm "`: type _merge'|`: format _merge'|`: var label _merge'|`: value label _merge'"
parqit use using `"`mm'.parquet"'
parqit merge 1:1 k using `"`mu'.parquet"'
parqit collect, clear
local pm "`: type _merge'|`: format _merge'|`: var label _merge'|`: value label _merge'"
if ("`pm'" != "`nm'") {
    di as err "FAIL _merge metadata: parqit [`pm'] native [`nm']"
    local ++fails
}
parqit close _all
* reshape wide: label "<j> <stub>", format kept
clear
input double id double year double inc
1 1 10
1 2 20
2 1 30
2 2 40
end
label var inc "income"
format inc %9.2f
tempfile rw
qui save `"`rw'.dta"'
parqit save `"`rw'.parquet"', replace data
reshape wide inc, i(id) j(year)
local nr "`: var label inc1'|`: format inc1'|`: var label inc2'"
parqit use using `"`rw'.parquet"'
parqit reshape wide inc, i(id) j(year)
parqit collect, clear
local pr "`: var label inc1'|`: format inc1'|`: var label inc2'"
if ("`pr'" != "`nr'") {
    di as err "FAIL reshape wide metadata: parqit [`pr'] native [`nr']"
    local ++fails
}
parqit close _all

* ---------- A3-9 keep in f/l letters and negatives ---------------------------
clear
set obs 60
gen long i = _n
tempfile kin
parqit save `"`kin'.parquet"', replace data
parqit use using `"`kin'.parquet"'
parqit sort i
parqit keep in 50/l
parqit collect, clear
assert _N == 11 & i[1] == 50 & i[11] == 60
parqit use using `"`kin'.parquet"'
parqit sort i
parqit keep in f/5
parqit collect, clear
assert _N == 5 & i[5] == 5
parqit use using `"`kin'.parquet"'
parqit sort i
parqit keep in -3/l
parqit collect, clear
assert _N == 3 & i[1] == 58
parqit use using `"`kin'.parquet"'
parqit sort i
parqit keep in -10/-5
parqit collect, clear
assert _N == 6 & i[1] == 51 & i[6] == 56
parqit use using `"`kin'.parquet"'
parqit sort i
parqit keep in l
parqit collect, clear
assert _N == 1 & i[1] == 60
parqit use using `"`kin'.parquet"'
capture noisily parqit keep in 10/f
assert _rc == 198
capture noisily parqit keep in x/l
assert _rc == 198
parqit close _all

* ---------- A2-6 append gen() case clash; A2-15.4 contract _freq -------------
clear
set obs 2
gen long id = _n
gen double val = _n
tempfile ap
parqit save `"`ap'.parquet"', replace data
clear
set obs 2
gen long id = _n + 10
parqit save `"`ap'_m.parquet"', replace data
parqit use using `"`ap'_m.parquet"'
capture noisily parqit append using `"`ap'.parquet"', generate(Val)
assert _rc == 198
parqit append using `"`ap'.parquet"', generate(src)
parqit collect, clear
assert _N == 4
parqit close _all
clear
set obs 4
gen byte grp = mod(_n, 2)
gen long _freq = _n
tempfile ct
parqit save `"`ct'.parquet"', replace data
parqit use using `"`ct'.parquet"'
capture noisily parqit contract grp
assert _rc == 198
parqit contract grp, freq(nn)
parqit collect, clear
assert _N == 2 & nn[1] == 2
parqit close _all

* ---------- A5-12 parqit sql alias note channel ------------------------------
capture erase v75sql.log
log using v75sql.log, text replace name(v75sql)
parqit sql "SELECT 1 AS a, 2 AS A"
log close v75sql
parqit close _all
python:
from sfi import Macro
txt = open("v75sql.log", encoding="utf-8", errors="replace").read().replace("\n> ", "")
Macro.setLocal("oksql", "1" if ("note: column \"A\" differs only by case" in txt and "warning: column \"A\"" not in txt) else "0")
end
if ("`oksql'" != "1") {
    di as err "FAIL: parqit sql alias message is not a note"
    local ++fails
}

if (`fails') {
    di as err "VERDICT(V75_VERB_SEMANTICS_NATIVE): FAIL - `fails' check(s) differ from native"
    exit 9
}
di "VERDICT(V75_VERB_SEMANTICS_NATIVE): PASS - real() grammar, date domain, levelsof/tabulate ordering and rendering with alias-shaped names, string() denormals, collapse/merge/reshape metadata, keep in f/l, append gen clash, contract _freq, sql note — all match native"
