* v57 — STATS-MISSKEY-1: lazy statistical group keys fold SQL NULL and
* empty-string encodings into one Stata missing value.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

* Define the ado's Mata protocol helpers globally so this test can inspect the
* display-only codebook/tabstat response records directly.
do `"`repo'/src/ado/p/parqit.ado"'
program parqit_plugin, plugin using(`"`plugin'"')

tempfile stem
python:
import pyarrow as pa
import pyarrow.parquet as pq
from sfi import Macro

b = Macro.getLocal("stem")
pq.write_table(pa.table({"id": pa.array([1], pa.int32()),
                         "key": pa.array([""], pa.string()),
                         "col": pa.array([""], pa.string())}),
               b + "_master.parquet")
pq.write_table(pa.table({"id": pa.array([2], pa.int32())}),
               b + "_using.parquet")
end

parqit use using `"`stem'_master.parquet"'
parqit append using `"`stem'_using.parquet"'

quietly parqit duplicates report key
assert r(N) == 2
assert r(unique_value) == 1
assert r(surplus) == 1

quietly parqit tabulate key, missing
assert r(N) == 2 & r(r) == 1
quietly parqit tabulate key col, missing
assert r(N) == 2 & r(r) == 1 & r(c) == 1

* distinct/codebook exclude Stata missing values, regardless of whether the
* lazy plan currently spells that missing as '' or SQL NULL.
quietly parqit distinct key
assert r(N) == 2 & r(ndistinct) == 0

tempfile req_cb resp_cb
local _sq_what "codebook"
local _sq_vars "key"
local _sq_limit ""
local _sq_expr ""
local _sq_joint ""
local _sq_pairwise ""
local _sq_missing ""
local _sq_stats ""
local _sq_by ""
local _sq_bins ""
mata: _parqit_wr_stats_request("`req_cb'", "`resp_cb'")
plugin call parqit_plugin, view_stats `reqhex'
mata:
fh = fopen(st_local("resp_cb"), "r")
f = _parqit_fields(fget(fh), 9)
fclose(fh)
st_local("cb_missing", f[3])
st_local("cb_distinct", f[4])
end
assert real("`cb_missing'") == 2
assert real("`cb_distinct'") == 0

* Native tabstat, by() omits missing by-groups. Inspect the response protocol
* directly because tabstat is a display command and has no group-count r().
tempfile req_ts resp_ts
local _sq_what "tabstat"
local _sq_vars "id"
local _sq_stats "n"
local _sq_by "key"
mata: _parqit_wr_stats_request("`req_ts'", "`resp_ts'")
plugin call parqit_plugin, view_stats `reqhex'
mata:
fh = fopen(st_local("resp_ts"), "r")
n = 0
while ((line = fget(fh)) != J(0, 0, "")) {
    f = _parqit_fields(line, 4)
    if (f[1] == "ts") n++
}
fclose(fh)
st_local("ts_groups", strofreal(n, "%21.0g"))
end
assert real("`ts_groups'") == 0

parqit close _all
di "VERDICT(V57_STATS_MISSING_KEYS): PASS - grouped statistics, codebook, distinct and tabstat use native missing semantics"
