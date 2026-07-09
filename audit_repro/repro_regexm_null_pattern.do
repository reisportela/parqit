* Repro: a two-table verb can introduce SQL NULL into a carried string column.
* Stata sees that cell as "", so regexm() must coalesce BOTH the subject and
* pattern. The current translator coalesces only the subject.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

local fails 0
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

* Materialise the combined data first and ask native Stata for the truth.
parqit use using `"`stem'_master.parquet"'
parqit append using `"`stem'_using.parquet"'
parqit collect, clear
gen double oracle_m = regexm(s, pat)
assert oracle_m[1] == 1 & oracle_m[2] == 1
save `oracle', replace
parqit close _all

* Run the same expression lazily. The appended row's pat is SQL NULL.
parqit use using `"`stem'_master.parquet"'
parqit append using `"`stem'_using.parquet"'
parqit gen double got_m = regexm(s, pat)
parqit collect, clear
merge 1:1 id using `oracle', nogen
capture assert got_m == oracle_m
if (_rc) {
    di as err "REPRODUCED: regexm with a SQL NULL pattern differs from the native empty-pattern result"
    local ++fails
}
parqit close _all

di as txt "VERDICT(REPRO_REGEXM_NULL_PATTERN): " ///
    cond(`fails' == 0, "PASS", "FAIL - pattern NULL was not normalized")
