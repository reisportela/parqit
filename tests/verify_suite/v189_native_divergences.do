* V189 — three divergences from native Stata, closed on 25 Oct 2026 (found by the
* features tour of 24 Oct). CSV-USING-1: a CSV file as the using side of
* merge, append and joinby is read as parqit use reads it — names as written,
* decimals in double, dates as dates — where it was read by import delimited
* (lowercased names, decimals in float, dates as text); oracles: the CSV's text
* parsed by Python, and parqit use of the same file. GROUP-TRUNCATE-1: egen
* group(), label truncate(#) gives native Stata's value-label texts (a string or
* a value label cut to # display columns and trimmed; a number without a label
* whole). SEQ-TO-1: egen seq() without to() runs to _N and starts again, and
* runs down when from() is above _N, as native egen does, under if, by() and
* block(). Needs pyarrow importable from Stata's Python.
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
parqit set threads 4
local fails 0
tempfile stem nat

* ---------------------------------------------------------------- CSV-USING-1
python:
from sfi import Macro
stem = Macro.getLocal("stem")
open(stem + "_lookup.csv", "w", encoding="utf-8").write(
    "ID,Rate,When,Name\n1,0.123456789012345,2024-01-15,Ana\n2,1.5,2024-02-29,Rui\n3,-2.0000000000000004,2023-12-31,Zé\n")
end
clear
set obs 4
gen long ID = _n
gen double Base = _n / 4
parqit save `"`stem'_main.parquet"', data replace
parqit use using `"`stem'_lookup.csv"', clear
quietly save `"`nat'"', replace

parqit use using `"`stem'_main.parquet"'
parqit merge 1:1 ID using `"`stem'_lookup.csv"', keep(match) nogenerate
parqit collect, clear
sort ID
* (python blocks stay at top level — the batch parser breaks on them in a block)
python:
from sfi import Data, Scalar
try:
    ok = (Data.getVarType("Rate") == "double" and Data.getVarFormat("When") == "%td"
          and list(Data.get("Rate")) == [0.123456789012345, 1.5, -2.0000000000000004])
except Exception as e:
    print("no Rate:", e); ok = False
Scalar.setValue("exact", int(ok))
end
capture confirm variable When Name, exact
local rc_names = _rc
capture cf ID Rate When Name using `"`nat'"'
local rc_cf = _rc
if (`rc_names' | `rc_cf' | scalar(exact) != 1) {
    di as err "FAIL: merge: the CSV's names, decimals in double or dates were not kept (rc `rc_names', `rc_cf')"
    local ++fails
}

parqit use using `"`stem'_main.parquet"'
parqit joinby ID using `"`stem'_lookup.csv"'
parqit collect, clear
sort ID
capture cf ID Rate When Name using `"`nat'"'
if (_rc | _N != 3) {
    di as err "FAIL: joinby: the CSV's columns differ from parqit use of the file"
    local ++fails
}

use `"`nat'"', clear
parqit save `"`stem'_lookup.parquet"', data replace
parqit use using `"`stem'_lookup.parquet"'
parqit append using `"`stem'_lookup.csv"'
parqit collect, clear
if (_N != 6) {
    di as err "FAIL: append: not 6 rows"
    local ++fails
}
else {
    quietly keep in 4/6
    capture cf ID Rate When Name using `"`nat'"'
    if (_rc) {
        di as err "FAIL: append: the CSV's rows differ from parqit use of the file"
        local ++fails
    }
}

* ----------------------------------------------------------- GROUP-TRUNCATE-1
clear
input str20 s byte lab int num
"中文字" 1 15
"ＡＢＣ" 2 150
"ab cd" 1 7
"x  yz" 3 15
"  lead" 2 150
"abc" 1 7
"é é é" 3 1500
end
label define labs 1 "one  label" 2 "  two" 3 "三三三"
label values lab labs
quietly replace lab = 9 in 7
quietly save `"`stem'_g.dta"', replace
egen g = group(s lab num), label truncate(3)
decode g, gen(gtxt)
keep s lab num g gtxt
sort s lab num
quietly save `"`nat'"', replace
use `"`stem'_g.dta"', clear
parqit save `"`stem'_g.parquet"', data replace
parqit use using `"`stem'_g.parquet"'
parqit egen g = group(s lab num), label truncate(3)
parqit collect, clear
decode g, gen(gtxt)
keep s lab num g gtxt
sort s lab num
capture cf _all using `"`nat'"'
if (_rc) {
    di as err "FAIL: egen group(), label truncate(3): codes or texts differ from native Stata"
    list s lab num g gtxt, noobs
    local ++fails
}

* ------------------------------------------------------------------ SEQ-TO-1
clear
set obs 10
gen byte grp = 1 + (_n > 6)
gen double x = cond(mod(_n, 3) == 0, -1, _n)
gen long id = _n
quietly save `"`stem'_s.dta"', replace
egen s1 = seq(), from(3)
egen s2 = seq(), from(12)
egen s3 = seq() if x > 0, from(2) block(2) by(grp)
egen s4 = seq(), from(10)
* native egen ... by() leaves the data sorted by its groups: compare by row
sort id
quietly save `"`nat'"', replace
use `"`stem'_s.dta"', clear
parqit save `"`stem'_s.parquet"', data replace
parqit use using `"`stem'_s.parquet"'
parqit egen s1 = seq(), from(3)
parqit egen s2 = seq(), from(12)
parqit egen s3 = seq() if x > 0, from(2) block(2) by(grp)
parqit egen s4 = seq(), from(10)
parqit collect, clear
sort id
capture cf id grp x s1 s2 s3 s4 using `"`nat'"'
if (_rc) {
    di as err "FAIL: egen seq() without to() differs from native Stata"
    local ++fails
}

if (`fails' == 0) di "VERDICT(V189_NATIVE_DIVERGENCES): PASS"
else di "VERDICT(V189_NATIVE_DIVERGENCES): FAIL (`fails')"
