* V82 — audit 2026-09-01 remediation, T5/T6:
*   MOD-TRUNC-1  mod() with a non-integer modulus matches native
*                (x - y*trunc(x/y), shifted by +y when negative);
*   DFMT-1       the old-style %d/%-d daily date formats are written as DATE
*                (a date32 for third parties) and restored exactly;
*   COUNT-FMT-1  a (count) of a string source carries %8.0g, not the %s
*                format Stata rejects (no "skipping display format" note);
*   DROP-IN-1    `parqit drop in f/l` (f/l letters, negative bounds) is the
*                complement of keep in, validated like it;
*   TAB-LABEL-1  tabulate displays value labels like native, nolabel shows
*                the codes, r() unchanged;
*   DUPLIST-SEP-1 a duplicates list cell holding a TAB stays one cell.
* Oracles: native Stata (mod values, drop in row sets, tabulate rendering),
* pyarrow (date32 physical type), cf on the round-trips.
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

tempfile stem
local dir `"`stem'_d"'
mkdir `"`dir'"'

* ---------- MOD-TRUNC-1 ----------------------------------------------------------
clear
input double x double y
7 0.00001
1 0.1
0.3 0.1
5.5 2
-5.5 2
1000000000000000.5 1
-7 3
7 3
2.5 0.3
10 0.00001
7 0
7 -3
. 3
3 .
end
gen long id = _n
gen double nat = mod(x, y)
parqit save `"`dir'/mod.parquet"', replace data
parqit use using `"`dir'/mod.parquet"'
parqit gen double pq = mod(x, y)
parqit collect, clear
sort id
forvalues i = 1/14 {
    assert (nat[`i'] == . & pq[`i'] == .) | (nat[`i'] == pq[`i'])
}
parqit close _all

* ---------- DFMT-1 -----------------------------------------------------------------
clear
set obs 4
gen long id = _n
gen long dd = td(01jan2020) + _n
format dd %d
gen double d2 = td(15mar2021) - _n
format d2 %-dCCYY-NN-DD
gen double d3 = td(01jan1960) + _n
format d3 %dM_d,_CY
replace d3 = . in 2
tempfile dref
save `"`dref'"', replace
parqit save `"`dir'/dfmt.parquet"', replace data
python:
from sfi import Macro
import os, pyarrow.parquet as pq
sch = pq.read_schema(os.path.join(Macro.getLocal("dir"), "dfmt.parquet"))
ok = all(str(sch.field(n).type) == "date32[day]" for n in ("dd", "d2", "d3"))
Macro.setLocal("dfmt_ok", "1" if ok else "0")
end
assert "`dfmt_ok'" == "1"
parqit use `"`dir'/dfmt.parquet"', clear
cf _all using `"`dref'"'
assert "`: format dd'" == "%d"
assert "`: format d2'" == "%-dCCYY-NN-DD"
assert "`: format d3'" == "%dM_d,_CY"
* the lazy path reads a %d column as a day count and a view save writes DATE
parqit use using `"`dir'/dfmt.parquet"'
parqit gen double y = year(dd)
parqit save `"`dir'/dfmt2.parquet"', replace
parqit collect, clear
assert y[1] == 2020
parqit close _all
python:
from sfi import Macro
import os, pyarrow.parquet as pq
sch = pq.read_schema(os.path.join(Macro.getLocal("dir"), "dfmt2.parquet"))
Macro.setLocal("dfmt2_ok", "1" if str(sch.field("dd").type) == "date32[day]" else "0")
end
assert "`dfmt2_ok'" == "1"

* ---------- COUNT-FMT-1 --------------------------------------------------------------
clear
set obs 6
gen byte g = mod(_n, 2)
gen str5 s = cond(_n <= 4, "x", "")
gen double price = _n * 10
format price %12.2f
parqit save `"`dir'/cnt.parquet"', replace data
capture log close v82c
log using `"`dir'/v82c.log"', replace name(v82c) text
parqit use using `"`dir'/cnt.parquet"'
parqit collapse (count) ns = s (count) np = price (mean) mp = price, by(g)
parqit collect, clear
log close v82c
assert "`: format ns'" == "%8.0g"
assert "`: format np'" == "%12.2f"      // native keeps the source format
assert "`: format mp'" == "%12.2f"
assert ns[1] == 2 & np[1] == 3
python:
from sfi import Macro
import os
txt = open(os.path.join(Macro.getLocal("dir"), "v82c.log"), encoding="utf-8", errors="replace").read()
Macro.setLocal("cnt_note", "1" if "skipping display format" in txt else "0")
end
assert "`cnt_note'" == "0"
parqit close _all

* ---------- DROP-IN-1 --------------------------------------------------------------
clear
set obs 10
gen long id = _n
gen double v = 100 - _n
parqit save `"`dir'/din.parquet"', replace data
tempfile dref2
save `"`dref2'"', replace
foreach rng in "2/3" "3/l" "-2/l" "5" "f/2" "1/l" {
    use `"`dref2'"', clear
    sort id
    capture noisily drop in `rng'
    local nrc = _rc
    tempfile nat
    if (!`nrc') save `"`nat'"', replace
    parqit use using `"`dir'/din.parquet"'
    parqit sort id
    capture noisily parqit drop in `rng'
    local prc = _rc
    if (!`prc') capture noisily parqit collect, clear
    local prc = cond(`prc', `prc', _rc)
    parqit close _all
    if (`nrc' == 0) {
        assert `prc' == 0
        sort id
        cf _all using `"`nat'"'
    }
    else assert `prc' != 0
}
* refusals: reversed, zero and past-the-end ranges
parqit use using `"`dir'/din.parquet"'
capture noisily parqit drop in 5/3
assert _rc == 198
capture noisily parqit drop in 0/2
assert _rc == 198
parqit drop in 20/25
capture noisily parqit count
assert _rc != 0
parqit close _all
* drop in composes with the pipeline (a prior keep if) and with _n
parqit use using `"`dir'/din.parquet"'
parqit sort id
parqit keep if id > 2
parqit drop in 1/2
parqit gen long n = _n
parqit collect, clear
assert _N == 6 & id[1] == 5 & n[1] == 1 & id[6] == 10 & n[6] == 6
parqit close _all

* ---------- TAB-LABEL-1 --------------------------------------------------------------
clear
set obs 6
gen byte g = mod(_n, 3)
label define gl 0 "zero" 1 "one" 2 "two"
label values g gl
gen byte h = _n > 3
label define hl 0 "low" 1 "high"
label values h hl
gen double v = _n
parqit save `"`dir'/tab.parquet"', replace data
log using `"`dir'/v82t.log"', replace name(v82t) text
parqit use using `"`dir'/tab.parquet"'
parqit tabulate g
assert r(N) == 6 & r(r) == 3
parqit tabulate g, nolabel
assert r(N) == 6 & r(r) == 3
parqit tabulate g h
assert r(N) == 6 & r(r) == 3 & r(c) == 2
parqit tabulate g h, nolabel
parqit levelsof g
assert "`r(levels)'" == "0 1 2"
parqit close _all
log close v82t
python:
from sfi import Macro
import os, re
txt = open(os.path.join(Macro.getLocal("dir"), "v82t.log"), encoding="utf-8", errors="replace").read()
# one-way: labels shown, then codes with nolabel
ok = txt.count("zero") >= 2 and txt.count("two") >= 2      # one-way + two-way rows
ok = ok and re.search(r"^\s+0\s+2\s+33\.33%", txt, re.M) is not None   # nolabel one-way
ok = ok and "low" in txt and "high" in txt                  # two-way column labels
Macro.setLocal("tab_ok", "1" if ok else "0")
end
assert "`tab_ok'" == "1"

* ---------- DUPLIST-SEP-1 --------------------------------------------------------------
clear
set obs 3
gen str10 s = "a" + char(9) + "b"
gen long k = 1
parqit save `"`dir'/dup.parquet"', replace data
log using `"`dir'/v82d.log"', replace name(v82d) text
parqit use using `"`dir'/dup.parquet"'
parqit duplicates list k
parqit close _all
log close v82d
python:
from sfi import Macro
import os, re
txt = open(os.path.join(Macro.getLocal("dir"), "v82d.log"), encoding="utf-8", errors="replace").read()
# the TAB stays inside the first cell ("a<TAB>b", which the text log may show
# as spaces), padded, then the k cell "1" — never a third cell
Macro.setLocal("dup_ok", "1" if re.search(r"^\s+a\s+b\s+1\s*$", txt, re.M) else "0")
end
assert "`dup_ok'" == "1"

di "VERDICT(V82_AUDIT_FIXES_20260901): PASS - mod() non-integer modulus native, %d written as DATE and restored, string (count) carries %8.0g without a note, drop in matches native (f/l, negatives, refusals, composition), tabulate shows value labels with nolabel for codes, duplicates list keeps a TAB inside its cell"
