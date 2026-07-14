* Regression for MM-ORDER-1.  Native Stata pairs m:m rows in physical within-key
* order, which a lazy plan cannot preserve.  The public lazy command therefore
* refuses before changing the view; mergein m:m remains the native control.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

local fails 0
tempfile master using mpq upq native
local using_dta `"`using'.dta"'

clear
input byte k int mv
1 20
1 10
end
gen byte mpos = _n
save `master', replace
parqit save `"`mpq'.parquet"', replace data

clear
input byte k int uv
1 300
1 100
1 200
end
gen byte upos = _n
save `"`using_dta'"', replace
parqit save `"`upq'.parquet"', replace data

use `master', clear
merge m:m k using `"`using_dta'"', nogen
sort upos
save `native', replace

parqit use using `"`mpq'.parquet"'
capture noisily parqit merge m:m k using `"`upq'.parquet"', nogen
local mmrc = _rc
if (`mmrc' != 198) {
    di as err "FAIL: lazy m:m was not refused with rc 198"
    local ++fails
}
parqit collect, clear
capture cf k mv mpos using `master'
if (_rc) {
    di as err "FAIL: refused lazy m:m changed the master view"
    local ++fails
}
parqit close _all

use `master', clear
parqit mergein m:m k using `"`using_dta'"', nogen
sort upos
capture cf mv uv mpos upos using `native'
if (_rc) {
    di as err "FAIL: mergein m:m no longer matches native Stata"
    local ++fails
}

di as txt "VERDICT(REPRO_MERGE_MM_PHYSICAL_ORDER): " ///
    cond(`fails' == 0, "PASS - lazy m:m refused; mergein m:m preserves native behavior", "FAIL")
