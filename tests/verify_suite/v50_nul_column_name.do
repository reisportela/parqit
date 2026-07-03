* v50 — NM1: a parquet column name containing a NUL byte must refuse loudly,
* never silently collide.
*
* duckdb_column_name (and every SPI name API) is a C string, so
* "col\0hidden" truncated to "col", collided with the real sibling "col",
* and the rebuilt fetch SELECT bound the same physical column twice: the
* NUL-named column's data was silently LOST, the sibling's duplicated into
* its slot — rc 0, k right, values wrong (the charter's worst failure mode).
* DuckDB's own VARCHARs are length-counted, so the footer query CAN see the
* NUL: the source gate now refuses such a file on every surface, relaxed
* included. Oracle: pyarrow wrote and re-reads both distinct columns.
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

tempfile nb
local nulfile `"`nb'.parquet"'
python:
import pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro, SFIToolkit
t = pa.Table.from_arrays(
    [pa.array([1.0, 2.0]), pa.array([3.0, 4.0])],
    names=['col\x00hidden', 'col'])
pq.write_table(t, Macro.getLocal("nulfile"))
# oracle: the file genuinely holds two distinct columns
rt = pq.read_table(Macro.getLocal("nulfile"))
ok = (rt.schema.names == ['col\x00hidden', 'col']
      and rt.column(0).to_pylist() == [1.0, 2.0]
      and rt.column(1).to_pylist() == [3.0, 4.0])
SFIToolkit.stata("local nul_oracle " + ("1" if ok else "0"))
end
assert `nul_oracle' == 1

* ---------- every read surface refuses, memory intact ----------
clear
set obs 3
gen sentinel = _n
capture noisily parqit use using `"`nulfile'"', clear
assert _rc != 0
assert _N == 3 & sentinel[3] == 3

capture noisily parqit use using `"`nulfile'"', clear relaxed
assert _rc != 0                              // relaxed is no escape hatch

capture noisily parqit use using `"`nulfile'"'
assert _rc != 0                              // lazy open

capture noisily parqit describe `"`nulfile'"'
assert _rc != 0                              // no plausible-but-fake schema

clear
set obs 2
gen double col = _n
parqit open _data
capture noisily parqit merge 1:1 col using `"`nulfile'"'
assert _rc != 0                              // using side
parqit close _all

* ---------- no false positives: hostile-but-NUL-free names still load ----
tempfile gb
local good `"`gb'.parquet"'
python:
import pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro
t = pa.Table.from_arrays(
    [pa.array([1, 2], pa.int32()), pa.array([3, 4], pa.int32()),
     pa.array([5, 6], pa.int32())],
    names=['col name', 'razão social', 'col'])
pq.write_table(t, Macro.getLocal("good"))
end
clear
parqit use using `"`good'"', clear
assert _rc == 0 & c(k) == 3 & _N == 2
assert col_name[1] == 1 & col[2] == 6

di "VERDICT(V50_NUL_COLNAME): PASS - NUL-bearing column names refuse loudly on use/relaxed/lazy/describe/merge-using; hostile NUL-free names unaffected"
