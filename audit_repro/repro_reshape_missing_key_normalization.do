* Repro: after append introduces SQL NULL into a string i() column, reshape
* must apply Stata's NULL == "" missing-key equivalence in uniqueness checks
* and grouping. Native reshape on the materialised combined data is the oracle.
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
pq.write_table(pa.table({"i": pa.array([""], pa.string()),
                         "x1": pa.array([10], pa.int32()),
                         "x2": pa.array([20], pa.int32())}),
               b + "_long_master.parquet")
pq.write_table(pa.table({"x1": pa.array([30], pa.int32()),
                         "x2": pa.array([40], pa.int32())}),
               b + "_long_using.parquet")
pq.write_table(pa.table({"i": pa.array([""], pa.string()),
                         "j": pa.array([1], pa.int32()),
                         "x": pa.array([10], pa.int32())}),
               b + "_wide_master.parquet")
pq.write_table(pa.table({"j": pa.array([2], pa.int32()),
                         "x": pa.array([20], pa.int32())}),
               b + "_wide_using.parquet")
end

* LONG oracle: after materialisation, both i values are Stata "" and native
* reshape must reject the duplicated i() identifier.
parqit use using `"`stem'_long_master.parquet"'
parqit append using `"`stem'_long_using.parquet"'
parqit collect, clear
capture reshape long x, i(i) j(j)
local native_long_rc = _rc
if (`native_long_rc' == 0) {
    di as err "oracle setup failed: native reshape long accepted duplicate i()"
    local ++fails
}
parqit close _all

* Current lazy implementation groups raw "" and SQL NULL separately, so it
* accepts the same logical data and later materialises duplicate (i,j) cells.
parqit use using `"`stem'_long_master.parquet"'
parqit append using `"`stem'_long_using.parquet"'
capture noisily parqit reshape long x, i(i) j(j)
local parqit_long_rc = _rc
if (`parqit_long_rc' == 0) {
    parqit collect, clear
    duplicates tag i j, gen(_dup)
    count if _dup > 0
    if (r(N) > 0) {
        di as err "REPRODUCED: lazy reshape long accepted duplicate Stata i()"
        local ++fails
    }
}
else {
    di as txt "reshape long already refused the normalized duplicate"
}
parqit close _all

* WIDE oracle: the two rows are one Stata i() group and native reshape emits
* one row carrying both x1 and x2.
parqit use using `"`stem'_wide_master.parquet"'
parqit append using `"`stem'_wide_using.parquet"'
parqit collect, clear
reshape wide x, i(i) j(j)
assert _N == 1 & x1[1] == 10 & x2[1] == 20
parqit close _all

* Current lazy GROUP BY uses raw i, producing two rows that both collect as i="".
parqit use using `"`stem'_wide_master.parquet"'
parqit append using `"`stem'_wide_using.parquet"'
capture noisily parqit reshape wide x, i(i) j(j)
local parqit_wide_rc = _rc
if (`parqit_wide_rc' == 0) {
    parqit collect, clear
    if (_N != 1 | x1[1] != 10 | x2[1] != 20) {
        di as err "REPRODUCED: lazy reshape wide split one Stata missing i() group"
        local ++fails
    }
}
else {
    di as err "parqit reshape wide unexpectedly failed instead of matching native"
    local ++fails
}
parqit close _all

di as txt "VERDICT(REPRO_RESHAPE_MISSING_KEYS): " ///
    cond(`fails' == 0, "PASS", "FAIL - NULL/empty i() semantics diverge")

