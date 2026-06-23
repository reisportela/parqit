* V25 — `parqit mergein` / `parqit appendin`: join the IN-MEMORY dataset with a disk
* file via a native merge/append (the in-memory data stays put; parqit reads only
* the needed columns of the disk side). Oracle: the result must equal a plain
* native merge/append against the same data read independently.
clear all
set more off
set varabbrev off

args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

tempfile d
local D "`d'_mi"
shell mkdir -p "`D'"

* master (in memory) and a disk lookup
clear
set obs 200
gen long id = _n
gen fid = mod(_n, 20) + 1
gen double y = _n * 1.5
save "`D'/master.dta", replace          // also the oracle master

clear
set obs 20
gen fid = _n
gen double rate = _n / 7
gen str3 tag = "f" + string(_n)
* disk lookup as parquet
parqit open _data
parqit save "`D'/look.parquet", replace
* and as a .dta for the native oracle
clear
set obs 20
gen fid = _n
gen double rate = _n / 7
gen str3 tag = "f" + string(_n)
save "`D'/look.dta", replace

local nfail = 0

* --- 1) mergein m:1 == native merge ---
use "`D'/master.dta", clear
parqit mergein m:1 fid using "`D'/look.parquet", keepusing(rate tag) nogenerate
sort id
tempfile got
save "`got'", replace

use "`D'/master.dta", clear
merge m:1 fid using "`D'/look.dta", keepusing(rate tag) nogenerate
sort id
cf _all using "`got'"
if (_rc) {
    di as error "  FAIL: mergein result differs from native merge"
    local ++nfail
}
else di as result "  PASS: mergein m:1 == native merge (values identical)"

* --- 2) in-memory master is untouched if disk read fails (loud, atomic) ---
use "`D'/master.dta", clear
local n_before = _N
capture noisily parqit mergein m:1 fid using "`D'/does_not_exist.parquet", nogenerate
if (_rc != 0 & _N == `n_before') di as result "  PASS: failed mergein leaves the in-memory data intact (rc=`=_rc')"
else {
    di as error "  FAIL: failed mergein damaged memory or returned rc 0"
    local ++nfail
}

* --- 3) appendin == native append ---
use "`D'/master.dta", clear
parqit appendin using "`D'/master.dta"        // self-append via parqit (dta source)
sort id y
tempfile gota
save "`gota'", replace

use "`D'/master.dta", clear
append using "`D'/master.dta"
sort id y
cf _all using "`gota'"
if (_rc) {
    di as error "  FAIL: appendin result differs from native append"
    local ++nfail
}
else di as result "  PASS: appendin == native append (values identical)"

if (`nfail' == 0) di as result _n "VERDICT(v25_mergein_appendin): PASS"
else              di as error  _n "VERDICT(v25_mergein_appendin): FAIL (`nfail')"
