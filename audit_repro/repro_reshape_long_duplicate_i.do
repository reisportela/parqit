* Repro: parqit reshape long accepts duplicate i() rows that native Stata rejects.
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

clear
input long id double inc2019 double inc2020
1 10 20
1 30 40
2 50 60
end

tempfile wide_dta wide_base
qui save `"`wide_dta'"'
local wide_pq `"`wide_base'.parquet"'
parqit save `"`wide_pq'"', replace

* Native Stata refuses this reshape because id does not uniquely identify rows.
use `"`wide_dta'"', clear
capture reshape long inc, i(id) j(year)
local native_rc = _rc

clear
parqit use using `"`wide_pq'"'
capture noisily parqit reshape long inc, i(id) j(year)
local parqit_rc = _rc
if (`parqit_rc' == 0) {
    parqit collect, clear
    local parqit_N = _N
}
else {
    local parqit_N = .
}

if (`native_rc' != 0 & `parqit_rc' == 0 & `parqit_N' == 6) {
    di "VERDICT(REPRO_RESHAPE_LONG_DUPLICATE_I): FAIL - parqit accepted duplicate i() that native Stata rejects"
    exit 9
}

di "VERDICT(REPRO_RESHAPE_LONG_DUPLICATE_I): PASS - parqit rejected duplicate i()"
