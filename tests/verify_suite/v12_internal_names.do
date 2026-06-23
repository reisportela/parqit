* CHARTER 12 (pq finding 12): user columns named like internal helpers must
* never be clobbered or clobber the machinery. parqit generates helper names
* and checks them against the live schema, so even columns called
* __parqit_rn_1 / _merge-lookalikes / _freq survive every pipeline.
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

tempfile fbase
local f `"`fbase'.parquet"'

python:
from sfi import Macro
import pyarrow as pa, pyarrow.parquet as pq
t = pa.table({
    "id": pa.array([1, 1, 2, 2, 3], pa.int32()),
    "__parqit_rn_1": pa.array([10, 20, 30, 40, 50], pa.int32()),
    "__parqit_mm_2": pa.array(["a", "b", "c", "d", "e"]),
    "x": pa.array([1.0, 2.0, 3.0, 4.0, 5.0]),
})
pq.write_table(t, Macro.getLocal("f"))
end

* pipeline exercising every helper-generating path: _n windows, dupdrop,
* collapse first/last — with hostile column names present throughout
parqit use using `"`f'"'
parqit sort id __parqit_rn_1
parqit gen rowno = _n
parqit duplicates drop id, force
parqit collect, clear
assert _N == 3
assert __parqit_rn_1[1] == 10 & __parqit_rn_1[3] == 50
assert __parqit_mm_2[1] == "a"
assert rowno[1] == 1

clear
parqit use using `"`f'"'
parqit sort __parqit_rn_1
parqit collapse (first) fx = x (last) lx = x, by(id)
parqit collect, clear
assert _N == 3
assert fx[1] == 1 & lx[1] == 2

* merge with a user column named exactly _merge: loud, not clobbered
clear
parqit use using `"`f'"'
parqit rename x _merge
capture parqit merge m:1 id using `"`f'"'
assert _rc != 0
parqit close

di "VERDICT(V12_INTERNAL_NAMES): PASS - hostile helper-named columns survive; _merge collision is loud"
