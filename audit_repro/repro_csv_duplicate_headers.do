* Independent repro (audit 2026-09-01, F4 / CSV-HEADER-1): a CSV whose header
* repeats a name, or repeats it in another case, loaded the engine's
* deduplicated names (a, a_1, b, A_2) silently — no note, no src_name, and the
* case-distinct `A` lost its exact name.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

tempfile stem
local csv `"`stem'.csv"'
mata:
f = fopen(st_local("csv"), "w")
fput(f, "a,a,b,A")
fput(f, "1,2,3,4")
fclose(f)
end
parqit use `"`csv'"', clear
qui ds
assert "`r(varlist)'" == "a a_1 b A"
assert "`: char a_1[src_name]'" == "a"
assert A[1] == 4 & a_1[1] == 2

di as result "VERDICT(REPRO_CSV_DUPLICATE_HEADERS): PASS - CSV header names get the Parquet name recovery"
