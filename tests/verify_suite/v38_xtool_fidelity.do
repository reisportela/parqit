* V38 — cross-tool data-integrity (absolute fidelity, both directions).
* Independent oracles only: pyarrow writes/reads the foreign side; native Stata
* is its own oracle. Locks the guarantee that a file produced by a FOREIGN tool
* (DuckDB/Arrow/pyarrow) lands in Stata with exact fidelity, and that Stata data
* saved to Parquet is exactly faithful to what Stata held — with the only losses
* being Stata's own limits (no int64 type; one missing concept), all announced.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
local fails 0
tempfile t

* ===================== A. foreign Parquet -> Stata (read) =====================
python:
import pyarrow as pa, pyarrow.parquet as pq, struct, decimal, datetime
from sfi import Macro
b = Macro.getLocal("t")
pq.write_table(pa.table({
  "i8":  pa.array([-128, 127, None], type=pa.int8()),
  "i32": pa.array([-2147483648, 2147483647, None], type=pa.int32()),
  "i64": pa.array([-9223372036854775808, 9007199254740993, None], type=pa.int64()),
  "u32": pa.array([0, 4294967295, None], type=pa.uint32()),
  "f32": pa.array([0.1, float("nan"), None], type=pa.float32()),
  "f64": pa.array([0.1, 8.9e307, None], type=pa.float64()),
  "bol": pa.array([True, False, None], type=pa.bool_()),
  "s":   pa.array(["café\U0001f986", "", None], type=pa.string()),
  "dec": pa.array([decimal.Decimal("123.4567890123"), None, None], type=pa.decimal128(20,10)),
}), b + "_fr.parquet")
pq.write_table(pa.table({
  "tus": pa.array([datetime.datetime(2020,6,15,12,30,45,123456), None], type=pa.timestamp("us")),
  "dt":  pa.array([datetime.date(2020,6,15), datetime.date(1959,12,31)], type=pa.date32()),
}), b + "_ts.parquet")
Macro.setLocal("f32_01", repr(struct.unpack("f", struct.pack("f", 0.1))[0]))
Macro.setLocal("i64d", repr(float(9007199254740993)))
end

parqit use using `"`t'_fr.parquet"', clear
capture assert i8[1]==-128 & i8[2]==127 & missing(i8[3])
if (_rc) di as err "FAIL A int8 extremes"
local fails=`fails'+(_rc!=0)
capture assert i32[1]==-2147483648 & i32[2]==2147483647 & missing(i32[3])
if (_rc) di as err "FAIL A int32 extremes"
local fails=`fails'+(_rc!=0)
capture assert i64[1]==-9223372036854775808 & i64[2]==`i64d'
if (_rc) di as err "FAIL A int64 -> nearest double"
local fails=`fails'+(_rc!=0)
capture assert u32[2]==4294967295
if (_rc) di as err "FAIL A uint32"
local fails=`fails'+(_rc!=0)
capture assert float(f32[1])==float(`f32_01') & missing(f32[2]) & missing(f32[3])
if (_rc) di as err "FAIL A float32 exact / NaN->missing"
local fails=`fails'+(_rc!=0)
capture assert f64[1]==0.1 & f64[2]==8.9e307 & !missing(f64[2]) & missing(f64[3])
if (_rc) di as err "FAIL A float64 exact / below-missval stays value"
local fails=`fails'+(_rc!=0)
capture assert bol[1]==1 & bol[2]==0 & missing(bol[3])
if (_rc) di as err "FAIL A bool"
local fails=`fails'+(_rc!=0)
capture assert strlen(s[1])==9 & ustrlen(s[1])==5 & s[2]=="" & s[3]==""
if (_rc) di as err "FAIL A string utf8/empty/null"
local fails=`fails'+(_rc!=0)
capture assert reldif(dec[1], 123.4567890123) < 1e-12
if (_rc) di as err "FAIL A decimal->double"
local fails=`fails'+(_rc!=0)

parqit use using `"`t'_ts.parquet"', clear
capture assert tus[1]==tc(15jun2020 12:30:45)+123 & missing(tus[2])
if (_rc) di as err "FAIL A timestamp us->ms (sub-ms truncates)"
local fails=`fails'+(_rc!=0)
capture assert dt[1]==td(15jun2020) & dt[2]==td(31dec1959)
if (_rc) di as err "FAIL A date32 (incl. pre-1960 negative)"
local fails=`fails'+(_rc!=0)

* ===================== B. Stata -> Parquet (write) ===========================
clear
set obs 4
gen byte   b1 = .
gen int    i1 = .
gen long   l1 = .
gen float  f1 = .
gen double d1 = .
gen str10  s1 = ""
gen strL   sl = ""
gen double td_v = .
gen double tc_v = .
gen double tm_v = .
format td_v %td
format tc_v %tc
format tm_v %tm
replace b1=100 in 1
replace i1=32740 in 1
replace l1=2147483620 in 1
replace f1=1/3 in 1
replace d1=1/3 in 1
replace s1="café🦆" in 1
replace sl="L" + "x"*5000 in 1
replace td_v=td(15jun2020) in 1
replace tc_v=tc(15jun2020 12:30:45) in 1
replace tm_v=tm(2020m6) in 1
replace b1=-127 in 2
replace d1=8.98e307 in 2
replace b1=.a in 4
replace d1=.z in 4
parqit save `"`t'_sw.parquet"', replace data

python:
import pyarrow.parquet as pq, struct, datetime
from sfi import Macro, Scalar
b=Macro.getLocal("t"); T=pq.read_table(b+"_sw.parquet"); s=T.schema
def c(n): return T[n].to_pylist()
f=0
def ck(x,m):
    global f
    if not x: print("BWFAIL",m); f+=1
ck(c("b1")==[100,-127,None,None], "byte")
ck(c("i1")[0]==32740, "int")
ck(c("l1")[0]==2147483620, "long")
ck(abs(c("f1")[0]-struct.unpack("f",struct.pack("f",1.0/3.0))[0])==0.0 and c("f1")[2] is None, "float32 exact+null")
ck("float" in str(s.field("f1").type), "f1 not float32: "+str(s.field("f1").type))
ck(c("d1")[0]==1.0/3.0 and c("d1")[1]==8.98e307 and c("d1")[2] is None and c("d1")[3] is None, "double exact+missing+extended")
ck(str(s.field("d1").type)=="double", "d1 not double")
ck(c("s1")[0]=="café🦆" and c("s1")[1]=="", "string utf8/empty")
ck(c("sl")[0]=="L"+"x"*5000, "strL")
ck(c("td_v")[0]==datetime.date(2020,6,15), "td->DATE")
ck(c("tc_v")[0]==datetime.datetime(2020,6,15,12,30,45), "tc->TIMESTAMP")
ck(c("tm_v")[0]==(2020-1960)*12+5, "tm->INTEGER period count")
Scalar.setValue("bwf", f)
end
capture assert bwf==0
if (_rc) di as err "FAIL B Stata->parquet (`=bwf' mismatches)"
local fails=`fails'+(_rc!=0)

* ===================== C. lazy-save round trip ===============================
parqit use using `"`t'_fr.parquet"'
parqit save `"`t'_fr_out.parquet"', replace
python:
import pyarrow.parquet as pq
from sfi import Macro, Scalar
b=Macro.getLocal("t"); B=pq.read_table(b+"_fr_out.parquet")
f=0
if B["f64"].to_pylist()!=[0.1, 8.9e307, None]: f+=1
if B["s"].to_pylist()!=["café🦆","",""]: f+=1   # NULL and "" both fold to ""
Scalar.setValue("lzf", f)
end
capture assert lzf==0
if (_rc) di as err "FAIL C lazy-save round trip"
local fails=`fails'+(_rc!=0)

* ===================== D. metadata round trip ================================
clear
set obs 3
gen byte grp = _n
label define GL 1 "one" 2 "two" 3 "three"
label values grp GL
label variable grp "the group var"
gen double pay = _n*1.5
format pay %9.2f
notes pay : note with accents café
char grp[myattr] "custom-char-value"
char _dta[source] "synthetic"
label data "my dataset label"
parqit save `"`t'_m.parquet"', replace data
parqit use using `"`t'_m.parquet"', clear
capture assert "`: label GL 2'"=="two" & "`: value label grp'"=="GL"
if (_rc) di as err "FAIL D value label"
local fails=`fails'+(_rc!=0)
capture assert "`: variable label grp'"=="the group var" & "`: format pay'"=="%9.2f"
if (_rc) di as err "FAIL D var label / format"
local fails=`fails'+(_rc!=0)
capture assert `"`: char pay[note1]'"'=="note with accents café"
if (_rc) di as err "FAIL D notes"
local fails=`fails'+(_rc!=0)
capture assert `"`: char grp[myattr]'"'=="custom-char-value" & `"`: char _dta[source]'"'=="synthetic"
if (_rc) di as err "FAIL D characteristics"
local fails=`fails'+(_rc!=0)
capture assert `"`: data label'"'=="my dataset label"
if (_rc) di as err "FAIL D dataset label"
local fails=`fails'+(_rc!=0)

* ===================== E. losses are LOUD, never silent ======================
* invalid UTF-8 on write -> refused
clear
set obs 1
gen str5 sb = char(200)+char(255)+"x"
capture noisily parqit save `"`t'_bad.parquet"', replace data
capture assert _rc!=0
if (_rc) di as err "FAIL E invalid UTF-8 not refused"
local fails=`fails'+(_rc!=0)
* DT-001: a %tc value at the int64-microsecond ceiling must error loudly, never
* silently write INT64_MIN (the guard literal used to round up one ulp)
clear
set obs 1
gen double tc_x = tc(01jan1960 00:00:00) + 9223687656054776
format tc_x %tc
capture noisily parqit save `"`t'_dtx.parquet"', replace data
capture assert _rc!=0
if (_rc) di as err "FAIL E DT-001: extreme %tc not rejected (silent INT64_MIN)"
local fails=`fails'+(_rc!=0)
* control: a normal %tc (incl. fractional ms and pre-1960) saves and round-trips
clear
set obs 2
gen double tcn = tc(15jun2020 12:30:45.123) in 1
replace tcn = tc(31dec1959 23:59:59) in 2
format tcn %tc
parqit save `"`t'_dtn.parquet"', replace data
parqit use using `"`t'_dtn.parquet"', clear
capture assert tcn[1]==tc(15jun2020 12:30:45.123) & tcn[2]==tc(31dec1959 23:59:59)
if (_rc) di as err "FAIL E normal %tc round-trip after DT-001 fix"
local fails=`fails'+(_rc!=0)

di as txt "VERDICT(V38_XTOOL_FIDELITY): " cond(`fails'==0,"PASS","FAIL — `fails' failures") ///
    " - foreign-read-exact / stata-write-exact / lazy-save-roundtrip / metadata-roundtrip / losses-loud"
