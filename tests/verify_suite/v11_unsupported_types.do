* CHARTER 11 (pq finding 11): decimal128 loads as numbers (it is real
* payload — warehouse money); unrepresentable types (list/struct) are
* dropped WITH a message; an all-null column (pyarrow's null type is written
* to Parquet as a physical int32 of all nulls, so DuckDB reads it as integer)
* loads as a faithful all-missing byte — never a silent all-missing DOUBLE
* among good columns, and never with real data/structure lost (finding 11).
* A file whose every column is unsupported errors loudly, not loads empty.
* (A genuinely typeless DuckDB NULL column drops per brief §4/§6.11 — that
* path is covered by the C++ unit test, since the read path never yields it.)
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

tempfile fbase
local f `"`fbase'.parquet"'

python:
from sfi import Macro
import decimal
import pyarrow as pa, pyarrow.parquet as pq
t = pa.table({
    "money": pa.array([decimal.Decimal("12345.67"), decimal.Decimal("-0.03"),
                       None], pa.decimal128(18, 2)),
    "lst": pa.array([[1, 2], [3], None], pa.list_(pa.int32())),
    "stru": pa.array([{"a": 1}, {"a": 2}, None], pa.struct([("a", pa.int32())])),
    "nul": pa.array([None, None, None], pa.null()),
    "good": pa.array([7, 8, 9], pa.int32()),
})
pq.write_table(t, Macro.getLocal("f"))
end

parqit use using `"`f'"', clear
assert _N == 3

* decimal -> double with VALUES (pq loaded all-missing under rc 0)
confirm double variable money
assert reldif(money[1], 12345.67) < 1e-12
assert reldif(money[2], -0.03) < 1e-12
assert missing(money[3])

* list/struct dropped (loudly); the all-null column is a faithful all-missing
* byte (DuckDB reads its int32 physical type, so it is sized to byte exactly
* like a real integer that happens to be all-null — no data is lost)
capture confirm variable lst
assert _rc != 0
capture confirm variable stru
assert _rc != 0
confirm byte variable nul
assert missing(nul[1]) & missing(nul[3])

* the good column is untouched next to all of this
assert good[1] == 7 & good[3] == 9

* a file whose EVERY column is unsupported (here all list) has nothing
* loadable -> loud error, never an empty or all-missing dataset (drop-all guard)
tempfile lbase
local lf `"`lbase'.parquet"'
python:
import pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro
pq.write_table(pa.table({"a": pa.array([[1], [2]], pa.list_(pa.int32())),
                         "b": pa.array([[3], [4]], pa.list_(pa.int32()))}),
               Macro.getLocal("lf"))
end
capture parqit use using `"`lf'"', clear
assert _rc != 0

di "VERDICT(V11_UNSUPPORTED): PASS - decimal=values, list/struct=loud drop, all-null=faithful byte, all-unsupported=loud error"
