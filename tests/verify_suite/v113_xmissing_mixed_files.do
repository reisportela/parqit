* CA-04: mixed value metadata must not silently downgrade extended missings.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
tempfile base
local fails 0
input long id double x
1 .a
2 10
end
parqit save `"`base'_mixed1.parquet"', data xmissing
clear
input long id double x
3 .
4 20
end
parqit save `"`base'_mixed2.parquet"', data xmissing
python:
from sfi import Macro
import pyarrow as pa
import pyarrow.parquet as pq
base = Macro.getLocal("base")
a = pq.read_table(base + "_mixed1.parquet")
b = pq.read_table(base + "_mixed2.parquet")
assert a.column("_parqit_xm_x").to_pylist() == [1,0]
assert b.column("x").to_pylist() == [None,20.]
pq.write_table(a, base + "_same1.parquet")
c = a.set_column(0, "id", pa.array([3,4],pa.int32()))
pq.write_table(c, base + "_same2.parquet")
pq.write_table(a, base + "_labels1.parquet")
md = dict(c.schema.metadata)
md[b"parqit.dtalabel"] = b'"different label"'
pq.write_table(c.replace_schema_metadata(md), base + "_labels2.parquet")
end
foreach path in eager describe lazy {
    clear
    set obs 1
    gen sentinel = 987
    quietly datasignature
    local before `"`r(datasignature)'"'
    parqit sql "SELECT 42 AS marker"
    if ("`path'" == "eager") capture noisily parqit use `"`base'_mixed*.parquet"', clear relaxed
    if ("`path'" == "describe") capture noisily parqit describe `"`base'_labels*.parquet"'
    if ("`path'" == "lazy") capture noisily parqit use using `"`base'_mixed*.parquet"', relaxed
    local rc = _rc
    di "mixed companions / `path': rc=`rc'"
    if (`rc' != 198) local ++fails
    quietly datasignature
    if (`"`r(datasignature)'"' != `"`before'"') local ++fails
    parqit count
    if (r(N) != 1) local ++fails
    parqit close _all
}
parqit use `"`base'_same*.parquet"', clear relaxed
sort id
assert _N == 4 & c(k) == 2 & x[1] == .a & x[3] == .a
parqit use `"`base'_mixed1.parquet"', clear
parqit appendin using `"`base'_mixed2.parquet"'
assert _N == 4 & x[1] == .a & x[3] == . & x[4] == 20
if (`fails') di "VERDICT(V113_XMISSING_MIXED_FILES): FAIL - `fails' checks"
else di "VERDICT(V113_XMISSING_MIXED_FILES): PASS"
