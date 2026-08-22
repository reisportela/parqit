* V77 — BRIDGE-LOSS-1 (audit 2026-08-22, A5-3/A5-2/A2-8) and CHAR-LEN-1 (A2-7).
* A .dta/Excel source read through the adapter bridge is a `parqit save` of the
* imported frame, so the save-side conversions apply: extended missings .a-.z
* collapse to ., fractional date/period counts round, legacy 8-bit text is
* transcoded (encoding() chooses the code page, windows-1252 by default). Those
* losses were SILENT on the bridge; they are now printed as notes naming the
* bridged file and returned as r(ext_missing)/r(frac_dates)/r(transcoded_*)/
* r(encoding) by every command that bridges: parqit use (lazy and eager),
* merge/append/joinby using a .dta, parqit open _data. A characteristic beyond
* Stata's 67,783-byte limit is truncated with a loud note on read instead of
* vanishing.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

tempfile stem
local lossy `"`stem'_lossy.dta"'
local master `"`stem'_master.parquet"'

* the lossy .dta: .a, a fractional %td, a legacy (Latin-1) label and note
clear
set obs 3
gen long id = _n
gen double e = .a
replace e = 1 in 2
gen double d = td(01jan2020) + 0.5
format d %td
gen str8 s = "caf" + char(233)
local lab = "Regi" + char(227) + "o"
label var s "`lab'"
notes: `lab'
qui save `"`lossy'"', replace
clear
set obs 3
gen long id = _n
gen double m = _n * 10
parqit save `"`master'"', replace data

* ---------- lazy use: notes + r() ---------------------------------------------
capture erase v77a.log
log using v77a.log, text replace name(v77a)
parqit use using `"`lossy'"'
local a_ext "`r(ext_missing)'"
local a_frac "`r(frac_dates)'"
local a_tvars "`r(transcoded_vars)'"
local a_tcells = r(transcoded_cells)
local a_tmeta = r(transcoded_meta)
local a_enc "`r(encoding)'"
log close v77a
assert "`a_ext'" == "e"
assert "`a_frac'" == "d"
assert "`a_tvars'" == "s"
assert `a_tcells' == 3
assert `a_tmeta' >= 2
assert "`a_enc'" == "windows-1252"
parqit collect, clear
assert e[1] == . & e[2] == 1 & d[1] == td(01jan2020) + 1
assert s[1] == "café" & `"`: var label s'"' == "Região"
parqit close _all
python:
from sfi import Macro
txt = open("v77a.log", encoding="utf-8", errors="replace").read().replace("\n> ", "")
ok = ("while bridging" in txt and "extended missing values" in txt and "rounded to the nearest unit" in txt
      and "transcoded from" in txt and "windows-1252" in txt)
Macro.setLocal("okl", "1" if ok else "0")
end
assert "`okl'" == "1"

* ---------- eager use: same, plus encoding(latin9) changes the code page --------
parqit use using `"`lossy'"', clear
assert "`r(ext_missing)'" == "e" & "`r(frac_dates)'" == "d" & "`r(transcoded_vars)'" == "s"
assert r(transcoded_cells) == 3 & "`r(encoding)'" == "windows-1252"
assert s[1] == "café"
parqit use using `"`lossy'"', clear encoding(latin9)
assert "`r(encoding)'" == "latin9"
assert s[1] == "café"                                   // 0xE9 is é in latin9 too
capture noisily parqit use using `"`lossy'"', clear encoding(bogus)
assert _rc == 198
* a Parquet source with encoding(): accepted with a note (nothing to transcode)
parqit use using `"`master'"', clear encoding(latin1)
assert _N == 3
assert "`r(encoding)'" == ""

* ---------- merge / joinby / append using the .dta -----------------------------
parqit use using `"`master'"'
parqit merge 1:1 id using `"`lossy'"'
assert "`r(ext_missing)'" == "e" & "`r(frac_dates)'" == "d" & "`r(transcoded_vars)'" == "s"
assert r(transcoded_cells) == 3
parqit collect, clear
assert s[1] == "café" & e[1] == .
parqit use using `"`master'"'
parqit merge 1:1 id using `"`lossy'"', encoding(macroman)
assert "`r(encoding)'" == "macroman"
parqit collect, clear
assert s[1] != "café"                                   // 0xE9 is not é in MacRoman
parqit use using `"`master'"'
parqit joinby id using `"`lossy'"'
assert "`r(ext_missing)'" == "e" & "`r(transcoded_vars)'" == "s"
parqit close _all
parqit use using `"`master'"'
parqit append using `"`lossy'"' `"`lossy'"'
assert "`r(ext_missing)'" == "e" & "`r(frac_dates)'" == "d" & "`r(transcoded_vars)'" == "s"
assert r(transcoded_cells) == 6 & r(n_bridges) == 2     // accumulated over both bridges
parqit close _all

* ---------- open _data: transcoded text reported and returned -----------------
use `"`lossy'"', clear
parqit open _data
assert "`r(ext_missing)'" == "e" & "`r(frac_dates)'" == "d" & "`r(transcoded_vars)'" == "s"
assert r(transcoded_cells) == 3 & "`r(encoding)'" == "windows-1252"
parqit close _all
use `"`lossy'"', clear
parqit open _data, encoding(latin1)
assert "`r(encoding)'" == "latin1"
parqit close _all

* ---------- CHAR-LEN-1: a characteristic past 67,783 bytes is truncated loudly --
clear
set obs 1
gen long id = 1
local big = 40000 * char(233)                           // 40,000 Latin-1 é -> 80,000 UTF-8 bytes
char _dta[bignote] "`big'"
tempfile bigc
parqit save `"`bigc'.parquet"', replace data
assert r(transcoded_meta) >= 1
capture erase v77c.log
log using v77c.log, text replace name(v77c)
parqit use using `"`bigc'.parquet"', clear
log close v77c
local got : char _dta[bignote]
assert strlen(`"`got'"') == 67783
python:
from sfi import Macro
import pyarrow.parquet as pq, json
ch = json.loads(pq.read_schema(Macro.getLocal("bigc") + ".parquet").metadata[b"parqit.chars"])
txt = open("v77c.log", encoding="utf-8", errors="replace").read().replace("\n> ", "")
ok = len(ch["_dta"]["bignote"].encode("utf-8")) == 80000 and "67,783" in txt and "truncated" in txt
Macro.setLocal("okc", "1" if ok else "0")
end
assert "`okc'" == "1"

di "VERDICT(V77_BRIDGE_LOSSES): PASS - .dta bridges report and return ext-missing/fractional/transcoding losses on use (lazy+eager), merge, joinby, append, open _data; encoding() honoured on every bridging command; oversize characteristic truncated loudly"
