* M1 roundtrip: every base Stata type, missings, labels, formats, notes,
* chars and value labels survive parqit save → parqit use; the on-disk payload
* is verified with pyarrow (independent oracle), incl. the charter's date
* rules: %td → DATE, %tc → TIMESTAMP, %tm stays INTEGER (never a calendar
* date), %tcHH:MM:SS stays a datetime (never an all-null time).
* Linear script: any failure stops before the final VERDICT line, which the
* runner reports as a failure.
*
* Usage: do t01_basic_roundtrip.do <repo_root> <plugin_path>
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

* ---------- build the fixture ----------
set obs 7
gen byte   b   = _n - 3            // -2..4
replace    b   = .  in 6
replace    b   = .a in 7
gen int    i16 = _n * 1000
replace    i16 = . in 6
gen long   i32 = _n * 100000
gen float  f32 = _pi * _n
replace    f32 = . in 6
gen double f64 = exp(_n) * 1e10
replace    f64 = -0 in 5
replace    f64 = . in 6
gen str8   s8  = "v" + string(_n)
replace    s8  = ""   in 6
replace    s8  = `"q"q"' in 7      // embedded quote
gen strL   sL  = s8 + " — longo 🦆 " + string(_n)
replace    sL  = sL * 200 in 3     // > 2045 bytes → true strL

gen long  d   = td(01jan1960) + (_n - 1) * 7000
format    d   %td
gen double t  = tc(01jan1960 00:00:00) + (_n - 1) * 90061234
format    t   %tcDD/NN/CCYY_HH:MM:SS   // display tokens must not corrupt storage
gen int   m   = tm(2026m1) + _n        // period count: stays integer on disk
format    m   %tm

label define yesno 0 "no" 1 "yes" .a "refused"
gen byte yn = mod(_n, 2)
replace  yn = .a in 7
label values yn yesno
label variable yn "did the thing"
label variable f64 "big floats with -0"
note yn: first note on yn
note: a dataset-level note with ümläuts
char yn[origin] "survey wave 3"
label data "parqit roundtrip fixture"

* ---------- write through parqit ----------
tempfile outbase
local out `"`outbase'.parquet"'
parqit save `"`out'"', replace
assert r(N) == 7 & r(k) == 11

* ---------- independent oracle: pyarrow reads what parqit wrote ----------
python:
from sfi import Macro
import pyarrow.parquet as pq
import datetime as dt
t = pq.read_table(Macro.getLocal("out"))
s = {f.name: str(f.type) for f in t.schema}
ok = True
def chk(cond, what):
    global ok
    if not cond:
        ok = False
        print("ORACLE FAIL:", what)
chk(s["b"] == "int8" and s["i16"] == "int16" and s["i32"] == "int32", "integer widths: " + str(s))
chk(s["f32"] == "float" and s["f64"] == "double", "float widths")
chk(s["s8"] == "string" and s["sL"] == "string", "strings")
chk(s["d"] == "date32[day]", "date32 for %td: " + s["d"])
chk(s["t"].startswith("timestamp"), "timestamp for %tc despite HH:MM tokens: " + s["t"])
chk(s["m"] == "int32", "%tm stays integer, never date: " + s["m"])
cols = t.to_pydict()
chk(cols["b"][:5] == [-2, -1, 0, 1, 2] and cols["b"][5] is None and cols["b"][6] is None,
    "byte payload incl . and .a -> null: " + str(cols["b"]))
chk(cols["d"][0] == dt.date(1960, 1, 1), "epoch date exact: " + str(cols["d"][0]))
chk(cols["t"][0] == dt.datetime(1960, 1, 1, 0, 0), "epoch datetime exact: " + str(cols["t"][0]))
chk(cols["t"][1] == dt.datetime(1960, 1, 2, 1, 1, 1, 234000), "ms precision: " + str(cols["t"][1]))
chk(cols["m"][0] == 793, "tm(2026m1)+1 = 793 on disk: " + str(cols["m"][0]))
chk(cols["s8"][5] == "" and cols["s8"][6] == 'q"q', "empty + quoted strings: " + str(cols["s8"][5:7]))
chk(len(cols["sL"][2].encode()) > 2045, "strL payload length")
chk(cols["f64"][4] == 0.0, "minus zero survives as 0")
md = t.schema.metadata or {}
chk(b"parqit.schema" in md and b"parqit.vallabs" in md, "parqit.* KV metadata present")
Macro.setLocal("oracle_ok", "1" if ok else "0")
end
assert "`oracle_ok'" == "1"

* ---------- read back through parqit ----------
parqit use using `"`out'"', clear
assert _N == 7 & c(k) == 11

* c() hygiene: no vanishing tempfile path, nothing "changed"
assert `"`c(filename)'"' == ""
assert c(changed) == 0

* payloads (collapse of .a -> . is the documented v1 loss)
assert b[1] == -2 & b[5] == 2
assert b[6] == . & b[7] == .
assert i16[2] == 2000 & i16[6] == .
assert i32[7] == 700000
assert reldif(f32[1], _pi) < 1e-7
assert f64[6] == . & f64[5] == 0
assert s8[6] == "" & s8[7] == `"q"q"'
assert strlen(sL[3]) > 2045
assert d[1] == td(01jan1960) & d[7] == td(01jan1960) + 42000
local fmt_d : format d
assert "`fmt_d'" == "%td"
assert t[2] == tc(01jan1960 00:00:00) + 90061234
local fmt_t : format t
assert strpos("`fmt_t'", "%tc") == 1
assert m[1] == tm(2026m1) + 1
local fmt_m : format m
assert "`fmt_m'" == "%tm"

* metadata round-trip
assert `"`: variable label yn'"' == "did the thing"
assert `"`: value label yn'"' == "yesno"
assert `"`: label yesno 1'"' == "yes"
assert `"`: label yesno .a'"' == "refused"
assert `"`: char yn[origin]'"' == "survey wave 3"
assert `"`: data label'"' == "parqit roundtrip fixture"
assert `"`: char yn[note1]'"' != ""
assert `"`: char _dta[note1]'"' != ""

di "VERDICT(T01_ROUNDTRIP): PASS - all types, payloads, metadata verified (pyarrow oracle)"
