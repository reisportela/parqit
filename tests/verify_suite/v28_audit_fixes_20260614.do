* V28 — adversarial-audit fixes (2026-06-14, second round). End-to-end Stata
* regressions for the correctness/perf bugs fixed after the multi-agent audit
* (PARQIT_ADVERSARIAL_AUDIT_2026-06-14_f386b5b.md):
*   NUM-1       x/0 and (-8)^0.5 -> missing in BOTH collect and save (no inf/nan on disk)
*   NUM-2       round() ties round toward +inf (round(-2.5)=-2), not away from zero
*   STR-1       upper()/lower() are ASCII-only; ustrupper()/ustrlower() are Unicode
*   DATE-1      an invalid mdy() triple is row-local missing, never a query abort
*   MERGE-1     m:1/1:1 uniqueness guard folds ""/NaN to Stata-missing like the join
*   MERGE-2     keep() is a set: keep(master master) returns MASTER rows, not using
*   RENAME-1    parqit rename (oldlist) (newlist) is accepted (documented syntax)
*   EGEN-1      egen fcn(expr) with internal commas, e.g. mean(cond(x>0,y,.))
*   RESHAPE-5   reshape long with a bare column == stub errors (Stata rc 110), not corrupt
*   TYPE-SAVE-1 in-memory save of an out-of-range %tc errors loudly (no int64-MIN on disk)
*   IO-1        parqit save onto the open view's own source file is refused
*   PERF-1      _n / _N idioms stay correct (gating the count(*) window did not change results)
* Every on-disk check uses an independent oracle (pyarrow), never parqit alone.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

local fails 0
tempfile t

* ---- NUM-2 : round() ties toward +inf -------------------------------------
clear
input id v
1 1
end
parqit save `"`t'_n.parquet"', replace data
parqit use using `"`t'_n.parquet"'
parqit gen rA = round(-2.5)
parqit gen rB = round(-0.5)
parqit gen rC = round(2.5)
parqit gen rD = round(-0.05, 0.1)
parqit collect, clear
capture assert rA[1]==-2 & rB[1]==0 & rC[1]==3 & reldif(rD[1],0)<1e-12
if (_rc) di as err "FAIL NUM-2: round ties not toward +inf"
local fails = `fails' + (_rc!=0)

* ---- NUM-1 : x/0 and (-8)^0.5 -> missing; save writes NULL, never inf -------
clear
input id v
1 1
2 4
end
parqit save `"`t'_d.parquet"', replace data
parqit use using `"`t'_d.parquet"'
parqit gen z = v/0
parqit gen w = (-8)^0.5
parqit collect, clear
capture assert mi(z[1]) & mi(z[2]) & mi(w[1]) & mi(w[2])
if (_rc) di as err "FAIL NUM-1 collect: div/pow not missing"
local fails = `fails' + (_rc!=0)
parqit use using `"`t'_d.parquet"'
parqit gen z = v/0
parqit save `"`t'_d2.parquet"', replace
python:
import pyarrow.parquet as pq
from sfi import Macro
T = pq.read_table(Macro.getLocal("t")+"_d2.parquet").to_pydict()
Macro.setLocal("ok", "1" if all(x is None for x in T["z"]) else "0")
end
if ("`ok'"!="1") di as err "FAIL NUM-1 save: inf/nan leaked to parquet"
local fails = `fails' + ("`ok'"!="1")

* ---- STR-1 : upper() ASCII-only vs ustrupper() Unicode --------------------
python:
import pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro
b = Macro.getLocal("t")
pq.write_table(pa.table({"id":[1], "s":pa.array(["café"])}), b+"_s.parquet")
end
parqit use using `"`t'_s.parquet"'
parqit gen sasc = upper(s)
parqit gen suni = ustrupper(s)
parqit collect, clear
capture assert sasc[1]=="CAF"+uchar(233) & suni[1]=="CAF"+uchar(201)
if (_rc) di as err "FAIL STR-1: ascii=[`=sasc[1]'] uni=[`=suni[1]']"
local fails = `fails' + (_rc!=0)

* ---- DATE-1 : invalid mdy() is row-local missing, no abort ----------------
clear
input id mm dd yy
1 1 15 2020
2 2 30 2020
3 1 16 2020
end
parqit save `"`t'_dt.parquet"', replace data
parqit use using `"`t'_dt.parquet"'
parqit gen md = mdy(mm, dd, yy)
parqit collect, clear
capture assert _N==3 & !mi(md[1]) & mi(md[2]) & !mi(md[3])
if (_rc) di as err "FAIL DATE-1: invalid mdy aborted or not row-local missing"
local fails = `fails' + (_rc!=0)

* ---- MERGE-1 : uniqueness guard folds ""/NaN to Stata-missing -------------
python:
import pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro
b = Macro.getLocal("t")
pq.write_table(pa.table({"id":pa.array(["a"]), "mv":[1]}), b+"_m1.parquet")
pq.write_table(pa.table({"id":pa.array(["", None]), "uv":[10,20]}), b+"_u1.parquet")
pq.write_table(pa.table({"k":pa.array([1.0]), "mv":[1]}), b+"_m1f.parquet")
pq.write_table(pa.table({"k":pa.array([float("nan"), None]), "uv":[10,20]}), b+"_u1f.parquet")
pq.write_table(pa.table({"id":pa.array(["a","b"]), "mv":[1,2]}), b+"_m2.parquet")
pq.write_table(pa.table({"id":pa.array(["a","b"]), "uv":[10,20]}), b+"_u2.parquet")
end
parqit use using `"`t'_m1.parquet"'
capture noisily parqit merge m:1 id using `"`t'_u1.parquet"', keepusing(uv)
if (_rc==0) di as err "FAIL MERGE-1(str): ''/NULL using keys passed uniqueness"
local fails = `fails' + (_rc==0)
parqit use using `"`t'_m1f.parquet"'
capture noisily parqit merge m:1 k using `"`t'_u1f.parquet"', keepusing(uv)
if (_rc==0) di as err "FAIL MERGE-1(num): NaN/NULL using keys passed uniqueness"
local fails = `fails' + (_rc==0)
parqit use using `"`t'_m2.parquet"'
capture noisily parqit merge 1:1 id using `"`t'_u2.parquet"', keepusing(uv)
local rc_ok = _rc
parqit collect, clear
capture assert `rc_ok'==0 & _N==2
if (_rc) di as err "FAIL MERGE-1 control: unique-key 1:1 merge regressed"
local fails = `fails' + (_rc!=0)

* ---- MERGE-2 : keep(master master) returns MASTER rows --------------------
python:
import pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro
b = Macro.getLocal("t")
pq.write_table(pa.table({"id":[1,2], "mv":[10,20]}), b+"_mas.parquet")
pq.write_table(pa.table({"id":[2,3], "uv":[200,300]}), b+"_usi.parquet")
end
parqit use using `"`t'_mas.parquet"'
parqit merge 1:1 id using `"`t'_usi.parquet"', keep(master master)
parqit collect, clear
sort id
capture assert _N==1 & id[1]==1
if (_rc) di as err "FAIL MERGE-2: keep(master master) did not return master-only"
local fails = `fails' + (_rc!=0)

* ---- RENAME-1 : (oldlist) (newlist) --------------------------------------
clear
input id a b
1 7 8
end
parqit save `"`t'_rn.parquet"', replace data
parqit use using `"`t'_rn.parquet"'
parqit rename (a b) (x y)
parqit collect, clear
capture confirm variable x
local r1 = _rc
capture confirm variable y
local r2 = _rc
if (`r1' | `r2') di as err "FAIL RENAME-1: (a b)(x y) not applied"
local fails = `fails' + ((`r1'|`r2')!=0)

* ---- EGEN-1 : egen fcn(expr) with internal commas ------------------------
clear
input id g x
1 1 5
2 1 -3
3 1 7
end
parqit save `"`t'_eg.parquet"', replace data
parqit use using `"`t'_eg.parquet"'
parqit egen m = mean(cond(x>0, x, .)), by(g)
parqit collect, clear
capture assert reldif(m[1],6) < 1e-9
if (_rc) di as err "FAIL EGEN-1: egen cond() rejected or wrong"
local fails = `fails' + (_rc!=0)

* ---- RESHAPE-5 : bare column == stub errors; control still works ---------
clear
input id inc inc1 inc2
1 5 10 11
end
parqit save `"`t'_r5.parquet"', replace data
parqit use using `"`t'_r5.parquet"'
capture noisily parqit reshape long inc, i(id) j(year)
if (_rc==0) di as err "FAIL RESHAPE-5: bare 'inc' colliding with stub did not error"
local fails = `fails' + (_rc==0)
clear
input id x2 inc1 inc2
1 7 10 11
end
parqit save `"`t'_rc.parquet"', replace data
parqit use using `"`t'_rc.parquet"'
parqit reshape long inc, i(id) j(year)
parqit collect, clear
gsort id year
capture assert _N==2 & inc[1]==10 & inc[2]==11 & x2[1]==7
if (_rc) di as err "FAIL RESHAPE control: normal reshape long regressed"
local fails = `fails' + (_rc!=0)

* ---- TYPE-SAVE-1 : out-of-range %tc save errors loudly -------------------
clear
set obs 2
gen double tc = 1577836800000
replace tc = 1e16 in 2
format tc %tc
capture noisily parqit save `"`t'_tc.parquet"', replace data
if (_rc==0) di as err "FAIL TYPE-SAVE-1: out-of-range %tc saved with rc 0"
local fails = `fails' + (_rc==0)

* ---- IO-1 : save onto the view's own source is refused, source intact ----
python:
import pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro
b = Macro.getLocal("t")
pq.write_table(pa.table({"id":list(range(1,11)), "v":list(range(1,11))}), b+"_io.parquet")
end
parqit use using `"`t'_io.parquet"'
parqit keep if id <= 3
capture noisily parqit save `"`t'_io.parquet"', replace
if (_rc==0) di as err "FAIL IO-1: save over the view's own source was not refused"
local fails = `fails' + (_rc==0)
python:
import pyarrow.parquet as pq
from sfi import Macro
n = pq.read_table(Macro.getLocal("t")+"_io.parquet").num_rows
Macro.setLocal("io_n", str(n))
end
if ("`io_n'"!="10") di as err "FAIL IO-1: source truncated to `io_n' rows (expected 10)"
local fails = `fails' + ("`io_n'"!="10")

* ---- PERF-1 : _n / _N stay correct after window gating -------------------
clear
input id v
1 100
2 200
3 300
end
parqit save `"`t'_p.parquet"', replace data
parqit use using `"`t'_p.parquet"'
parqit sort id
parqit gen seq = _n
parqit gen tot = _N
parqit gen frac = _n / _N
parqit collect, clear
sort id
capture assert seq[1]==1 & seq[3]==3 & tot[1]==3 & reldif(frac[3],1)<1e-12
if (_rc) di as err "FAIL PERF-1: _n/_N results changed"
local fails = `fails' + (_rc!=0)
parqit use using `"`t'_p.parquet"'
parqit sort id
parqit keep if _n <= 2
parqit collect, clear
capture assert _N==2
if (_rc) di as err "FAIL PERF-1: keep if _n<=2 wrong"
local fails = `fails' + (_rc!=0)

* ---- META-1 : long value label / characteristic survive the read ---------
* (a value-label text > ~16 KB used to overflow the 32768-byte fget line cap
*  and either truncate silently or abort the load with r(3300))
local longtext "ABCDEF"
forvalues i = 1/13 {
    local longtext "`longtext'`longtext'"
}
local longtext = substr(`"`longtext'"', 1, 30000)
local n0 = strlen(`"`longtext'"')
clear
set obs 3
gen x = _n
label define mylab 1 `"`longtext'"' 2 "two" 3 "three"
label values x mylab
char _dta[bignote] `"`longtext'"'
parqit save `"`t'_meta.parquet"', replace data
parqit use using `"`t'_meta.parquet"'
parqit collect, clear
local lt : label mylab 1
local c1 : char _dta[bignote]
capture assert strlen(`"`lt'"')==`n0' & substr(`"`lt'"',-6,6)=="ABCDEF" & strlen(`"`c1'"')==`n0'
if (_rc) di as err "FAIL META-1: long label/char truncated (lab=`=strlen(`"`lt'"')' char=`=strlen(`"`c1'"')' want `n0')"
local fails = `fails' + (_rc!=0)

* ---- TT-3 : generated _merge carries Stata's standard value labels --------
python:
import pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro
b = Macro.getLocal("t")
pq.write_table(pa.table({"id":[1,2], "mv":[10,20]}), b+"_tm.parquet")
pq.write_table(pa.table({"id":[2,3], "uv":[200,300]}), b+"_tu.parquet")
end
parqit use using `"`t'_tm.parquet"'
parqit merge 1:1 id using `"`t'_tu.parquet"', keepusing(uv)
parqit collect, clear
local lab1 : label (_merge) 1
local lab3 : label (_merge) 3
capture assert "`lab1'"=="Master only (1)" & "`lab3'"=="Matched (3)" & _merge[1]>=1
if (_rc) di as err "FAIL TT-3: _merge value labels missing (1=[`lab1'] 3=[`lab3'])"
local fails = `fails' + (_rc!=0)

di as txt "VERDICT(V28_AUDIT_FIXES_R2): " cond(`fails'==0, "PASS", "FAIL — `fails' failures") ///
    " - round/div/pow/upper/mdy/merge-unique/keep/rename/egen/reshape/%tc/save-source/_n/meta-long/_merge-labels"
