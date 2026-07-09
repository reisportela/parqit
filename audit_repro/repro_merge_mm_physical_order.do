* Repro: parqit's deterministic m:m fallback sorts within keys by all columns;
* native Stata pairs rows in their existing within-key order. The documented
* sequential reuse rule is the same, but the paired payload can differ.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

local fails 0
tempfile master using mpq upq native

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
save `using', replace
parqit save `"`upq'.parquet"', replace data

use `master', clear
merge m:m k using `using', nogen
sort upos
save `native', replace

parqit use using `"`mpq'.parquet"'
parqit merge m:m k using `"`upq'.parquet"', nogen
parqit collect, clear
sort upos
capture cf mv uv mpos upos using `native'
if (_rc) {
    di as err "REPRODUCED: parqit m:m paired payloads in a different within-key order"
    local ++fails
}
parqit close _all

di as txt "VERDICT(REPRO_MERGE_MM_PHYSICAL_ORDER): " ///
    cond(`fails' == 0, "PASS", "FAIL - deterministic fallback differs from native physical-order pairing")
