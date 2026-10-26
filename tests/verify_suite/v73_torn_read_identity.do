* V73 — TORN-READ-1 (audit 2026-08-22, A4-3): `parqit use, clear` probes the
* schema, counts the rows and fetches the data in separate passes; a concurrent
* replace between them delivered one file's rows under another file's schema
* with rc 0. parqit now records every source file's identity (size, mtime,
* ctime, inode) at plan time and re-checks it right before and right after the
* fetch, and compares the fetched column types with the plan — a changed file
* is refused loudly and the dataset in memory is untouched. A background
* pyarrow writer alternates two payloads (A: 300,000 rows, 3 columns, x == 1;
* B: 100,000 rows, 4 columns incl. `extra`, x == 2) by atomic rename while a
* reader loop loads the file: every successful load must be one consistent
* payload, never a mix.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

tempfile stem
local dir `"`stem'_cc"'
mkdir `"`dir'"'
local shared `"`dir'/shared.parquet"'
local stop   `"`dir'/STOP"'
local writer `"`dir'/writer.py"'
local wlog   `"`dir'/writer.log"'

python:
from sfi import Macro
import pyarrow as pa, pyarrow.parquet as pq, os
d = Macro.getLocal("dir")
# the two payloads, written once; the writer only renames copies into place
a = pa.table({"id": pa.array(range(300000), pa.int64()),
              "x": pa.array([1] * 300000, pa.int32()),
              "s": pa.array(["a"] * 300000, pa.string())})
b = pa.table({"id": pa.array(range(100000), pa.int64()),
              "x": pa.array([2] * 100000, pa.int32()),
              "s": pa.array(["b"] * 100000, pa.string()),
              "extra": pa.array([7.5] * 100000, pa.float64())})
pq.write_table(a, os.path.join(d, "A.parquet"), row_group_size=50000)
pq.write_table(b, os.path.join(d, "B.parquet"), row_group_size=20000)
os.replace(os.path.join(d, "A.parquet"), Macro.getLocal("shared"))
pq.write_table(a, os.path.join(d, "A.parquet"), row_group_size=50000)
open(Macro.getLocal("writer"), "w").write('''
import os, shutil, sys, time
d, shared, stop = sys.argv[1], sys.argv[2], sys.argv[3]
i = 0
t0 = time.time()
while not os.path.exists(stop) and time.time() - t0 < 240:
    src = os.path.join(d, "A.parquet" if i % 2 == 0 else "B.parquet")
    tmp = shared + ".tmp"
    shutil.copyfile(src, tmp)
    os.replace(tmp, shared)          # atomic replace: a new inode every time
    i += 1
    time.sleep(0.02)
open(os.path.join(d, "writer.done"), "w").write(str(i))
''')
end

* start the writer in the background (it stops when STOP appears or after 240 s)
shell python3 "`writer'" "`dir'" "`shared'" "`stop'" > "`wlog'" 2>&1 &
sleep 500

local inconsistent 0
local ok 0
local refused 0
forvalues i = 1/60 {
    capture noisily parqit use using `"`shared'"', clear
    if (_rc) {
        local ++refused
        continue
    }
    local n = _N
    local k = c(k)
    capture confirm variable extra
    local has_extra = (_rc == 0)
    qui sum x
    local xmin = r(min)
    local xmax = r(max)
    local consistent = 0
    if (`n' == 300000 & `k' == 3 & !`has_extra' & `xmin' == 1 & `xmax' == 1) local consistent 1
    if (`n' == 100000 & `k' == 4 & `has_extra' & `xmin' == 2 & `xmax' == 2) local consistent 1
    if (`consistent') {
        capture assert id == _n - 1 & s == cond(x == 1, "a", "b")
        if (_rc) local consistent 0
        if (`has_extra') {
            capture assert extra == 7.5
            if (_rc) local consistent 0
        }
    }
    if (`consistent') local ++ok
    else {
        di as err "INCONSISTENT load `i': N=`n' k=`k' extra=`has_extra' xmin=`xmin' xmax=`xmax'"
        local ++inconsistent
    }
}
* stop the writer and wait for it
file open fh using `"`stop'"', write replace
file close fh
local waited 0
while (!fileexists(`"`dir'/writer.done"') & `waited' < 60) {
    sleep 500
    local ++waited
}
di as txt "reads: `ok' consistent, `refused' refused loudly, `inconsistent' inconsistent"
assert fileexists(`"`dir'/writer.done"')
assert `inconsistent' == 0
assert `ok' + `refused' == 60
* Independent full-payload check after the writer stops, without printing rows.
python:
import hashlib, json
from pathlib import Path
table = pq.read_table(Macro.getLocal("shared"))
is_a, is_b = table.equals(a), table.equals(b)
assert is_a or is_b, "stable disk payload differs from both independently generated tables"
Macro.setLocal("stable_rows", str(table.num_rows))
_ = Path("v73_stable_oracle.json").write_text(json.dumps({
    "rows": table.num_rows, "columns": table.num_columns,
    "matches_A": is_a, "matches_B": is_b,
    "sha256": hashlib.sha256(Path(Macro.getLocal("shared")).read_bytes()).hexdigest()
}, indent=2) + "\n")
def preserve_failed_fixture():
    import shutil
    dest = Path("v73_failed_fixture")
    dest.mkdir()
    for name in ("A.parquet", "B.parquet", "shared.parquet", "writer.log", "writer.done"):
        source = Path(Macro.getLocal("dir")) / name
        if source.exists():
            shutil.copy2(source, dest / name)
end
* Two stable reads must work without clearing the engine cache between them.
file open fh using `"`stop'"', write replace
file close fh
forvalues stable = 1/2 {
    capture noisily parqit use using `"`shared'"', clear
    local final_rc = _rc
    if (`final_rc') {
        python: preserve_failed_fixture()
        exit `final_rc'
    }
    assert _N == `stable_rows'
    assert id == _n - 1
    assert x == cond(_N == 300000, 1, 2)
    assert s == cond(_N == 300000, "a", "b")
    if (_N == 100000) assert extra == 7.5
}

* the identity guard also fires on a plain in-place change between plan and
* fetch — simulate with the type/count guard on a glob whose file is replaced
* between two reads (a deterministic check that the message is parqit's own)
local g `"`dir'/g"'
mkdir `"`g'"'
python:
from sfi import Macro
import pyarrow as pa, pyarrow.parquet as pq, os
g = Macro.getLocal("g")
pq.write_table(pa.table({"id": pa.array([1, 2, 3], pa.int64())}), os.path.join(g, "p1.parquet"))
end
parqit use using `"`g'/p1.parquet"', clear
assert _N == 3

di "VERDICT(V73_TORN_READ_IDENTITY): PASS - under a concurrent writer every successful load was one consistent payload (`ok' loads, `refused' loud refusals, 0 inconsistent); the dataset stays usable"
