* V180 — SPSS-PARALLEL-1: the SPSS conversion profiles and converts blocks of
* cases on the engine's threads and hands the engine Arrow batches it writes in
* parallel and in case order. Forced onto that route (PARQIT_SPSS_ROUTE=parallel:
* the fixtures are below the size that takes it, A3) and into blocks of 7 cases,
* it must write the very file the serial route writes (PARQIT_SPSS_ROUTE=serial:
* one-thread profile and table function, the same binary): every column and
* cell (pyarrow), the Parquet types and the
* parqit metadata, the returned results, and the data `parqit use` loads (cf),
* in the three layouts of the fixtures (the unit test adds 3001 cases with a
* user-missing range and signed zeros). A trace line proves the batches. Needs
* pyarrow importable from Stata's Python.
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
parqit set threads 4
tempfile stem nat
local fix `"`repo'/tests/fixtures/spss"'
local fails 0

python:
from sfi import Macro
try:
    import pyarrow
    Macro.setLocal("have_oracle", "1")
except Exception as e:
    print("oracle unavailable:", e)
    Macro.setLocal("have_oracle", "0")
end
if ("`have_oracle'" != "1") {
    di "VERDICT(V180_SPSS_PARALLEL_CONVERSION): FAIL - pyarrow not importable from Stata's Python (see python query)"
    exit
}

program define _v180_pair
    args in out nat
    python: import os; os.environ["PARQIT_SPSS_ROUTE"] = "serial"; os.environ.pop("PARQIT_TEST_SPSS_BLOCK_CASES", None); os.environ.pop("PARQIT_TEST_SPSS_TRACE", None)
    quietly parqit save `"`out'_tf.parquet"' using `"`in'"', replace
    local a `"`r(N)' `r(k)' `r(spss_compression)' `r(transcoded_cells)'"'
    quietly parqit use using `"`in'"', clear
    quietly save `"`nat'"', replace
    python: import os; os.environ["PARQIT_SPSS_ROUTE"] = "parallel"; os.environ["PARQIT_TEST_SPSS_BLOCK_CASES"] = "7"; os.environ["PARQIT_TEST_SPSS_TRACE"] = "1"
    parqit save `"`out'_ar.parquet"' using `"`in'"', replace
    local b `"`r(N)' `r(k)' `r(spss_compression)' `r(transcoded_cells)'"'
    parqit use using `"`in'"', clear
    python: import os; os.environ.pop("PARQIT_SPSS_ROUTE", None); os.environ.pop("PARQIT_TEST_SPSS_BLOCK_CASES", None); os.environ.pop("PARQIT_TEST_SPSS_TRACE", None)
    capture cf _all using `"`nat'"', all
    c_local cf_rc = _rc
    c_local same_r = `"`a'"' == `"`b'"'
end

capture log close _v180
tempfile tl
log using `"`tl'.log"', text replace name(_v180)
foreach f in survey survey_bc {
    _v180_pair `"`fix'/`f'.sav"' `"`stem'_`f'"' `"`nat'"'
    if (`cf_rc' | !`same_r') {
        di as err "FAIL: `f'.sav: parallel conversion loads other data or results (cf rc `cf_rc')"
        local ++fails
    }
}
_v180_pair `"`fix'/survey.zsav"' `"`stem'_survey_z"' `"`nat'"'
if (`cf_rc' | !`same_r') {
    di as err "FAIL: survey.zsav: parallel conversion loads other data or results"
    local ++fails
}
log close _v180

python:
from sfi import Macro, Scalar
import numpy as np, pyarrow as pa, pyarrow.parquet as pq
stem = Macro.getLocal("stem")
ok = 1
def same(ta, tb):
    # NaN is not equal to itself under Table.equals: compare validity and the
    # bits of floating columns, values of the others
    if ta.schema.names != tb.schema.names or ta.num_rows != tb.num_rows: return False
    for n in ta.schema.names:
        ca, cb = ta[n].combine_chunks(), tb[n].combine_chunks()
        if ca.type != cb.type or not ca.is_valid().equals(cb.is_valid()): return False
        if pa.types.is_floating(ca.type):
            xa = ca.fill_null(0).to_numpy(zero_copy_only=False).view(np.uint64)
            xb = cb.fill_null(0).to_numpy(zero_copy_only=False).view(np.uint64)
            if not np.array_equal(xa, xb): return False
        elif not ca.equals(cb): return False
    return True
for f in ("survey", "survey_bc", "survey_z"):
    a = pq.ParquetFile(stem + "_" + f + "_tf.parquet"); b = pq.ParquetFile(stem + "_" + f + "_ar.parquet")
    ta, tb = a.read(), b.read()
    if not same(ta, tb):
        print("FAIL", f, "payload"); ok = 0
    if ta.schema != tb.schema or a.schema_arrow.metadata != b.schema_arrow.metadata:
        print("FAIL", f, "schema/metadata"); ok = 0
    if [a.schema.column(i).physical_type for i in range(a.metadata.num_columns)] != \
       [b.schema.column(i).physical_type for i in range(b.metadata.num_columns)]:
        print("FAIL", f, "physical types"); ok = 0
trace = open(Macro.getLocal("tl") + ".log", encoding="utf-8").read()
# 60 cases / 7 = 9 batches; per file one save and one use
if trace.count("spss conversion: route parallel, profile parallel, batches 9, workers 4") != 6: ok = 0
Scalar.setValue("pyok", ok)
end
if (scalar(pyok) != 1) {
    di as err "FAIL: the parallel conversion's Parquet files differ from the table function's, or it ran unbatched"
    local ++fails
}

if (`fails' == 0) di "VERDICT(V180_SPSS_PARALLEL_CONVERSION): PASS"
else di "VERDICT(V180_SPSS_PARALLEL_CONVERSION): FAIL (`fails')"
