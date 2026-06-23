* ADVERSARIAL: width. 2500+ variables stress the column manifest, the
* request/response files, the plugin-call varlist (argv) and Stata macro
* limits. Load, project, compute, collect and save must all survive with
* names and positions intact (charter: never index positionally by
* accident — spot-check first/middle/last columns).
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

local fails 0
local K 2500
tempfile fbase
local f `"`fbase'.parquet"'
local kk "`K'"

python:
from sfi import Macro
import pyarrow as pa, pyarrow.parquet as pq
K = int(Macro.getLocal("kk"))
cols = {f"v{i:04d}": pa.array([float(i), float(i) + 0.5, None]) for i in range(1, K + 1)}
pq.write_table(pa.table(cols), Macro.getLocal("f"))
end

* ---------- full materialise: 2500 vars through prepare/fetch --------------
parqit use using `"`f'"', clear
if (_N != 3 | c(k) != `K') {
    di as err "FAIL: wide load shape (" _N "x" c(k) ")"
    local ++fails
}
* spot-check positions: names must bind to their own payloads
assert v0001[1] == 1
assert v1250[1] == 1250 & v1250[2] == 1250.5
assert v2500[1] == 2500 & missing(v2500[3])

* ---------- lazy: project a sparse subset out of 2500 ----------------------
parqit use using `"`f'"'
parqit keep v0007 v1250 v2499
parqit gen double s = v0007 + v2499
parqit collect, clear
if (c(k) != 4 | reldif(s[2], 7.5 + 2499.5) > 1e-12) {
    di as err "FAIL: sparse projection over 2500 columns"
    local ++fails
}
parqit close _all

* ---------- save the wide table back and verify the far edge ---------------
parqit use using `"`f'"', clear
tempfile obase
local o `"`obase'.parquet"'
qui parqit save `"`o'"', replace
python:
from sfi import Macro, Scalar
import pyarrow.parquet as pq
t = pq.read_table(Macro.getLocal("o"))
ok = 1
if t.num_columns != int(Macro.getLocal("kk")): ok = 0
if t.column("v2500").to_pylist()[:2] != [2500.0, 2500.5]: ok = 0
if t.column("v0001").to_pylist()[0] != 1.0: ok = 0
Scalar.setValue("pyok", ok)
end
if (scalar(pyok) != 1) {
    di as err "FAIL: wide round-trip lost columns or payloads"
    local ++fails
}

if (`fails' == 0) di "VERDICT(V18_WIDE_2500_VARS): PASS - 2500-var manifest survives load/project/collect/save with positions intact"
else {
    di as err "VERDICT(V18_WIDE_2500_VARS): FAIL - `fails' check(s)"
    exit 9
}
