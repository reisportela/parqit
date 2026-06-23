* CHARTER 2 + 14 (pq findings 2, 14): columns whose names need sanitising —
* reserved words, leading digits, >32 chars, spaces, unicode — must load
* with their VALUES (pq loaded them all-missing), under documented renames,
* with the original name kept in char var[src_name]. A space in a name must
* never brick the file.
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
cols = {
    "if": pa.array([1, 2, 3], pa.int32()),
    "in": pa.array([4, 5, 6], pa.int32()),
    "byte": pa.array([7, 8, 9], pa.int32()),
    "1x": pa.array([10, 11, 12], pa.int32()),
    "x y": pa.array([13, 14, 15], pa.int32()),
    "a" * 33: pa.array([16, 17, 18], pa.int32()),
    "ano_decisão": pa.array([19, 20, 21], pa.int32()),
    "normal": pa.array([22, 23, 24], pa.int32()),
}
pq.write_table(pa.table(cols), Macro.getLocal("f"))
end

parqit use using `"`f'"', clear
assert _N == 3 & c(k) == 8

* every renamed column carries its data — the exact pq corruption signature
* was values all-missing under rc 0
assert _if[1] == 1 & _if[3] == 3
assert _in[1] == 4
assert _byte[2] == 8
assert _1x[3] == 12
assert x_y[1] == 13
local a32 = "a" * 32
assert `a32'[2] == 17
assert ano_decisão[3] == 21
assert normal[1] == 22

* original names preserved (documented, reversible scheme)
assert `"`: char _if[src_name]'"' == "if"
assert `"`: char x_y[src_name]'"' == "x y"
assert `"`: char `a32'[src_name]'"' == "a" * 33

di "VERDICT(V02_RENAMED): PASS - reserved/digit/space/long/unicode names load with values"
