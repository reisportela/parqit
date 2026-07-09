* v54 — TEMPORAL-ROUND-1: both save paths must apply native Stata's
* floor(x + .5) integer-unit rule to fractional temporal values.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

tempfile stem
assert round(100.5) == 101
assert round(-100.5) == -100

clear
set obs 2
gen long id = _n
foreach v in d tc leap m {
    gen double `v' = cond(id == 1, 100.5, -100.5)
}
format d %td
format tc %tc
format leap %tC
format m %tm
parqit save `"`stem'_memory.parquet"', replace data

clear
set obs 2
gen long id = _n
foreach v in d tc leap m {
    gen double `v' = 0
}
format d %td
format tc %tc
format leap %tC
format m %tm
parqit save `"`stem'_source.parquet"', replace data
parqit use using `"`stem'_source.parquet"'
foreach v in d tc leap m {
    parqit replace `v' = cond(id == 1, 100.5, -100.5)
}
parqit save `"`stem'_view.parquet"', replace
parqit close _all

python:
from datetime import date, datetime
import pyarrow.parquet as pq
from sfi import Macro

b = Macro.getLocal("stem")
mem = pq.read_table(b + "_memory.parquet").to_pydict()
view = pq.read_table(b + "_view.parquet").to_pydict()
expected = {
    "d": [date(1960, 4, 11), date(1959, 9, 23)],
    "tc": [datetime(1960, 1, 1, 0, 0, 0, 101000),
           datetime(1959, 12, 31, 23, 59, 59, 900000)],
    "leap": [101, -100],
    "m": [101, -100],
}
for name, want in expected.items():
    assert mem[name] == want, (name, "memory", mem[name], want)
    assert view[name] == want, (name, "view", view[name], want)
end

di "VERDICT(V54_TEMPORAL_SAVE_PATH_PARITY): PASS - direct and lazy saves use native Stata half-tie rounding"
