* Repro: append can represent Stata string missing as either '' or SQL NULL.
* duplicates must group both encodings together, as native Stata does.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

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

* Native oracle after materialising the exact combined rows.
parqit use using `"`stem'_master.parquet"'
parqit append using `"`stem'_using.parquet"'
parqit collect, clear
quietly duplicates report key
local want_unique = r(unique_value)
local want_surplus = r(N) - r(unique_value)
assert `want_unique' == 1
assert `want_surplus' == 1
parqit close _all

* Lazy command currently groups SQL NULL and '' separately.
parqit use using `"`stem'_master.parquet"'
parqit append using `"`stem'_using.parquet"'
quietly parqit duplicates report key
if (r(unique_value) != `want_unique' | r(surplus) != `want_surplus') {
    di as err "REPRODUCED: lazy duplicates splits two encodings of Stata string missing"
    local ++fails
}
parqit close _all

di as txt "VERDICT(REPRO_DUPLICATES_MISSING_KEY): " ///
    cond(`fails' == 0, "PASS", "FAIL - missing key encodings formed separate groups")
