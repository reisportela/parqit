* V27 — adversarial-audit fixes (2026-06-14). End-to-end Stata regressions for
* the correctness bugs found by the cross-audit:
*   TT-1       merge/joinby match missing/empty/NaN keys across tool encodings
*   RESHAPE-1  reshape long: a stub that prefixes another var carries it, not absorbs
*   RESHAPE-2  reshape long: an i() var named like a stub is not consumed
*   COLLAPSE-1 collapse (count) on a string counts NONMISSING (skips "")
*   STR-1      a UUID column does not crash the eager `parqit use` path
*   TYPE-1     a finite double >= Stata's missing sentinel loads as missing WITH a note
*   TYPE-2     saving an out-of-range period/date value errors loudly (no silent wrap)
* Every data check uses an independent oracle (pyarrow / duckdb), never parqit alone.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

local fails 0
tempfile t

* ---- RESHAPE-1 / RESHAPE-2 -------------------------------------------------
clear
input id x2 inc1 inc2 income
1 7 10 11 100
2 8 20 21 200
end
parqit save `"`t'_rl.parquet"', replace data
parqit use using `"`t'_rl.parquet"'
parqit reshape long inc, i(id) j(year)
parqit collect, clear
gsort id year
capture assert _N==4 & income[1]==100 & income[2]==100 & inc[1]==10 & inc[2]==11
local r = _rc
if (`r') di as err "FAIL RESHAPE-1: income absorbed or rows fabricated"
local fails = `fails' + (`r'!=0)
capture confirm variable x2
local r = _rc
if (`r') di as err "FAIL RESHAPE-2: i() var x2 consumed by stub"
local fails = `fails' + (`r'!=0)

* ---- COLLAPSE-1 ------------------------------------------------------------
clear
input id str5 g
1 "a"
2 ""
3 "b"
end
gen byte one = 1
parqit save `"`t'_c.parquet"', replace data
parqit use using `"`t'_c.parquet"'
parqit collapse (count) ng=g (count) n1=one
parqit collect, clear
capture assert ng[1]==2 & n1[1]==3
local r = _rc
if (`r') di as err "FAIL COLLAPSE-1: count on string counted empty strings"
local fails = `fails' + (`r'!=0)

* ---- TT-1 (merge missing/empty/NaN keys) — pyarrow oracle ------------------
python:
import pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro
base = Macro.getLocal("t")
pq.write_table(pa.table({"g": pa.array(["x", None]), "k": pa.array([1.0, float("nan")]), "mv":[1,2]}), base+"_mm.parquet")
pq.write_table(pa.table({"g": pa.array(["x", ""]), "uv":[10,20]}), base+"_us.parquet")
pq.write_table(pa.table({"k": pa.array([1.0, None]), "uk":[100,200]}), base+"_uk.parquet")
end
parqit use using `"`t'_mm.parquet"'
parqit merge m:1 g using `"`t'_us.parquet"', keepusing(uv)
parqit collect, clear
count if _merge==3
capture assert r(N)==2
local r = _rc
if (`r') di as err "FAIL TT-1: string missing-key did not match across NULL/'' encodings"
local fails = `fails' + (`r'!=0)
parqit use using `"`t'_mm.parquet"'
parqit merge m:1 k using `"`t'_uk.parquet"', keepusing(uk)
parqit collect, clear
count if _merge==3
capture assert r(N)==2
local r = _rc
if (`r') di as err "FAIL TT-1: numeric NaN-key did not match NULL"
local fails = `fails' + (`r'!=0)

* ---- STR-1 (UUID does not crash eager use) — duckdb oracle -----------------
shell duckdb -c "COPY (SELECT gen_random_uuid() AS uid, i AS n FROM range(3) t(i)) TO '`t'_uuid.parquet' (FORMAT PARQUET)"
capture parqit use using `"`t'_uuid.parquet"', clear
local r = _rc
if (`r') di as err "FAIL STR-1: UUID crashed eager use (rc=`r')"
local ty = cond(`r', "<crash>", "`:type uid'")
capture assert "`ty'" == "str36"
local r2 = _rc
if (`r2') di as err "FAIL STR-1: UUID column not loaded as str36 (got `ty')"
local fails = `fails' + (`r'!=0) + (`r2'!=0)

* ---- TYPE-1 (finite double >= sentinel -> missing, never a silent value) ---
python:
import pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro
base = Macro.getLocal("t")
pq.write_table(pa.table({"d": pa.array([1.5, 9e307, 1e308, 8.9e307, -1e308])}), base+"_sent.parquet")
end
parqit use using `"`t'_sent.parquet"', clear
count if missing(d)
capture assert r(N)==3
local r = _rc
if (`r') di as err "FAIL TYPE-1: sentinel-band doubles not flagged missing (got `r(N)')"
local fails = `fails' + (`r'!=0)
capture assert d[1]==1.5 & reldif(d[4], 8.9e307) < 1e-12
local r = _rc
if (`r') di as err "FAIL TYPE-1: in-range double corrupted"
local fails = `fails' + (`r'!=0)

* ---- TYPE-2 (out-of-range period value errors loudly, no silent wrap) ------
clear
set obs 1
gen double bigm = 5000000000
format bigm %tm
capture parqit save `"`t'_ov.parquet"', replace data
local r = _rc
if (`r'==0) di as err "FAIL TYPE-2: out-of-range period value saved silently"
local fails = `fails' + (`r'==0)

* ---------------------------------------------------------------------------
if (`fails'==0) {
    di "VERDICT(V27_AUDIT_FIXES): PASS - reshape/collapse/merge-missing/UUID/double-sentinel/save-overflow"
}
else {
    di "VERDICT(V27_AUDIT_FIXES): FAIL - `fails' check(s) failed"
}
