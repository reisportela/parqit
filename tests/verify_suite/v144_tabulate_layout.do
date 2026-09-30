* V144 — tabulate's layout is native tabulate's (TAB-LAYOUT-1, 2026-09-29):
*   the one-way and two-way tables print the text native tabulate prints when
*   a variable's label is long (it wraps, word by word within the stub's
*   bytes, and never widens the stub), when value labels or string values are
*   long (cut, or abbreviated with ".." for strings; numbers in their format
*   at width 9, never cut), for str# variables (the
*   stub follows the storage width), for a long column label (wrapped to the
*   columns and centred) and for tables wider than the line (panels).
* Oracle: native tabulate on the same data in memory; the table lines of the
*   two logs are compared text for text (trailing blanks aside).
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
tempfile stem
local out `"`stem'_logs"'
mkdir `"`out'"'
global V144_OUT `"`out'"'

* native on the data in memory, parqit on a view of the same data
program define _v144_pair
    version 16.0
    args tag cmd
    tempfile f
    quietly parqit save `"`f'.parquet"', data replace
    quietly parqit use using `"`f'.parquet"', name(v144)
    quietly log using `"${V144_OUT}/`tag'_native.log"', text replace name(v144n)
    `cmd'
    quietly log close v144n
    quietly log using `"${V144_OUT}/`tag'_parqit.log"', text replace name(v144p)
    parqit `cmd'
    quietly log close v144p
    parqit close v144
end

parqit set statamissing off
set linesize 80
sysuse auto, clear
_v144_pair two_auto     "tabulate rep78 foreign"
_v144_pair two_short    "tabulate foreign rep78"
_v144_pair one_auto     "tabulate rep78"
_v144_pair one_short    "tabulate foreign"
_v144_pair two_rowcol   "tabulate rep78 foreign, row column"
_v144_pair two_missing  "tabulate rep78 foreign, missing"
_v144_pair two_nolabel  "tabulate rep78 foreign, nolabel"
_v144_pair one_string   "tabulate make if price > 13000"
_v144_pair two_string   "tabulate make foreign if price > 13000"

* long value labels, and a long row label over them
label define rl 1 "a" 2 "abcdefghijkl" 3 "abcdefghijklmnopqrst" ///
    4 "abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmn" 5 "x y z w v u t s r q p o"
label values rep78 rl
_v144_pair two_longvals "tabulate rep78 foreign"
_v144_pair one_longvals "tabulate rep78"
label variable rep78 "Repair record 1978 of the car, long label here"
_v144_pair two_bothlong "tabulate rep78 foreign"
_v144_pair one_bothlong "tabulate rep78"
label values rep78

* one word longer than the stub, accents (bytes), a long column label, labels
* of the column values longer than 9, no labels at all
label variable rep78 "Displacementincubicinches"
_v144_pair two_word     "tabulate rep78 foreign"
_v144_pair one_word     "tabulate rep78"
label variable rep78 "Região de residência habitual"
_v144_pair two_utf      "tabulate rep78 foreign"
_v144_pair one_utf      "tabulate rep78"
label variable rep78 "Repair record 1978"
label variable foreign "Origin of the car as recorded in the 1978 data"
_v144_pair two_collabel "tabulate rep78 foreign"
label variable foreign "Ação é útil"
_v144_pair two_colutf   "tabulate rep78 foreign"
label variable foreign "Car origin"
label define ol 0 "Domestic cars made in USA" 1 "abcdefghij"
label values foreign ol
_v144_pair two_colvals  "tabulate rep78 foreign"
label values foreign
label variable rep78 ""
label variable foreign ""
_v144_pair two_names    "tabulate rep78 foreign"

* string variables: storage width, and ".." for values that do not fit
sysuse auto, clear
gen int id = _n
gen str30 s30 = substr(make, 1, 5)
gen str50 longs = "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGH" + string(_n)
gen str20 cs = cond(foreign, "Foreign-made car", "Dom")
_v144_pair two_str30    "tabulate s30 foreign if price > 13000"
_v144_pair one_str30    "tabulate s30 if price > 13000"
_v144_pair two_strlong  "tabulate longs foreign if id <= 6"
_v144_pair one_strlong  "tabulate longs if id <= 6"
_v144_pair two_strcol   "tabulate rep78 cs"

* wider than the line: panels, at three line sizes around the boundary
gen byte c15 = mod(_n, 15)
label variable c15 "Fifteen column groups for a wide table"
foreach ls in 80 79 78 {
    set linesize `ls'
    _v144_pair two_wide`ls' "tabulate rep78 c15"
}

* a number is shown in its format at width 9 (e-notation, no commas) and is
* never cut
clear
set obs 8
gen byte g = 1
gen double v = cond(_n == 1, 12345678, cond(_n == 2, 123456789, cond(_n == 3, 1234567891, ///
    cond(_n == 4, 1.5, cond(_n == 5, 123.456789, cond(_n == 6, -1234567, ///
    cond(_n == 7, 0.000012345, 98765.4321)))))))
set linesize 200
foreach f in %12.0g %10.2f %15.0fc {
    format v `f'
    local t = subinstr(subinstr("`f'", "%", "", .), ".", "_", .)
    _v144_pair two_numcol`t' "tabulate g v"
    _v144_pair two_numrow`t' "tabulate v g"
    _v144_pair one_num`t'    "tabulate v"
}

* a date or time shows in its format when that fits 9 columns, else in its
* class's default format, else abbreviated with ".."
clear
set obs 4
gen byte g = mod(_n, 2)
gen int d = mdy(1, _n, 2020)
format d %tdCCYY-NN-DD
gen int m = ym(2020, _n)
format m %tmMonth_CCYY
gen double tc = clock("2020-01-0" + string(_n) + " 13:45:10", "YMDhms")
format tc %tc
gen double hm = tc
format hm %tcHH:MM
foreach v in d m tc hm {
    _v144_pair two_datecol_`v' "tabulate g `v'"
    _v144_pair two_daterow_`v' "tabulate `v' g"
    _v144_pair one_date_`v'    "tabulate `v'"
}

python:
import glob, os
from sfi import Macro
out = Macro.getGlobal("V144_OUT")
def table(path):
    rows = []
    for s in open(path, encoding="utf-8"):
        s = s.rstrip()
        if s.startswith(". ") or s.startswith("> "):
            continue
        if "|" in s or (s.startswith("-") and "+" in s):
            rows.append(s)
    return rows
cases = sorted(p[len(out) + 1:-len("_native.log")] for p in glob.glob(os.path.join(out, "*_native.log")))
assert len(cases) == 50, cases
bad = []
for tag in cases:
    nat = table(os.path.join(out, tag + "_native.log"))
    pq_ = table(os.path.join(out, tag + "_parqit.log"))
    if len(nat) < 4 or nat != pq_:
        diff = [f"  native: {a!r}\n  parqit: {b!r}" for a, b in zip(nat, pq_) if a != b][:3]
        bad.append(f"{tag} ({len(nat)} vs {len(pq_)} lines)\n" + "\n".join(diff))
assert not bad, "\n".join(bad)
end

di "VERDICT(V144_TABULATE_LAYOUT): PASS - one-way and two-way tabulate print native tabulate's table in 50 layouts: long variable labels wrap by bytes within the stub and never widen it, long value labels are cut and long strings abbreviated with .., numbers show in their format at width 9 and are never cut, dates and times in their format or their class default within 9 columns, str# stubs follow the storage width, long column labels wrap centred over the columns, column values show 9 characters, and wide tables split into native panels at the line-size boundary"
