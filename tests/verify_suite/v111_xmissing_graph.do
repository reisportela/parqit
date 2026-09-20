* CA-02: invalid companion graphs refuse before memory or a view is replaced.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
tempfile base
local fails 0
python:
import json
import pyarrow as pa
import pyarrow.parquet as pq
from sfi import Macro
base = Macro.getLocal("base")
cases = {
    "self": ({"id": [1,2], "x": [10,20]}, {"x":"x"}),
    "cycle": ({"id": [1,2], "x": [10,20], "y": [30,40]}, {"x":"y", "y":"x"}),
    "chain": ({"id": [1,2], "x": [None,None], "y": [1,2], "codes": [0,0]}, {"x":"y", "y":"codes"}),
    "orphan": ({"id": [1,2], "_parqit_xm_x": [1,26]}, {"x":"_parqit_xm_x"}),
}
for name, (cols, mapping) in cases.items():
    table = pa.table({k:pa.array(v, pa.int32()) for k,v in cols.items()})
    table = table.replace_schema_metadata({b"parqit.xmissing":json.dumps(mapping).encode()})
    pq.write_table(table, base + "_" + name + ".parquet")
    assert pq.read_table(base + "_" + name + ".parquet").to_pydict() == cols
end
foreach case in self cycle chain {
    foreach path in eager describe lazy append {
        clear
        set obs 1
        gen sentinel = 987
        quietly datasignature
        local before `"`r(datasignature)'"'
        parqit sql "SELECT 42 AS marker"
        if ("`path'" == "eager") capture noisily parqit use `"`base'_`case'.parquet"', clear
        if ("`path'" == "describe") capture noisily parqit describe `"`base'_`case'.parquet"'
        if ("`path'" == "lazy") capture noisily parqit use using `"`base'_`case'.parquet"'
        if ("`path'" == "append") capture noisily parqit append using `"`base'_`case'.parquet"'
        local rc = _rc
        di "`case' / `path': rc=`rc'"
        if (`rc' != 198) local ++fails
        quietly datasignature
        if (`"`r(datasignature)'"' != `"`before'"') local ++fails
        parqit count
        if (r(N) != 1) local ++fails
        parqit close _all
    }
}
parqit use `"`base'_orphan.parquet"', clear
assert _N == 2 & c(k) == 1 & id == _n
if (`fails') di "VERDICT(V111_XMISSING_GRAPH): FAIL - `fails' checks"
else di "VERDICT(V111_XMISSING_GRAPH): PASS"
