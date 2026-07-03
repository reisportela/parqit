* v48 — SCH1/SCH2: strict mode must never guess across a mixed-schema glob.
*
* DuckDB's plain read_parquet takes the FIRST file's schema and casts every
* later file to it. In strict (default) mode that silently DOWN-CAST a column
* that widened in a later file — `x` int32 in a.parquet + double in b.parquet
* loaded as byte [1,2,4,4] with 3.5/4.5 destroyed, rc 0, no warning, the
* result depending on the glob's filename sort — and a column extra in a
* later file silently vanished. The help always promised "a schema mismatch
* across the matched files is a loud error"; the strict_schema_gate now makes
* that true on every read surface (eager use, lazy use, describe, and the
* using side of merge/append/joinby), while physically-different files that
* RESOLVE identically (INT96 vs TIMESTAMP) still read fine, and relaxed
* still unions correctly. Oracle: pyarrow/duckdb ground truth per file.
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

* ---------- fixtures ----------
tempfile dbase
local dir `"`dbase'.d"'
mkdir `"`dir'"'
mkdir `"`dir'/mixed"'
mkdir `"`dir'/extra"'
mkdir `"`dir'/perm"'
mkdir `"`dir'/int96"'
mkdir `"`dir'/same"'
python:
import datetime, pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro
d = Macro.getLocal("dir")
# SCH1: same column, wider type in the later-sorted file
pq.write_table(pa.table({'x': pa.array([1, 2], pa.int32()),
                         't': ['A', 'A']}), f'{d}/mixed/a.parquet')
pq.write_table(pa.table({'x': pa.array([3.5, 4.5], pa.float64()),
                         't': ['B', 'B']}), f'{d}/mixed/b.parquet')
# SCH2: extra column only in the later-sorted file
pq.write_table(pa.table({'id': pa.array([1, 2], pa.int32()),
                         'v': pa.array([10, 20], pa.int32())}),
               f'{d}/extra/a.parquet')
pq.write_table(pa.table({'id': pa.array([3, 4], pa.int32()),
                         'v': pa.array([30, 40], pa.int32()),
                         'extra': pa.array([99, 98], pa.int32())}),
               f'{d}/extra/b.parquet')
# permuted columns, identical schema: must stay readable (aligned by name)
pq.write_table(pa.table({'a': pa.array([1, 2], pa.int32()),
                         'b': ['x', 'y'],
                         'c': pa.array([1.5, 2.5])}), f'{d}/perm/a.parquet')
pq.write_table(pa.table({'c': pa.array([3.5, 4.5]),
                         'b': ['z', 'w'],
                         'a': pa.array([3, 4], pa.int32())}),
               f'{d}/perm/b.parquet')
# physically different, identically resolving: INT96 vs TIMESTAMP(us)
ts = [datetime.datetime(2020, 3, 1, 12, 0, 0),
      datetime.datetime(2021, 6, 2, 8, 30, 0)]
t = pa.table({'ev': pa.array(ts, pa.timestamp('us')),
              'id': pa.array([1, 2], pa.int32())})
pq.write_table(t, f'{d}/int96/a_int96.parquet',
               use_deprecated_int96_timestamps=True)
pq.write_table(t, f'{d}/int96/b_us.parquet')
# genuinely identical schemas: the normal multi-file panel
pq.write_table(pa.table({'id': pa.array([1, 2], pa.int32()),
                         'w': pa.array([1.0, 2.0])}), f'{d}/same/a.parquet')
pq.write_table(pa.table({'id': pa.array([3, 4], pa.int32()),
                         'w': pa.array([3.0, 4.0])}), f'{d}/same/b.parquet')
end

* ---------- SCH1: strict refuses the type conflict on every surface ----------
clear
set obs 3
gen sentinel = _n
capture noisily parqit use using `"`dir'/mixed/*.parquet"', clear
assert _rc != 0
assert _N == 3 & sentinel[3] == 3            // atomic: memory untouched

capture noisily parqit use using `"`dir'/mixed/*.parquet"'   // lazy open
assert _rc != 0
capture noisily parqit describe `"`dir'/mixed/*.parquet"'    // schema preview
assert _rc != 0

* using side of a merge refuses too (the same silent down-cast lived there)
clear
set obs 2
gen long id = _n
gen t = cond(_n == 1, "A", "B")
parqit open _data
capture noisily parqit merge m:1 t using `"`dir'/mixed/*.parquet"'
assert _rc != 0
parqit close _all

* relaxed still unions correctly: 3.5/4.5 survive as double
clear
parqit use using `"`dir'/mixed/*.parquet"', clear relaxed
sort t x
assert _N == 4
assert x[3] == 3.5 & x[4] == 4.5
local st : type x
assert "`st'" == "double"

* ---------- SCH2: the silently-vanishing extra column now refuses ----------
clear
capture noisily parqit use using `"`dir'/extra/*.parquet"', clear
assert _rc != 0
parqit use using `"`dir'/extra/*.parquet"', clear relaxed
sort id
assert _N == 4 & c(k) == 3
assert extra[3] == 99 & extra[4] == 98       // B's payload intact
assert missing(extra[1]) & missing(extra[2]) // A's rows null-fill

* ---------- no false positives ----------
* permuted-but-identical schemas read strict, values under the right names
clear
parqit use using `"`dir'/perm/*.parquet"', clear
sort a
assert _N == 4 & a[4] == 4 & b[4] == "w" & reldif(c[4], 4.5) < 1e-12

* INT96 vs TIMESTAMP resolve identically: strict must still read them
clear
parqit use using `"`dir'/int96/*.parquet"', clear
assert _rc == 0
assert _N == 4
assert ev[1] == tc(01mar2020 12:00:00)

* the normal identical-schema glob is untouched
clear
parqit use using `"`dir'/same/*.parquet"', clear
sort id
assert _N == 4 & reldif(w[4], 4.0) < 1e-12

* a single literal file skips the gate entirely (fast path intact)
clear
parqit use using `"`dir'/same/a.parquet"', clear
assert _N == 2

di "VERDICT(V48_STRICT_GLOB): PASS - mixed-schema globs refuse loudly on use/lazy/describe/merge-using; relaxed unions correctly; permuted/INT96/identical globs still read"
