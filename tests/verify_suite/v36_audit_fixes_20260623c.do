* V36 — follow-up audit fixes (2026-06-23). Independent oracles only.
*   COLLAPSE-EMPTY-1     a no-by collapse over zero input rows yields zero obs,
*                        not one fabricated (mean ., sum 0, count 0) row
*   SRCNAME-ROUNDTRIP    a foreign-named column survives load->save->load with
*                        its data and its original-name provenance intact
*                        (RESAVE-STALE-SRCNAME-1 was evaluated and NOT applied:
*                         dropping src_name would lose original-name recovery)
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

local fails 0
tempfile t

* ---- COLLAPSE-EMPTY-1 : no-by collapse over 0 rows -> 0 obs ----------------
clear
input double x
1
2
3
end
parqit save `"`t'_ce.parquet"', replace data
parqit use using `"`t'_ce.parquet"'
parqit keep if x > 9999
parqit collapse (mean) mx = x (sum) sx = x (count) nx = x
parqit collect, clear
capture assert _N == 0
if (_rc) di as err "FAIL COLLAPSE-EMPTY-1: no-by collapse over 0 rows gave `=_N' obs (want 0)"
local fails = `fails' + (_rc!=0)
* control: non-empty no-by collapse still yields exactly one row
parqit use using `"`t'_ce.parquet"'
parqit collapse (mean) mx = x (sum) sx = x (count) nx = x
parqit collect, clear
capture assert _N == 1 & reldif(mx[1], 2) < 1e-12 & sx[1]==6 & nx[1]==3
if (_rc) di as err "FAIL COLLAPSE-EMPTY-1 control: non-empty no-by collapse wrong (_N=`=_N')"
local fails = `fails' + (_rc!=0)

* ---- SRCNAME-ROUNDTRIP : foreign-named column survives load->save->load -----
python:
import pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro
b = Macro.getLocal("t")
pq.write_table(pa.table({"raw name":[10,20,30], "ok":[1,2,3]}), b+"_rs1.parquet")
end
* load foreign file: "raw name" is sanitised; src_name char records the original
parqit use using `"`t'_rs1.parquet"', clear
qui ds
local v1 `r(varlist)'
local sn0 : char `=word("`v1'",1)'[src_name]
capture assert "`sn0'" == "raw name"
if (_rc) di as err "FAIL SRCNAME-ROUNDTRIP: src_name not set on first load (`sn0')"
local fails = `fails' + (_rc!=0)
* re-save then reload: data and original-name provenance must survive (we keep
* src_name in the metadata so the original name stays recoverable)
parqit save `"`t'_rs2.parquet"', replace data
parqit use using `"`t'_rs2.parquet"', clear
qui ds
local v2 `r(varlist)'
local sn1 : char `=word("`v2'",1)'[src_name]
capture assert "`sn1'" == "raw name" & _N == 3
if (_rc) di as err "FAIL SRCNAME-ROUNDTRIP: original name/data lost on round trip (src=`sn1' N=`=_N')"
local fails = `fails' + (_rc!=0)

di as txt "VERDICT(V36_AUDIT_FIXES_20260623C): " cond(`fails'==0, "PASS", "FAIL — `fails' failures") ///
    " - collapse-empty-no-fabricated-row / foreign-name-provenance-roundtrip"
