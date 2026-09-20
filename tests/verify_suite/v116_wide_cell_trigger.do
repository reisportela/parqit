* Isolate the parallel cell trigger: fewer than 50,000 rows, over 2M cells.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
tempfile base
python:
import os
import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq
from sfi import Macro
cols = {"v"+str(k):np.arange(20000,dtype=np.int32)+k*100000 for k in range(110)}
pq.write_table(pa.table(cols), Macro.getLocal("base")+".parquet", row_group_size=4000)
os.environ.pop("PARQIT_FILL_THREADS", None)
os.environ["PARQIT_TEST_FAIL_THREAD_AT"] = "0"
end
parqit set fill_threads auto
parqit version
local cpus = r(cpus)
if (`cpus' >= 2) {
    set obs 1
    gen sentinel = 987
    capture noisily parqit use `"`base'.parquet"', clear
    local rc = _rc
    assert `rc' != 0 & _N == 1 & sentinel == 987
}
else di "note: one CPU available; worker fault injection is not applicable"
parqit set fill_threads 1
parqit use `"`base'.parquet"', clear
assert _N == 20000 & c(k) == 110
foreach k of numlist 0/109 {
    assert v`k' == _n-1+`k'*100000
}
quietly datasignature
local serial `"`r(datasignature)'"'
python: os.environ.pop("PARQIT_TEST_FAIL_THREAD_AT")
parqit set fill_threads auto
parqit use `"`base'.parquet"', clear
foreach k of numlist 0/109 {
    assert v`k' == _n-1+`k'*100000
}
quietly datasignature
assert `"`r(datasignature)'"' == `"`serial'"'
di "VERDICT(V116_WIDE_CELL_TRIGGER): PASS"
