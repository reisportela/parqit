* v49 — NUM1/IO1 (+T2): a narrowing fill must never silently convert a real
* value to missing.
*
* The F2 fast path sizes byte/int/long/float columns from the Parquet
* footer's stats_min/max (exact for honest writers — the spec requires it).
* A file whose footer UNDERSTATES the true range (hand-forged here by
* byte-patching the stats while leaving the data pages intact, the same
* class as a historically-buggy writer) made parqit pick a too-narrow Stata
* type, and SF_vstore then silently stored the real value as `.` — rc 0, no
* note, on both the eager use and the lazy passthrough collect. The same
* silent `.` hit a DATE beyond Stata's %td long window (year ~5.88M) even
* with honest stats, because DATE never range-refines. fill_column now
* counts every value outside its planned type's storable window and the
* load refuses loudly; the in-memory dataset survives (staged swap).
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

tempfile fb hb db
local forged `"`fb'.parquet"'
local honest `"`hb'.parquet"'
local fardate `"`db'.parquet"'

python:
import struct
import pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro

data = [1, 2, 3, 4, 150]
t = pa.table({'v': pa.array(data, pa.int32())})
# honest twin: same data, truthful stats
pq.write_table(t, Macro.getLocal("honest"), compression='none',
               write_statistics=True)
# forged: identical bytes, then patch the footer max stat 150 -> 5
pq.write_table(t, Macro.getLocal("forged"), compression='none',
               write_statistics=True)
fn = Macro.getLocal("forged")
b = bytearray(open(fn, 'rb').read())
pat, small = struct.pack('<i', 150), struct.pack('<i', 5)
occ, s = [], 0
while True:
    j = b.find(pat, s)
    if j < 0:
        break
    occ.append(j)
    s = j + 1
# the last two occurrences are the column-chunk / page-index max stats;
# the first is the data page value, which stays intact
for j in occ[-2:]:
    b[j:j+4] = small
open(fn, 'wb').write(b)

# a DATE beyond Stata's %td long window: day 2147479968 + 3653 > 2^31-28
d = pa.table({'d': pa.array([0, 2147479968], pa.date32()),
              'k': pa.array([1, 2], pa.int32())})
pq.write_table(d, Macro.getLocal("fardate"))
end

* the forge really lies: duckdb sees data max 150 under footer stat max 5
python:
import subprocess
from sfi import Macro, SFIToolkit
q = ("select max(v)::VARCHAR || '|' || "
     "(select max(stats_max_value) from parquet_metadata('{f}')) "
     "from read_parquet('{f}')").format(f=Macro.getLocal("forged"))
out = subprocess.run(['duckdb', '-noheader', '-list', '-c', q],
                     capture_output=True, text=True).stdout.strip()
SFIToolkit.stata("local forge_oracle " + ("1" if out == "150|5" else "0"))
end
assert `forge_oracle' == 1

* ---------- the honest twin loads perfectly (no false positive) ----------
clear
parqit use using `"`honest'"', clear
assert _rc == 0 & _N == 5
local st : type v
assert "`st'" == "int"           // sized from truthful stats (150 <= 32740)
assert v[5] == 150

* ---------- eager use of the forged file refuses loudly ----------
clear
set obs 3
gen sentinel = _n
capture noisily parqit use using `"`forged'"', clear
assert _rc != 0
assert _N == 3 & sentinel[3] == 3          // staged swap: memory intact

* ---------- the lazy passthrough collect refuses too ----------
* (a forged-stats file also fools DuckDB's own predicate pushdown, so there
* is no lazy-filter escape: the only real fix is to rewrite the statistics.
* parqit's job is to refuse rather than silently store `.` — which it does.)
clear
parqit use using `"`forged'"'
capture noisily parqit collect, clear
assert _rc != 0
parqit close _all

* ---------- a DATE beyond Stata's %td window refuses (was a silent `.`) ----
clear
capture noisily parqit use using `"`fardate'"', clear
assert _rc != 0

* honest far-but-storable dates still load: td window edge stays intact
tempfile ob
local okdate `"`ob'.parquet"'
python:
import pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro
t = pa.table({'d': pa.array([0, 2936549 - 3653], pa.date32())})  # 9999-12-31
pq.write_table(t, Macro.getLocal("okdate"))
end
clear
parqit use using `"`okdate'"', clear
assert _rc == 0
assert d[2] == 2936549                       // 31dec9999, exact

di "VERDICT(V49_STATS_RANGE_GUARD): PASS - forged-stats and out-of-range-date loads refuse loudly on eager and lazy paths; honest files and the lazy-filter remedy intact"
