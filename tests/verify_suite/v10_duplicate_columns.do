* CHARTER 10 (pq finding 10): duplicate column names in a parquet file are
* disambiguated with a warning — never silently dropped. pq's signature: a
* 2-column file "dup,dup" loaded as a single variable with the first
* column's payload gone.
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
# legal parquet: two columns with the same name
t = pa.Table.from_arrays(
    [pa.array([1, 2], pa.int32()), pa.array([3, 4], pa.int32())],
    names=["dup", "dup"])
pq.write_table(t, Macro.getLocal("f"))
end

parqit use using `"`f'"', clear
assert c(k) == 2
assert _N == 2

* both payloads present, deterministic names (the engine disambiguates as
* dup, dup_1; the true parquet name is preserved in the src_name char)
assert dup[1] == 1 & dup[2] == 2
assert dup_1[1] == 3 & dup_1[2] == 4
assert `"`: char dup_1[src_name]'"' == "dup"

di "VERDICT(V10_DUP_COLUMNS): PASS - both duplicate columns load, disambiguated deterministically"
