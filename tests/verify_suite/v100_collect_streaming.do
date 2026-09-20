* ADVERSARIAL: the STREAMED fill (PERF-STREAM-1). cmd_use_fetch no longer runs
* its SELECT through duckdb_query (which "stores the full (materialized) result",
* duckdb.h:1209) but through Session::query_streaming, so the engine's parallel
* scan produces chunks while the worker pool is already writing cells into the
* staged frame. The loops, fill_column and every message are unchanged — only
* the origin of a chunk is. This test proves the change is invisible in the data
* and visible nowhere else:
*   1. a direct read of a 300k-row, 5-row-group, 8-column file (int/double with
*      NULLs and 1.7e300 extremes/int8/int16/date/timestamp/UTF-8 with emoji and
*      empty strings/strL over the 2045-byte boundary) matches an independent
*      pyarrow oracle cell for cell, in file order;
*   2. the lazy path (filter + gen + descending sort + collect, i.e. the temp-
*      table variant of the same fetch) matches an independent duckdb-CLI oracle;
*   3. PARQIT_FETCH_MATERIALIZED=1 (the escape hatch that keeps the old
*      duckdb_query fetch) produces a byte-identical dataset -- cf _all;
*   4. a fetch that fails mid-stream (PARQIT_TEST_FAIL_FETCH_AT) is loud on both
*      the parallel and the serial path, leaves the dataset in memory untouched,
*      and leaves the connection healthy: the very next read in the same session
*      succeeds, which is what proves the abandoned stream was cancelled.
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

local fails 0
tempfile fbase
local f `"`fbase'.parquet"'
tempfile obase
local ocsv `"`obase'.csv"'
tempfile dbase
local dta1 `"`dbase'.dta"'

* ---------------------------------------------------------------- the fixture
* 300k rows, row_group_size 65536 -> 5 row groups, snappy. Every value is a
* closed-form function of id, so any misplaced chunk is caught exactly.
python:
from sfi import Macro, Scalar
import pyarrow as pa, pyarrow.parquet as pq
import numpy as np

N = 300_000
idx = np.arange(N, dtype=np.int32)

# x: multiples of 0.25 (exact in binary64, so sums compare exactly), both signs,
# NULLs every 50k rows and one huge positive / one huge negative value.
x = ((idx % 1001).astype(np.float64) - 500.0) * 0.25
x[7] = 1.7e300
x[11] = -1.7e300
xnull = (idx % 50_000) == 3
xmask = ~xnull

b = ((idx % 127) - 63).astype(np.int8)
s = ((idx % 32_000) - 16_000).astype(np.int16)
d = (1000 + (idx % 40_000)).astype(np.int32)              # days since 1970-01-01
ts = (idx % 1000).astype(np.int64) * 86_400_000_000 + 123_000   # us, ms-exact

txt = np.array(["t" + str(i % 997) for i in range(N)], dtype=object)
txt[13] = "café-acentuado"
txt[17] = "\U0001F600\U0001F389 emoji"
txt[21] = ""
tnull = (idx % 40_000) == 9
txt_arr = pa.array(txt, type=pa.string(), mask=tnull)

big = np.array(["b" + str(i % 13) for i in range(N)], dtype=object)
long_rows = [17, 100_017, 200_017]
for r in long_rows:
    big[r] = "L" + ("z" * 2500) + "#" + str(r)            # > 2045 bytes -> strL

t = pa.table({
    "id":  pa.array(idx, pa.int32()),
    "x":   pa.array(x, pa.float64(), mask=xnull),
    "b":   pa.array(b, pa.int8()),
    "s":   pa.array(s, pa.int16()),
    "d":   pa.array(d, pa.date32()),
    "ts":  pa.array(ts, pa.timestamp("us")),
    "txt": txt_arr,
    "big": pa.array(big, type=pa.string()),
})
pq.write_table(t, Macro.getLocal("f"), row_group_size=65536, compression="snappy")

small = xmask & (np.abs(x) < 1e6)
Scalar.setValue("oN",         N)
Scalar.setValue("o_rowgroups", pq.ParquetFile(Macro.getLocal("f")).num_row_groups)
Scalar.setValue("o_id_sum",   float(idx.astype(np.float64).sum()))
Scalar.setValue("o_x_nonmiss", int(xmask.sum()))
Scalar.setValue("o_x_small_sum", float(x[small].sum()))
Scalar.setValue("o_b_sum",    float(b.astype(np.float64).sum()))
Scalar.setValue("o_s_sum",    float(s.astype(np.float64).sum()))
# Stata has no string NULL: a Parquet NULL lands as "" (documented), so the
# empty-string count is the 8 NULLs plus the one genuinely empty cell.
Scalar.setValue("o_txt_empty", int(tnull.sum()) + 1)
# Stata storage: %td = days since 1960-01-01 (+3653); %tc = ms since 1960-01-01
Scalar.setValue("o_d_1",      float(d[0] + 3653))
Scalar.setValue("o_d_last",   float(d[N - 1] + 3653))
Scalar.setValue("o_ts_2",     float(ts[1] // 1000 + 3653 * 86_400_000))
Scalar.setValue("o_big_len",  len(big[17]))
Macro.setLocal("o_big_tail",  big[17][-12:])
Macro.setLocal("o_txt_emoji", txt[17])
Macro.setLocal("o_txt_acc",   txt[13])
end

if (scalar(o_rowgroups) < 5) {
    di as err "FAIL: fixture has only `=scalar(o_rowgroups)' row groups"
    local ++fails
}

* ------------------------------------------- 1. direct read through the stream
parqit use using `"`f'"', clear

if (_N != scalar(oN)) {
    di as err "FAIL: direct read N `=_N' != `=scalar(oN)'"
    local ++fails
}
* file order and every cell bound to its exact source row
assert id == _n - 1
assert b == mod(id, 127) - 63
assert s == mod(id, 32000) - 16000
assert missing(x) if mod(id, 50000) == 3
assert x == (mod(id, 1001) - 500) * 0.25 if mod(id, 50000) != 3 & id != 7 & id != 11
assert x[8] == 1.7e300
assert x[12] == -1.7e300

summarize id, meanonly
if (r(sum) != scalar(o_id_sum)) {
    di as err "FAIL: id sum `=r(sum)' != oracle `=scalar(o_id_sum)'"
    local ++fails
}
quietly count if !missing(x)
if (r(N) != scalar(o_x_nonmiss)) {
    di as err "FAIL: x non-missing `=r(N)' != oracle `=scalar(o_x_nonmiss)'"
    local ++fails
}
summarize x if abs(x) < 1e6, meanonly
if (r(sum) != scalar(o_x_small_sum)) {
    di as err "FAIL: x sum `=r(sum)' != oracle `=scalar(o_x_small_sum)'"
    local ++fails
}
summarize b, meanonly
if (r(sum) != scalar(o_b_sum)) {
    di as err "FAIL: b sum `=r(sum)' != oracle `=scalar(o_b_sum)'"
    local ++fails
}
summarize s, meanonly
if (r(sum) != scalar(o_s_sum)) {
    di as err "FAIL: s sum `=r(sum)' != oracle `=scalar(o_s_sum)'"
    local ++fails
}
quietly count if txt == ""
if (r(N) != scalar(o_txt_empty)) {
    di as err "FAIL: txt empty/NULL count `=r(N)' != oracle `=scalar(o_txt_empty)'"
    local ++fails
}
if (d[1] != scalar(o_d_1) | d[scalar(oN)] != scalar(o_d_last)) {
    di as err "FAIL: date endpoints `=d[1]'/`=d[_N]' != oracle"
    local ++fails
}
if (ts[2] != scalar(o_ts_2)) {
    di as err "FAIL: timestamp row 2 `=ts[2]' != oracle `=scalar(o_ts_2)'"
    local ++fails
}
* UTF-8 payloads: emoji, accents and the empty string survive the streamed fill
if (txt[18] != `"`o_txt_emoji'"' | txt[14] != `"`o_txt_acc'"' | txt[22] != "") {
    di as err "FAIL: UTF-8 cells corrupted by the streamed fill"
    local ++fails
}
* strL: a row over the 2045-byte boundary, whole and in the right place
local bl = strlen(big[18])
if (`bl' != scalar(o_big_len) | substr(big[18], -12, .) != `"`o_big_tail'"') {
    di as err "FAIL: strL row 18 wrong (len `bl', tail `=substr(big[18], -12, .)')"
    local ++fails
}
if (big[19] != "b" + string(mod(18, 13))) {
    di as err "FAIL: short strL neighbour wrong"
    local ++fails
}
save `"`dta1'"', replace

* ------------------------------------------ 3. escape-hatch parity (same data)
python:
import os
os.environ["PARQIT_FETCH_MATERIALIZED"] = "1"
end
parqit use using `"`f'"', clear
python:
import os
os.environ.pop("PARQIT_FETCH_MATERIALIZED", None)
end
capture cf _all using `"`dta1'"'
if (_rc != 0) {
    di as err "FAIL: PARQIT_FETCH_MATERIALIZED=1 differs from the streamed read (rc `=_rc')"
    local ++fails
}

* --------------------- 2. lazy path (filter + gen + sort + collect) vs duckdb
* Independent oracle: the duckdb CLI binary, a separate process and build, over
* the same file and the same (SQL) missing semantics parqit compiles to.
shell duckdb -c "COPY (SELECT count(*) AS n, max(id) AS id_first, min(id) AS id_last, sum(id::BIGINT) AS id_sum, sum(CASE WHEN abs(x) < 1e6 THEN x * 2 ELSE 0 END) AS y_small_sum, count(*) FILTER (WHERE abs(x) >= 1e6) AS n_big FROM read_parquet('`f'') WHERE x > 0) TO '`ocsv'' (FORMAT CSV, HEADER)"

python:
from sfi import Macro, Scalar
import csv
with open(Macro.getLocal("ocsv")) as fh:
    row = next(csv.DictReader(fh))
for k in ("n", "id_first", "id_last", "id_sum", "y_small_sum", "n_big"):
    Scalar.setValue("q_" + k, float(row[k]))
end

parqit use using `"`f'"'
parqit keep if x > 0
parqit gen double y = x * 2
parqit gsort -id
parqit collect, clear
parqit close _all

if (_N != scalar(q_n)) {
    di as err "FAIL: lazy N `=_N' != duckdb oracle `=scalar(q_n)'"
    local ++fails
}
* NULL x was dropped by the filter (SQL missing semantics), so none may remain
quietly count if missing(x)
if (r(N) != 0) {
    di as err "FAIL: `=r(N)' missing x survived `parqit keep if x > 0'"
    local ++fails
}
* descending order, end to end, with no equal or inverted neighbour
assert id > id[_n+1] if _n < _N
assert y == x * 2
if (id[1] != scalar(q_id_first) | id[_N] != scalar(q_id_last)) {
    di as err "FAIL: sorted endpoints `=id[1]'/`=id[_N]' != oracle"
    local ++fails
}
summarize id, meanonly
if (r(sum) != scalar(q_id_sum)) {
    di as err "FAIL: lazy id sum `=r(sum)' != oracle `=scalar(q_id_sum)'"
    local ++fails
}
summarize y if abs(x) < 1e6, meanonly
if (r(sum) != scalar(q_y_small_sum)) {
    di as err "FAIL: lazy y sum `=r(sum)' != oracle `=scalar(q_y_small_sum)'"
    local ++fails
}
quietly count if abs(x) >= 1e6
if (r(N) != scalar(q_n_big)) {
    di as err "FAIL: extreme-value rows `=r(N)' != oracle `=scalar(q_n_big)'"
    local ++fails
}

* ------------------- 4. a failed fetch is loud, atomic, and leaves no residue
clear
set obs 3
gen long sentinel = 909
gen str8 keepme = "precious"
datasignature set, reset

tempname lg
local plog "`c(tmpdir)'/_parqit_v100_inject.log"

* parallel path
capture erase `"`plog'"'
python:
import os
os.environ["PARQIT_FILL_THREADS"] = "4"
os.environ["PARQIT_TEST_FAIL_FETCH_AT"] = "2"
end
log using `"`plog'"', text name(`lg')
capture noisily parqit use using `"`f'"', clear
local injrc = _rc
log close `lg'
mata: st_local("injtxt", invtokens(cat(st_local("plog"))', char(10)))
capture erase `"`plog'"'
if (`injrc' == 0) {
    di as err "FAIL: an injected fetch failure returned rc 0 (parallel path)"
    local ++fails
}
if (strpos(`"`injtxt'"', "deterministic test injection at chunk") == 0) {
    di as err "FAIL: the injected fetch failure did not name itself (parallel path)"
    local ++fails
}
capture datasignature confirm
if (_rc != 0 | _N != 3 | sentinel[1] != 909) {
    di as err "FAIL: the dataset in memory did not survive the failed fetch (parallel)"
    local ++fails
}

* serial path
capture erase `"`plog'"'
python:
import os
os.environ["PARQIT_FILL_THREADS"] = "1"
end
log using `"`plog'"', text name(`lg')
capture noisily parqit use using `"`f'"', clear
local injrc = _rc
log close `lg'
mata: st_local("injtxt", invtokens(cat(st_local("plog"))', char(10)))
capture erase `"`plog'"'
if (`injrc' == 0) {
    di as err "FAIL: an injected fetch failure returned rc 0 (serial path)"
    local ++fails
}
if (strpos(`"`injtxt'"', "deterministic test injection at chunk") == 0) {
    di as err "FAIL: the injected fetch failure did not name itself (serial path)"
    local ++fails
}
capture datasignature confirm
if (_rc != 0 | _N != 3 | keepme[3] != "precious") {
    di as err "FAIL: the dataset in memory did not survive the failed fetch (serial)"
    local ++fails
}

* the abandoned streams must have been cancelled: the same session reads again
python:
import os
os.environ.pop("PARQIT_TEST_FAIL_FETCH_AT", None)
os.environ.pop("PARQIT_FILL_THREADS", None)
end
capture noisily parqit use using `"`f'"', clear
if (_rc != 0) {
    di as err "FAIL: the session was left unusable after the aborted streams (rc `=_rc')"
    local ++fails
}
else if (_N != scalar(oN) | id[1] != 0 | id[_N] != scalar(oN) - 1) {
    di as err "FAIL: the read after the aborted streams returned `=_N' rows"
    local ++fails
}

if (`fails' == 0) di "VERDICT(V100_COLLECT_STREAMING): PASS - streamed fill matches pyarrow and duckdb oracles, the materialised hatch is byte-identical, and a failed fetch is loud, atomic and leaves the session healthy"
else {
    di as err "VERDICT(V100_COLLECT_STREAMING): FAIL - `fails' check(s)"
    exit 9
}
