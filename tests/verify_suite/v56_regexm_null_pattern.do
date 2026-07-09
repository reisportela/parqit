* v56 — REGEXM-NULL-1: both regexm arguments use Stata string semantics.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

tempfile stem oracle
python:
import pyarrow as pa
import pyarrow.parquet as pq
from sfi import Macro

b = Macro.getLocal("stem")
pq.write_table(pa.table({"id": pa.array([1], pa.int32()),
                         "s": pa.array(["abc"], pa.string()),
                         "pat": pa.array([""], pa.string())}),
               b + "_master.parquet")
pq.write_table(pa.table({"id": pa.array([2], pa.int32()),
                         "s": pa.array(["xyz"], pa.string())}),
               b + "_using.parquet")
end

parqit use using `"`stem'_master.parquet"'
parqit append using `"`stem'_using.parquet"'
parqit collect, clear
gen double oracle_m = regexm(s, pat)
assert oracle_m == 1
save `oracle', replace
parqit close _all

parqit use using `"`stem'_master.parquet"'
parqit append using `"`stem'_using.parquet"'
parqit gen double got_m = regexm(s, pat)
parqit collect, clear
merge 1:1 id using `oracle', nogen
assert got_m == oracle_m
parqit close _all

di "VERDICT(V56_REGEXM_NULL_PATTERN): PASS - SQL NULL patterns behave as native Stata empty strings"
