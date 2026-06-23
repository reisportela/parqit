* CHARTER 9 (pq finding 9): parqit use ..., clear must never destroy the
* in-memory dataset unless the load fully succeeds. pq's signature: a typo'd
* filename left _N==0 after rc 601.
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

* precious unsaved data
set obs 5
gen x = _n
gen s = "keep me " + string(_n)
datasignature set, reset
local sig `"`r(datasignature)'"'

* 1. nonexistent file
capture parqit use using "/no/such/file_parqit.parquet", clear
assert _rc != 0
datasignature confirm
assert _N == 5 & x[5] == 5 & s[1] == "keep me 1"

* 2. corrupt parquet (garbage bytes)
tempfile gbase
local garbage `"`gbase'.parquet"'
python:
from sfi import Macro
open(Macro.getLocal("garbage"), "wb").write(b"PAR1 this is not really parquet")
end
capture parqit use using `"`garbage'"', clear
assert _rc != 0
datasignature confirm
assert _N == 5

* 3. file whose only columns are unsupported (every column dropped → error,
*    memory untouched)
tempfile ubase
local unsup `"`ubase'.parquet"'
python:
from sfi import Macro
import pyarrow as pa, pyarrow.parquet as pq
t = pa.table({"iv": pa.array([(1, 2)], pa.list_(pa.int32()))})
pq.write_table(t, Macro.getLocal("unsup"))
end
capture parqit use using `"`unsup'"', clear
assert _rc != 0
datasignature confirm
assert _N == 5

* 4. without clear, parqit use opens a LAZY VIEW — it never touches memory;
*    over a corrupt file it errors loudly and memory stays intact
capture parqit use using `"`garbage'"'
assert _rc != 0
datasignature confirm
assert _N == 5

di "VERDICT(V09_ATOMIC_CLEAR): PASS - failed loads never destroy the in-memory dataset"
