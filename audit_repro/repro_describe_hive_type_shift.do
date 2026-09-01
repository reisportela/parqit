* Independent repro (audit 2026-09-01, F3 / DESCRIBE-ALIGN-1): the file form of
* `parqit describe` on a partition_by() tree printed the engine types shifted
* by one from the partition key onwards (names in manifest order, types in scan
* order, zipped positionally).
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

tempfile stem
local dir `"`stem'_d"'
mkdir `"`dir'"'
clear
set obs 6
gen long id = _n
gen int year = 2000 + mod(_n, 2)
gen str4 name = "n" + string(_n)
gen double wage = _n * 1.5
parqit save `"`dir'/tree"', replace data partition_by(year)
parqit describe `"`dir'/tree"'
forvalues i = 1/`r(n_cols)' {
    local t_`r(name_`i')' `"`r(type_`i')'"'
}
assert "`t_id'" == "INTEGER"
assert "`t_name'" == "VARCHAR"
assert "`t_wage'" == "DOUBLE"
assert inlist("`t_year'", "BIGINT", "INTEGER", "SMALLINT", "VARCHAR")

di as result "VERDICT(REPRO_DESCRIBE_HIVE_TYPE_SHIFT): PASS - describe pairs each variable with its own engine type on a partitioned tree"
