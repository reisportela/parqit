* CHARTER 6 (pq finding 6): uint32 values ≥ 2^31 must arrive as numbers —
* pq's signature was [0, 2^31, 2^32-1] -> [0, ., .] with rc 0. Also covers
* uint64/int64 beyond 2^53 (values arrive, rounded, with a loud note) and
* int32 boundary values that would collide with Stata missing codes.
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
    "u32": pa.array([0, 2**31, 2**32 - 1], pa.uint32()),
    "u64": pa.array([1, 2**53, 2**64 - 1], pa.uint64()),
    "i64": pa.array([-2**62, 0, 2**62], pa.int64()),
    "edge32": pa.array([-2**31, 0, 2**31 - 1], pa.int32()),
})
pq.write_table(t, Macro.getLocal("f"))
end

parqit use using `"`f'"', clear
assert _N == 3

* the pq corruption signature: u32 == [0, ., .]
assert u32[1] == 0
assert u32[2] == 2147483648
assert u32[3] == 4294967295
assert !missing(u32[2]) & !missing(u32[3])

* uint64/int64: representable as doubles (rounding documented + warned)
assert u64[1] == 1
assert u64[2] == 9007199254740992
assert reldif(u64[3], 18446744073709551616) < 1e-15
assert i64[1] == -4611686018427387904 | reldif(i64[1], -4611686018427387904) < 1e-15

* int32 edges beyond Stata's long range must be in double, never missing
confirm double variable edge32
assert edge32[1] == -2147483648
assert edge32[3] == 2147483647

di "VERDICT(V06_UINT32): PASS - unsigned/wide integers arrive as values, never silent missings"
