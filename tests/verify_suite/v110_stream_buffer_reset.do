* CA-01: zero restores the engine default after numeric/auto/environment caps.
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
tempfile source
local fails 0
python:
import os
os.environ.pop("PARQIT_STREAM_BUFFER_MB", None)
os.environ.pop("PARQIT_FETCH_MATERIALIZED", None)
end

program define buffer_value, rclass
    parqit sql "SELECT current_setting('streaming_buffer_size') AS buffer"
    parqit levelsof buffer
    local value `"`r(levels)'"'
    parqit close
    return local value `"`value'"'
end
buffer_value
local initial `"`r(value)'"'
set obs 100
gen long x = _n
parqit save `"`source'.parquet"', data

foreach mode in 8 auto {
    parqit set stream_buffer_mb `mode'
    parqit use `"`source'.parquet"', clear
    parqit set stream_buffer_mb 0
    parqit use `"`source'.parquet"', clear
    assert _N == 100 & x == _n
    buffer_value
    di `"after `mode' -> 0: `r(value)'; default: `initial'"'
    if (`"`r(value)'"' != `"`initial'"') local ++fails
}
python: os.environ["PARQIT_STREAM_BUFFER_MB"] = "8"
parqit set stream_buffer_mb auto
parqit use `"`source'.parquet"', clear
parqit set stream_buffer_mb 0
parqit use `"`source'.parquet"', clear
buffer_value
if (`"`r(value)'"' != `"`initial'"') local ++fails
python: os.environ["PARQIT_STREAM_BUFFER_MB"] = "0"
parqit set stream_buffer_mb 8
parqit use `"`source'.parquet"', clear
parqit set stream_buffer_mb auto
parqit use `"`source'.parquet"', clear
buffer_value
if (`"`r(value)'"' != `"`initial'"') local ++fails

* 2.0 auto bounds its total-budget replenishment; an explicit larger cap wins.
parqit version
if regexm(`"`r(duckdb_version)'"', "^v?2[.]") {
    tempfile large
    python:
import os
import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq
from sfi import Macro
os.environ.pop("PARQIT_STREAM_BUFFER_MB", None)
values=pa.array(np.ones(10000000,dtype=np.float64))
pq.write_table(pa.table({"x"+str(i):values for i in range(16)}),Macro.getLocal("large")+".parquet")
end
    parqit set stream_buffer_mb auto
    parqit use "`large'.parquet", clear
    assert _N==10000000 & c(k)==16 & x0==1 & x15==1
    buffer_value
    local automatic `"`r(value)'"'
    parqit set stream_buffer_mb 1024
    parqit use "`large'.parquet", clear
    buffer_value
    if (`"`r(value)'"' != `"`automatic'"') local ++fails
    parqit set stream_buffer_mb 2048
    parqit use "`large'.parquet", clear
    buffer_value
    if (`"`r(value)'"' == `"`automatic'"') local ++fails
}
if (`fails') di "VERDICT(V110_STREAM_BUFFER_RESET): FAIL - `fails' transitions"
else di "VERDICT(V110_STREAM_BUFFER_RESET): PASS"
