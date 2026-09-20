* CA-03: all 26 codes survive a lossless BIGINT text read; corrupt codes refuse.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
tempfile base
local fails 0
python:
import os, json
import pyarrow as pa
import pyarrow.parquet as pq
from sfi import Macro
base = Macro.getLocal("base")
values = [9007199254740993] + [None]*27
codes = [0] + list(range(1,27)) + [0]
schema = {"version":1,"sortedby":[],"vars":[{"name":"x","src":"x","type":"double","fmt":"%21.0g"}]}
for name in ["valid", "high", "negative", "onvalue"]:
    xc = list(codes)
    if name == "high": xc[1] = 27
    if name == "negative": xc[1] = -1
    if name == "onvalue": xc[0] = 1
    table = pa.table({"id":pa.array(range(1,29),pa.int32()), "x":pa.array(values,pa.int64()),
                      "_parqit_xm_x":pa.array(xc,pa.int8())})
    table = table.replace_schema_metadata({b"parqit.xmissing":b'{"x":"_parqit_xm_x"}',
                                          b"parqit.schema":json.dumps(schema).encode()})
    pq.write_table(table, base + "_" + name + ".parquet")
    assert pq.read_table(base + "_" + name + ".parquet").column("x").to_pylist() == values
end
foreach materialized in 0 1 {
    python: os.environ["PARQIT_FETCH_MATERIALIZED"] = Macro.getLocal("materialized")
    foreach workers in 1 2 {
        parqit set fill_threads `workers'
        foreach case in high negative onvalue {
            clear
            set obs 1
            gen sentinel = 987
            quietly datasignature
            local before `"`r(datasignature)'"'
            capture noisily parqit use `"`base'_`case'.parquet"', clear int64(string)
            local rc = _rc
            di "`case' / fetch=`materialized' / workers=`workers': rc=`rc'"
            if (`rc' == 0) local ++fails
            quietly datasignature
            if (`"`r(datasignature)'"' != `"`before'"') local ++fails
        }
        parqit use `"`base'_valid.parquet"', clear int64(string)
        assert _N == 28 & c(k) == 2 & x[1] == "9007199254740993" & x[28] == ""
        capture assert x == "." + char(95+id) in 2/27
        if (_rc) local ++fails
    }
}
if (`fails') di "VERDICT(V112_XMISSING_TEXT_INTEGRITY): FAIL - `fails' checks"
else di "VERDICT(V112_XMISSING_TEXT_INTEGRITY): PASS"
