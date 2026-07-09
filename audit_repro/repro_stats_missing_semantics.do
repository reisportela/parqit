* Repro: two-table verbs can mix '' and SQL NULL for one Stata string missing.
* Grouped stats must fold them, while codebook/distinct/tabstat must exclude
* them where native Stata excludes missing values.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
do `"`repo'/src/ado/p/parqit.ado"'
program parqit_plugin, plugin using(`"`plugin'"')

local fails 0
tempfile stem
python:
import pyarrow as pa
import pyarrow.parquet as pq
from sfi import Macro

b = Macro.getLocal("stem")
pq.write_table(pa.table({"id": pa.array([1], pa.int32()),
                         "key": pa.array([""], pa.string())}),
               b + "_master.parquet")
pq.write_table(pa.table({"id": pa.array([2], pa.int32())}),
               b + "_using.parquet")
end

parqit use using `"`stem'_master.parquet"'
parqit append using `"`stem'_using.parquet"'

quietly parqit duplicates report key
if (r(unique_value) != 1 | r(surplus) != 1) local ++fails
quietly parqit tabulate key, missing
if (r(N) != 2 | r(r) != 1) local ++fails
quietly parqit distinct key
if (r(ndistinct) != 0) local ++fails

tempfile req resp
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
mata: _parqit_wr_stats_request("`req'", "`resp'")
plugin call parqit_plugin, view_stats `reqhex'
mata:
fh = fopen(st_local("resp"), "r")
f = _parqit_fields(fget(fh), 9)
fclose(fh)
st_local("cb_missing", f[3])
st_local("cb_distinct", f[4])
end
if (real("`cb_missing'") != 2 | real("`cb_distinct'") != 0) local ++fails

parqit close _all
di as txt "VERDICT(REPRO_STATS_MISSING_SEMANTICS): " ///
    cond(`fails' == 0, "PASS", "FAIL - lazy stats disagree with Stata missing semantics")
