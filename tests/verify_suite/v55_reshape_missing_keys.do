* v55 — RESHAPE-MISSKEY-1: reshape keys use Stata's NULL == "" equivalence.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

tempfile stem
python:
import pyarrow as pa
import pyarrow.parquet as pq
from sfi import Macro

b = Macro.getLocal("stem")
pq.write_table(pa.table({"i": pa.array([""], pa.string()),
                         "x1": pa.array([10], pa.int32()),
                         "x2": pa.array([20], pa.int32())}),
               b + "_long_master.parquet")
pq.write_table(pa.table({"x1": pa.array([30], pa.int32()),
                         "x2": pa.array([40], pa.int32())}),
               b + "_long_using.parquet")
pq.write_table(pa.table({"i": pa.array([""], pa.string()),
                         "j": pa.array([1], pa.int32()),
                         "x": pa.array([10], pa.int32())}),
               b + "_wide_master.parquet")
pq.write_table(pa.table({"j": pa.array([2], pa.int32()),
                         "x": pa.array([20], pa.int32())}),
               b + "_wide_using.parquet")
end

parqit use using `"`stem'_long_master.parquet"'
parqit append using `"`stem'_long_using.parquet"'
capture noisily parqit reshape long x, i(i) j(j)
assert _rc != 0
parqit close _all

parqit use using `"`stem'_wide_master.parquet"'
parqit append using `"`stem'_wide_using.parquet"'
parqit reshape wide x, i(i) j(j)
parqit collect, clear
assert _N == 1
assert i[1] == "" & x1[1] == 10 & x2[1] == 20
parqit close _all

di "VERDICT(V55_RESHAPE_MISSING_KEYS): PASS - reshape folds SQL NULL and empty string keys like native Stata"
