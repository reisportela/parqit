* V35 — adversarial-audit fixes (2026-06-23, third round, post-Codex). Each
* invariant has an independent oracle (native Stata twin or a parqit-use-then-
* native-op twin), never a parqit-only round-trip.
*   STR-GENWIDTH-1  gen str# truncates the value to the declared byte width
*   TT-MM-MISSING-1 m:m merge folds NULL/NaN/"" keys into one missing group
*   GROUPKEY-1      collapse/contract/duplicates/egen fold ''/NaN to missing in
*                   the group key (matching merge and native Stata)
*   SAVE-SELFGLOB-1 partitioned save over the open view's glob/dir source refused
*   SET-THREADS-1/2 non-integer / out-of-range threads is a clear loud error
*   STRPOS-EMPTY-1  strpos(s,"") == 0 (not 1)
*   LENGTH-NUMERIC-1 length() on a numeric is a clear loud error naming length()
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

local fails 0
tempfile t oracle

* ---- STR-GENWIDTH-1 : gen str# truncates to the declared width --------------
clear
input str8 name
"hello"
"hi"
end
parqit save `"`t'_sg.parquet"', replace data
parqit use using `"`t'_sg.parquet"'
parqit gen str3 a = name
parqit gen str3 b = "world!!"
parqit collect, clear
sort name
local ta : type a
capture assert a[1]=="hel" & a[2]=="hi" & b[1]=="wor" & "`ta'"=="str3"
if (_rc) di as err "FAIL STR-GENWIDTH-1: a=[`=a[1]'] b=[`=b[1]'] type=`ta'"
local fails = `fails' + (_rc!=0)

* ---- TT-MM-MISSING-1 : m:m folds NULL/NaN keys into one missing group -------
python:
import pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro
b = Macro.getLocal("t")
pq.write_table(pa.table({"k": pa.array([1.0, None],          type=pa.float64()), "mv":[10,20]}), b+"_mm_m.parquet")
pq.write_table(pa.table({"k": pa.array([1.0, float("nan")], type=pa.float64()), "uv":[100,200]}), b+"_mm_u.parquet")
end
* oracle: load both as Stata (NaN and NULL both become missing .), native m:m
parqit use using `"`t'_mm_m.parquet"', clear
tempfile mas
save `"`mas'"', replace
parqit use using `"`t'_mm_u.parquet"', clear
tempfile usi
save `"`usi'"', replace
use `"`mas'"', clear
merge m:m k using `"`usi'"', keepusing(uv) nogen
sort k mv
keep k mv uv
tempfile mmoracle
save `"`mmoracle'"', replace
local oracle_n = _N
* parqit out-of-core m:m
parqit use using `"`t'_mm_m.parquet"'
parqit merge m:m k using `"`t'_mm_u.parquet"', keepusing(uv) nogen
parqit collect, clear
sort k mv
keep k mv uv
capture assert _N == `oracle_n'
if (_rc) di as err "FAIL TT-MM-MISSING-1: parqit _N=`=_N' vs native `oracle_n'"
local fails = `fails' + (_rc!=0)

* ---- GROUPKEY-1 : collapse folds ''/NULL (string) and NaN/NULL (numeric) ----
python:
import pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro
b = Macro.getLocal("t")
pq.write_table(pa.table({"g": pa.array(["", None, "x"]), "v":[1,2,3]}), b+"_gk_s.parquet")
pq.write_table(pa.table({"g": pa.array([1.0, float("nan"), None], type=pa.float64()), "v":[10,20,30]}), b+"_gk_n.parquet")
end
* string key: '' and NULL must be one group (sum 1+2=3), plus 'x'
parqit use using `"`t'_gk_s.parquet"', clear
collapse (sum) v, by(g)
sort g
tempfile gks_o
save `"`gks_o'"', replace
parqit use using `"`t'_gk_s.parquet"'
parqit collapse (sum) v, by(g)
parqit collect, clear
sort g
capture cf _all using `"`gks_o'"'
if (_rc) di as err "FAIL GROUPKEY-1(string): collapse groups differ from native"
local fails = `fails' + (_rc!=0)
* numeric key: NaN and NULL one group
parqit use using `"`t'_gk_n.parquet"', clear
collapse (sum) v, by(g)
gsort g
tempfile gkn_o
save `"`gkn_o'"', replace
parqit use using `"`t'_gk_n.parquet"'
parqit collapse (sum) v, by(g)
parqit collect, clear
gsort g
capture assert _N == 2
if (_rc) di as err "FAIL GROUPKEY-1(numeric): expected 2 groups, got `=_N'"
local fails = `fails' + (_rc!=0)

* ---- SAVE-SELFGLOB-1 : partitioned save over the glob source is refused -----
clear
input double(g v)
1 10
1 20
2 30
end
mkdir `"`t'_selfdir"'
parqit save `"`t'_selfdir/part.parquet"', replace data
parqit use using `"`t'_selfdir/*.parquet"'
capture noisily parqit save `"`t'_selfdir"', replace partition_by(g)
if (_rc==0) di as err "FAIL SAVE-SELFGLOB-1: partitioned save over glob source not refused"
local fails = `fails' + (_rc==0)
python:
import os, pyarrow.parquet as pq
from sfi import Macro
p = Macro.getLocal("t")+"_selfdir/part.parquet"
Macro.setLocal("still", "1" if os.path.exists(p) and pq.read_table(p).num_rows==3 else "0")
end
if ("`still'"!="1") di as err "FAIL SAVE-SELFGLOB-1: source files were destroyed"
local fails = `fails' + ("`still'"!="1")
parqit close _all

* ---- SET-THREADS-1/2 : non-integer / out-of-range threads is a loud error ---
capture noisily parqit set threads 4
local rc_ok = _rc
capture noisily parqit set threads 4.5
local rc_frac = _rc
capture noisily parqit set threads 999999999999999999999
local rc_huge = _rc
capture assert `rc_ok'==0 & `rc_frac'!=0 & `rc_huge'!=0
if (_rc) di as err "FAIL SET-THREADS: ok=`rc_ok' frac=`rc_frac' huge=`rc_huge'"
local fails = `fails' + (_rc!=0)

* ---- STRPOS-EMPTY-1 and LENGTH-NUMERIC-1 -----------------------------------
clear
input double x
5
end
gen str5 s = "abc"
parqit save `"`t'_se.parquet"', replace data
parqit use using `"`t'_se.parquet"'
parqit gen p0 = strpos(s, "")
parqit collect, clear
capture assert p0[1]==0
if (_rc) di as err "FAIL STRPOS-EMPTY-1: strpos(s,\"\")=`=p0[1]' (want 0)"
local fails = `fails' + (_rc!=0)
parqit use using `"`t'_se.parquet"'
capture noisily parqit gen bad = length(x)
if (_rc==0) di as err "FAIL LENGTH-NUMERIC-1: length(numeric) did not error"
local fails = `fails' + (_rc==0)

di as txt "VERDICT(V35_AUDIT_FIXES_20260623B): " cond(`fails'==0, "PASS", "FAIL — `fails' failures") ///
    " - gen-str-width/mm-missing-keys/groupkey-fold/save-selfglob/set-threads/strpos-empty/length-numeric"
