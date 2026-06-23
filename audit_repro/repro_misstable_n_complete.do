* Repro: parqit misstable returns r(n_complete)=0/1 by variable-missing status,
* not the number of complete observations.
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

clear
input double x double y
1 10
. 20
3 .
end

count if !missing(x, y)
local native_complete = r(N)

tempfile base
local pq `"`base'.parquet"'
parqit save `"`pq'"', replace

clear
parqit use using `"`pq'"'
parqit misstable
local parqit_complete = r(n_complete)

if (`parqit_complete' != `native_complete') {
    di "native complete observations = `native_complete'"
    di "parqit r(n_complete)       = `parqit_complete'"
    di "VERDICT(REPRO_MISSTABLE_N_COMPLETE): FAIL - r(n_complete) is not complete-observation count"
    exit 9
}

di "VERDICT(REPRO_MISSTABLE_N_COMPLETE): PASS - r(n_complete) matches native complete-observation count"
