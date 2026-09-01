* V80 — DESCRIBE-ALIGN-1 (audit 2026-09-01, F3): the file form of `parqit
*   describe` pairs every variable with the engine type of the SAME column.
*   It used to zip the DESCRIBE rows (scan order: a Hive partition key last)
*   positionally with the var records (manifest order: the key in its original
*   place), so on a partitioned tree — or a glob over one — every type after
*   the key was shifted by one, in the printed table and in r(type_i).
*   Oracles: pyarrow schema types by NAME for file columns; the key itself is
*   the engine's Hive-inferred type, never a file column's.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

tempfile stem
local dir `"`stem'_d"'
mkdir `"`dir'"'

* collect r(name_i)/r(type_i) into locals t_<name>
program define _v80_types
    args target
    quietly parqit describe `"`target'"'
    local k = r(n_cols)
    forvalues i = 1/`k' {
        c_local t_`r(name_`i')' `"`r(type_`i')'"'
    }
    c_local v80_k `k'
end

clear
set obs 20
gen long id = _n
gen int year = 2000 + mod(_n, 3)
gen float age = 20 + _n
gen double wage = _n * 100.5
gen str8 name = "n" + string(_n)
gen byte flag = mod(_n, 2)
parqit save `"`dir'/flat.parquet"', replace data
parqit save `"`dir'/tree"', replace data partition_by(year)
parqit save `"`dir'/tree2"', replace data partition_by(flag)
parqit save `"`dir'/flat2.parquet"', replace data

* pyarrow: the file columns' physical types by name (the flat file)
python:
from sfi import Macro
import os, pyarrow.parquet as pq
sch = pq.read_schema(os.path.join(Macro.getLocal("dir"), "flat.parquet"))
m = {"int64": "BIGINT", "int32": "INTEGER", "int16": "SMALLINT", "int8": "TINYINT",
     "float": "FLOAT", "double": "DOUBLE", "string": "VARCHAR", "large_string": "VARCHAR",
     "date32[day]": "DATE", "bool": "BOOLEAN"}
for f in sch:
    Macro.setLocal("pa_" + f.name, m.get(str(f.type), str(f.type)))
end

* (the glob is quoted: an unquoted /**/ would open a Stata block comment)
foreach target in "flat.parquet" "tree" "tree/**/*.parquet" "tree2" {
    _v80_types `"`dir'/`target'"'
    assert `v80_k' == 6
    foreach v in id age wage name flag year {
        local iskey = ("`target'" != "flat.parquet" & "`v'" == cond("`target'" == "tree2", "flag", "year"))
        if (`iskey') {
            * the partition key arrives as the engine's Hive-inferred type
            assert inlist("`t_`v''", "BIGINT", "INTEGER", "SMALLINT", "VARCHAR")
        }
        else {
            assert "`t_`v''" == "`pa_`v''"
        }
    }
}
* a plain glob over two flat files keeps every pairing too
_v80_types `"`dir'/flat*.parquet"'
assert `v80_k' == 6
foreach v in id year age wage name flag {
    assert "`t_`v''" == "`pa_`v''"
}

* ---------- a foreign file whose scan names are rewritten (sanitised, deduped,
* case-aliased) still pairs by name ----------------------------------------------
python:
from sfi import Macro
import os, pyarrow as pa, pyarrow.parquet as pq
t = pa.Table.from_arrays(
    [pa.array([1, 2], pa.int64()), pa.array([1.5, 2.5], pa.float32()),
     pa.array([3.5, 4.5], pa.float64()), pa.array([7, 8], pa.int16()), pa.array(["u", "v"])],
    names=["a b", "dup", "dup", "X", "x"])
pq.write_table(t, os.path.join(Macro.getLocal("dir"), "foreign.parquet"))
end
_v80_types `"`dir'/foreign.parquet"'
assert `v80_k' == 5
assert "`t_a_b'" == "BIGINT"
assert "`t_dup'" == "FLOAT"
assert "`t_dup_1'" == "DOUBLE"
assert "`t_X'" == "SMALLINT"
assert "`t_x'" == "VARCHAR"

* the view form was never affected; it must still agree with the file form
parqit use using `"`dir'/tree"'
parqit describe
assert r(n_cols) == 6
parqit close _all

di "VERDICT(V80_DESCRIBE_ALIGNMENT): PASS - describe pairs names and engine types by name on flat files, Hive trees, globs over trees and sanitised/deduped/case-aliased foreign files"
