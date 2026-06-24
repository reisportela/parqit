* V37 — fourth adversarial-audit round (2026-06-23). Independent oracles only
* (native Stata twin and/or pyarrow read-back); never a parqit-only round trip.
*
*   PQ-AUD-001  lazy FLOAT/DOUBLE columns normalize NaN/+-Inf/out-of-range to
*               Stata missing at the boundary (and on save), matching the eager
*               `use, clear` fill; generated specials (exp(10000)) too.
*   PQ-AUD-002  lazy string columns fold a Parquet NULL to "" at the boundary
*               (and on save), so order / _n / keep-in / dedup match native.
*   PQ-AUD-004  egen with an explicit narrow numeric type enforces value
*               semantics (out-of-range -> missing), and a string storage type
*               is a loud type error, not metadata-only.
*   PQ-AUD-005  gen with a storage type whose family disagrees with the
*               expression is a loud type mismatch, not silently accepted.
*   PQ-AUD-006  duplicates drop with no varlist folds NULL-vs-"" and NaN-vs-NULL
*               so they collapse like native Stata (resolved by the 001/002
*               boundary normalization; SELECT DISTINCT stays, no extra cost).
*   PQ-AUD-007  td()/tc()/tC() reject impossible calendar dates and a 60th
*               second loudly (native r(198)), never rolling them forward.
*   PQ-AUD-003  REJECTED as a false positive and pinned by a regression guard:
*               native Stata `replace` AUTO-PROMOTES storage to fit the value
*               (byte b=200 -> 200/int; str3 s="abcdef" -> "abcdef"/str6), it
*               does NOT truncate/null into the old narrow type. parqit already
*               matches; this test fails if a future change "fixes" it wrongly.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

local fails 0
tempfile t

* ============ PQ-AUD-001 : lazy float specials -> Stata missing =============
python:
import pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro
b = Macro.getLocal("t")
pq.write_table(pa.table({
    "id": pa.array([1,2,3,4], type=pa.int32()),
    "x":  pa.array([1.0, float("nan"), float("inf"), None], type=pa.float64())
}), b + "_fs.parquet")
end

* eager load is the oracle: it maps NaN/Inf/None to Stata missing
parqit use using `"`t'_fs.parquet"', clear
gen byte m_eager = missing(x)
sort id
capture assert m_eager[1]==0 & m_eager[2]==1 & m_eager[3]==1 & m_eager[4]==1
if (_rc) di as err "FAIL PQ-AUD-001 oracle: eager missing(x) over NaN/Inf/None unexpected"
local fails = `fails' + (_rc!=0)

* lazy must match the eager oracle
parqit use using `"`t'_fs.parquet"'
parqit gen m_lazy = missing(x)
parqit collect, clear
sort id
capture assert m_lazy[1]==0 & m_lazy[2]==1 & m_lazy[3]==1 & m_lazy[4]==1
if (_rc) di as err "FAIL PQ-AUD-001: lazy missing(x) did not treat NaN/Inf as Stata missing"
local fails = `fails' + (_rc!=0)

* lazy save must not write IEEE specials (independent pyarrow oracle)
parqit use using `"`t'_fs.parquet"'
parqit save `"`t'_fs_out.parquet"', replace
python:
import pyarrow.parquet as pq, math
from sfi import Macro
b = Macro.getLocal("t")
vals = pq.read_table(b + "_fs_out.parquet")["x"].to_pylist()
bad = any((isinstance(v,float) and (math.isnan(v) or math.isinf(v))) for v in vals if v is not None)
Macro.setLocal("ieee", "1" if bad else "0")
end
capture assert `ieee' == 0
if (_rc) di as err "FAIL PQ-AUD-001: lazy save preserved a NaN/Inf on disk"
local fails = `fails' + (_rc!=0)

* generated special: exp(10000) -> +Inf must read as missing and save as NULL
clear
set obs 1
gen one = 1
parqit save `"`t'_one.parquet"', replace data
parqit use using `"`t'_one.parquet"'
parqit gen double z = exp(10000)
parqit gen byte mz = missing(z)
parqit collect, clear
capture assert mz[1]==1 & missing(z[1])
if (_rc) di as err "FAIL PQ-AUD-001: missing(exp(10000)) not reported as missing"
local fails = `fails' + (_rc!=0)
parqit use using `"`t'_one.parquet"'
parqit gen double z = exp(10000)
parqit save `"`t'_z_out.parquet"', replace
python:
import pyarrow.parquet as pq, math
from sfi import Macro
b = Macro.getLocal("t")
vals = pq.read_table(b + "_z_out.parquet")["z"].to_pylist()
bad = any((isinstance(v,float) and (math.isnan(v) or math.isinf(v))) for v in vals if v is not None)
Macro.setLocal("ieeez", "1" if bad else "0")
end
capture assert `ieeez' == 0
if (_rc) di as err "FAIL PQ-AUD-001: lazy save preserved a generated +Inf on disk"
local fails = `fails' + (_rc!=0)

* ============ PQ-AUD-002 : lazy string NULL -> "" =============
python:
import pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro
b = Macro.getLocal("t")
pq.write_table(pa.table({
    "id": pa.array([1,2,3], type=pa.int32()),
    "s":  pa.array([None, "a", ""], type=pa.string())
}), b + "_sn.parquet")
end
* native/eager oracle: NULL and "" are both "", so sort s id -> 1,3,2
parqit use using `"`t'_sn.parquet"', clear
sort s id
capture assert id[1]==1 & id[2]==3 & id[3]==2
if (_rc) di as err "FAIL PQ-AUD-002 oracle: eager string NULL/order unexpected"
local fails = `fails' + (_rc!=0)
* lazy must match
parqit use using `"`t'_sn.parquet"'
parqit sort s id
parqit collect, clear
capture assert id[1]==1 & id[2]==3 & id[3]==2
if (_rc) di as err "FAIL PQ-AUD-002: lazy sort order over string NULL wrong"
local fails = `fails' + (_rc!=0)
* keep in 1/1 after the sort must keep id 1 (a "" row)
parqit use using `"`t'_sn.parquet"'
parqit sort s id
parqit keep in 1/1
parqit collect, clear
capture assert _N==1 & id[1]==1
if (_rc) di as err "FAIL PQ-AUD-002: keep in 1/1 after string sort kept the wrong row"
local fails = `fails' + (_rc!=0)
* lazy save must not write a string NULL
parqit use using `"`t'_sn.parquet"'
parqit save `"`t'_sn_out.parquet"', replace
python:
import pyarrow.parquet as pq
from sfi import Macro
b = Macro.getLocal("t")
vals = pq.read_table(b + "_sn_out.parquet")["s"].to_pylist()
Macro.setLocal("hasnull", "1" if any(v is None for v in vals) else "0")
end
capture assert `hasnull' == 0
if (_rc) di as err "FAIL PQ-AUD-002: lazy save preserved a string NULL on disk"
local fails = `fails' + (_rc!=0)

* ============ PQ-AUD-003 GUARD : replace AUTO-PROMOTES (must NOT regress) =====
clear
input byte b str3 s int i
1 "zz" 1
end
parqit save `"`t'_typed.parquet"', replace data
* native twin: replace promotes storage to fit
clear
input byte b str3 s int i
1 "zz" 1
end
replace b = 200
replace s = "abcdef"
replace i = 40000
tempfile nat
save `"`nat'"', replace
capture assert b[1]==200 & s[1]=="abcdef" & i[1]==40000
if (_rc) di as err "FAIL PQ-AUD-003 oracle: native replace did not promote as expected"
local fails = `fails' + (_rc!=0)
* lazy parqit must match native promotion (NOT truncate/null to the old type)
parqit use using `"`t'_typed.parquet"'
parqit replace b = 200
parqit replace s = "abcdef"
parqit replace i = 40000
parqit collect, clear
capture assert b[1]==200 & s[1]=="abcdef" & i[1]==40000 & !missing(b[1])
if (_rc) di as err "FAIL PQ-AUD-003 GUARD: replace wrongly truncated/nulled (PQ-AUD-003 must stay rejected)"
local fails = `fails' + (_rc!=0)

* ============ PQ-AUD-004 : egen explicit storage = value semantics ===========
clear
input double x
100
100
end
parqit save `"`t'_e.parquet"', replace data
* native: egen byte total(100,100)=200 -> missing (byte cannot hold 200)
clear
input double x
100
100
end
egen byte t_nat = total(x)
capture assert missing(t_nat[1]) & missing(t_nat[2])
if (_rc) di as err "FAIL PQ-AUD-004 oracle: native egen byte total not missing"
local fails = `fails' + (_rc!=0)
parqit use using `"`t'_e.parquet"'
parqit egen byte tb = total(x)
parqit collect, clear
capture assert missing(tb[1]) & missing(tb[2])
if (_rc) di as err "FAIL PQ-AUD-004: parqit egen byte total did not enforce byte range"
local fails = `fails' + (_rc!=0)
* a string storage type on a numeric egen is a loud error
parqit use using `"`t'_e.parquet"'
capture noisily parqit egen str3 ss = total(x)
capture assert _rc != 0
if (_rc) di as err "FAIL PQ-AUD-004: parqit accepted egen str3 = total(x)"
local fails = `fails' + (_rc!=0)
* control: in-range narrow egen still works (egen long total over 1,2,3 = 6)
clear
input double x
1
2
3
end
parqit save `"`t'_e2.parquet"', replace data
parqit use using `"`t'_e2.parquet"'
parqit egen long sm = total(x)
parqit collect, clear
capture assert sm[1]==6 & "`: type sm'"=="long"
if (_rc) di as err "FAIL PQ-AUD-004 control: in-range egen long total wrong"
local fails = `fails' + (_rc!=0)

* ============ PQ-AUD-005 : gen storage/expression family mismatch is loud =====
parqit use using `"`t'_one.parquet"'
capture noisily parqit gen str3 s5 = 123
capture assert _rc != 0
if (_rc) di as err "FAIL PQ-AUD-005: parqit accepted gen str3 = 123"
local fails = `fails' + (_rc!=0)
parqit use using `"`t'_one.parquet"'
capture noisily parqit gen byte b5 = "abc"
capture assert _rc != 0
if (_rc) di as err "FAIL PQ-AUD-005: parqit accepted gen byte = (string)"
local fails = `fails' + (_rc!=0)
* controls: a matching family still works (string->str#, numeric->double)
parqit use using `"`t'_one.parquet"'
parqit gen str3 sok = "abcdef"
parqit gen double dok = 123
parqit collect, clear
capture assert sok[1]=="abc" & dok[1]==123
if (_rc) di as err "FAIL PQ-AUD-005 control: valid gen str3/double broke"
local fails = `fails' + (_rc!=0)

* ============ PQ-AUD-006 : duplicates drop no-varlist folds missing ==========
python:
import pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro
b = Macro.getLocal("t")
pq.write_table(pa.table({"s": pa.array([None, ""], type=pa.string()),
                         "x": pa.array([1, 1], type=pa.int32())}), b + "_dups.parquet")
pq.write_table(pa.table({"g": pa.array([float("nan"), None], type=pa.float64()),
                         "x": pa.array([1, 1], type=pa.int32())}), b + "_dupn.parquet")
end
parqit use using `"`t'_dups.parquet"'
parqit duplicates drop
parqit collect, clear
capture assert _N == 1
if (_rc) di as err "FAIL PQ-AUD-006: NULL-vs-empty-string rows not deduped (_N=`=_N')"
local fails = `fails' + (_rc!=0)
parqit use using `"`t'_dupn.parquet"'
parqit duplicates drop
parqit collect, clear
capture assert _N == 1
if (_rc) di as err "FAIL PQ-AUD-006: NaN-vs-NULL rows not deduped (_N=`=_N')"
local fails = `fails' + (_rc!=0)

* ============ PQ-AUD-007 : impossible date/time literals are loud ============
parqit use using `"`t'_one.parquet"'
capture noisily parqit gen d1 = td(31feb2020)
capture assert _rc != 0
if (_rc) di as err "FAIL PQ-AUD-007: parqit accepted td(31feb2020)"
local fails = `fails' + (_rc!=0)
parqit use using `"`t'_one.parquet"'
capture noisily parqit gen d2 = td(29feb2019)
capture assert _rc != 0
if (_rc) di as err "FAIL PQ-AUD-007: parqit accepted td(29feb2019) (not a leap year)"
local fails = `fails' + (_rc!=0)
parqit use using `"`t'_one.parquet"'
capture noisily parqit gen double c1 = tc(01jan2020 00:00:60)
capture assert _rc != 0
if (_rc) di as err "FAIL PQ-AUD-007: parqit accepted tc(...:60)"
local fails = `fails' + (_rc!=0)
* controls: valid dates/times still compile and match native day/ms counts
parqit use using `"`t'_one.parquet"'
parqit gen double dleap = td(29feb2020)
parqit gen double cgood = tc(01jan2020 23:59:59)
parqit collect, clear
capture assert dleap[1]==td(29feb2020) & cgood[1]==tc(01jan2020 23:59:59)
if (_rc) di as err "FAIL PQ-AUD-007 control: a valid date/time literal broke"
local fails = `fails' + (_rc!=0)

di as txt "VERDICT(V37_AUDIT_FIXES_20260623D): " cond(`fails'==0, "PASS", "FAIL — `fails' failures") ///
    " - lazy-float-specials/lazy-string-null/egen-storage/gen-typefamily/dup-missing-fold/date-literal-validation/replace-promotion-guard"
