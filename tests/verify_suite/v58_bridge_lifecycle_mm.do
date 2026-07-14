* V58 — 2026-07-14 audit remediation:
*   BRIDGE-LIFETIME-1  operation-owned adapter bridges clean on every branch;
*   MM-ORDER-1         lazy merge m:m refuses before import/view mutation,
*                      while native mergein m:m remains available.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
global V58FAILS 0

program define _v58_fail
    version 16.0
    args msg
    di as err `"FAIL: `msg'"'
    global V58FAILS = $V58FAILS + 1
end

program define _v58_no_import_roots
    version 16.0
    args tag
    local roots : dir `"`c(tmpdir)'"' dirs "_parqit_bridge_import_*"
    local legacy : dir `"`c(tmpdir)'"' files "_parqit_imp_*.parquet"
    if (`"`roots'`legacy'"' != "") {
        _v58_fail `"`tag': adapter bridge residue: `roots' `legacy'"'
    }
end

program define _v58_exists
    version 16.0
    args path expected tag
    capture confirm file `"`path'"'
    local exists = (_rc == 0)
    if (`exists' != `expected') {
        _v58_fail `"`tag': path existence=`exists' expected=`expected' (`path')"'
    }
end

tempfile master lookup lookup2 lookup_bad mm_master mm_using mm_native protected

clear
input long id double x
1 10
2 20
end
parqit save `"`master'.parquet"', replace data

clear
input long id double z
1 100
2 200
end
save `"`lookup'.dta"', replace

clear
input long id double z2
1 1000
2 2000
end
save `"`lookup2'.dta"', replace

clear
input long id double w
1 7
1 8
end
save `"`lookup_bad'.dta"', replace

* 0) An internal owned_files claim cannot turn an arbitrary user path into a
* package-owned deletion candidate.
clear
set obs 1
gen byte guard = 1
parqit save `"`protected'.parquet"', replace data
capture noisily parqit use using `"`protected'.parquet"', name(forged) owned
if (_rc != 198) _v58_fail "an unregistered owned_files claim was not refused"
capture confirm file `"`protected'.parquet"'
if (_rc) _v58_fail "an arbitrary user path was erased by bridge cleanup"

* 1) A projected eager .dta use that fails after bridge creation cleans the
* bridge and leaves the in-memory dataset untouched.
clear
set obs 1
gen long sentinel = 777
capture noisily parqit use does_not_exist using `"`lookup'.dta"', clear
local rc = _rc
if (`rc' != 111) _v58_fail "failed projected eager use did not return rc 111"
if (_N != 1 | sentinel[1] != 777) _v58_fail "failed eager use mutated memory"
_v58_no_import_roots "failed eager use"

* 1b) A successful eager adapter consumes and removes its bridge immediately.
parqit use using `"`lookup'.dta"', clear
if (_N != 2 | z[1] != 100) _v58_fail "successful eager adapter payload changed"
_v58_no_import_roots "successful eager use"

* 2) A lazy merge failure after importing a .dta cleans the bridge and leaves
* the master view usable and unchanged.
parqit use using `"`master'.parquet"', name(failmerge)
capture noisily parqit merge m:1 missing_key using `"`lookup'.dta"'
if (_rc == 0) _v58_fail "merge with a missing key unexpectedly succeeded"
_v58_no_import_roots "failed lazy merge"
quietly parqit count
if (r(N) != 2) _v58_fail "failed lazy merge changed the current view"
parqit close failmerge

* 3) A successful lazy merge owns its adapter until that view is closed.
parqit use using `"`master'.parquet"', name(okmerge)
parqit merge m:1 id using `"`lookup'.dta"', nogen
local merge_bridge `"`r(bridge)'"'
if (`"`merge_bridge'"' == "") _v58_fail "successful merge did not report its owned bridge"
else _v58_exists `"`merge_bridge'"' 1 "merge before close"
parqit close okmerge
if (`"`merge_bridge'"' != "") _v58_exists `"`merge_bridge'"' 0 "merge after close"

* 3b) Successful multi-source append transfers every adapter bridge.
parqit use using `"`master'.parquet"', name(okappend)
parqit append using `"`lookup'.dta"' `"`lookup2'.dta"'
local append_n = r(n_bridges)
local append1 `"`r(bridge_1)'"'
local append2 `"`r(bridge_2)'"'
if (`append_n' != 2 | `"`append1'"' == "" | `"`append2'"' == "") {
    _v58_fail "successful append did not transfer both adapter bridges"
}
else {
    _v58_exists `"`append1'"' 1 "append bridge 1 before close"
    _v58_exists `"`append2'"' 1 "append bridge 2 before close"
}
parqit close okappend
if (`"`append1'"' != "") _v58_exists `"`append1'"' 0 "append bridge 1 after close"
if (`"`append2'"' != "") _v58_exists `"`append2'"' 0 "append bridge 2 after close"

* 3c) Successful joinby transfers its adapter bridge.
parqit use using `"`master'.parquet"', name(okjoin)
parqit joinby id using `"`lookup'.dta"'
local join_bridge `"`r(bridge)'"'
if (`"`join_bridge'"' == "") _v58_fail "successful joinby did not report its owned bridge"
else _v58_exists `"`join_bridge'"' 1 "joinby before close"
quietly parqit count
if (r(N) != 2) _v58_fail "successful joinby changed row-count semantics"
parqit close okjoin
if (`"`join_bridge'"' != "") _v58_exists `"`join_bridge'"' 0 "joinby after close"

* 4) A failed named replacement neither switches the current view nor drops
* the target's old bridge.  A successful replacement then transfers ownership.
parqit use using `"`lookup'.dta"', name(repl)
local repl1 `"`r(bridge)'"'
parqit use using `"`master'.parquet"', name(anchor)
local roots_before : dir `"`c(tmpdir)'"' dirs "_parqit_bridge_import_*"
capture noisily parqit use does_not_exist using `"`lookup2'.dta"', name(repl)
if (_rc == 0) _v58_fail "failed named replacement unexpectedly succeeded"
local roots_after : dir `"`c(tmpdir)'"' dirs "_parqit_bridge_import_*"
if (`"`roots_before'"' != `"`roots_after'"') {
    _v58_fail "failed named replacement changed the set of adapter bridges"
}
if (`"`repl1'"' != "") _v58_exists `"`repl1'"' 1 "failed replacement old bridge"
parqit collect, clear
capture confirm variable x
local bad_current = _rc
if (!`bad_current') local bad_current = (x[1] != 10)
if (`bad_current') _v58_fail "failed named replacement switched the current view"
parqit close anchor
parqit view repl
parqit collect, clear
capture confirm variable z
local bad_target = _rc
if (!`bad_target') local bad_target = (z[1] != 100)
if (`bad_target') _v58_fail "failed named replacement changed the target view"
parqit use using `"`lookup2'.dta"', name(repl)
local repl2 `"`r(bridge)'"'
if (`"`repl1'"' == "" | `"`repl2'"' == "" | `"`repl1'"' == `"`repl2'"') {
    _v58_fail "replacement bridges were missing or not distinct"
}
else {
    _v58_exists `"`repl1'"' 0 "replaced view old bridge"
    _v58_exists `"`repl2'"' 1 "replaced view new bridge"
}
parqit close repl
if (`"`repl2'"' != "") _v58_exists `"`repl2'"' 0 "replacement close"

* 5a) Distinct named views clean independently.
parqit use using `"`lookup'.dta"', name(one)
local one `"`r(bridge)'"'
parqit use using `"`lookup2'.dta"', name(two)
local two `"`r(bridge)'"'
if (`"`one'"' == "" | `"`two'"' == "" | `"`one'"' == `"`two'"') {
    _v58_fail "two named adapter views did not receive distinct bridges"
}
parqit close one
if (`"`one'"' != "") _v58_exists `"`one'"' 0 "close first named view"
if (`"`two'"' != "") _v58_exists `"`two'"' 1 "second named view remains"
parqit view two
parqit collect, clear
if (_N != 2 | z2[1] != 1000) _v58_fail "closing first view broke second view"
parqit close two
if (`"`two'"' != "") _v58_exists `"`two'"' 0 "close second named view"

* 5b) A view embedded as a using source shares the bridge reference: closing
* the source view must not invalidate the derived plan.
parqit use using `"`lookup'.dta"', name(lookupview)
local shared `"`r(bridge)'"'
parqit use using `"`master'.parquet"', name(derived)
parqit merge m:1 id using view:lookupview, nogen
parqit close lookupview
if (`"`shared'"' != "") _v58_exists `"`shared'"' 1 "shared bridge after source close"
parqit view derived
parqit collect, clear
if (_N != 2 | z[1] != 100) _v58_fail "derived view lost a shared using bridge"
parqit close derived
if (`"`shared'"' != "") _v58_exists `"`shared'"' 0 "shared bridge after final close"

* 6) If append imports one adapter and a later source fails, every bridge made
* by that operation is removed and the view stays unchanged.
parqit use using `"`master'.parquet"', name(appfail)
capture noisily parqit append using `"`lookup'.dta"' `"`c(tmpdir)'/missing_v58.dta"'
if (_rc == 0) _v58_fail "partial multi-file append unexpectedly succeeded"
_v58_no_import_roots "partial append"
quietly parqit count
if (r(N) != 2) _v58_fail "partial append changed the current view"
parqit close appfail

* 7) close _all is the final safety sweep for every package-owned bridge.
parqit use using `"`lookup'.dta"', name(sweep1)
local sweep1 `"`r(bridge)'"'
parqit use using `"`lookup2'.dta"', name(sweep2)
local sweep2 `"`r(bridge)'"'
parqit close _all
if (`"`sweep1'"' != "") _v58_exists `"`sweep1'"' 0 "close all bridge 1"
if (`"`sweep2'"' != "") _v58_exists `"`sweep2'"' 0 "close all bridge 2"
_v58_no_import_roots "close all"

* MM-ORDER-1: lazy m:m refuses with a stable rc before it imports the .dta or
* changes the plan.  The original master remains collectable.
clear
input byte k int mv
1 20
1 10
end
save `"`mm_master'.dta"', replace
parqit save `"`mm_master'.parquet"', replace data

clear
input byte k int uv
1 300
1 100
1 200
end
save `"`mm_using'.dta"', replace

parqit use using `"`mm_master'.parquet"', name(mmrefuse)
capture noisily parqit merge m:m k using `"`mm_using'.dta"', nogen
local mmrc = _rc
if (`mmrc' != 198) _v58_fail `"lazy merge m:m rc=`mmrc', expected 198"'
_v58_no_import_roots "refused lazy m:m"
parqit collect, clear
if (_N != 2 | mv[1] != 20 | mv[2] != 10) _v58_fail "refused m:m changed its master view"
parqit close mmrefuse

* Native mergein m:m remains deliberately available and must match a direct
* native merge on the same physical row order.
use `"`mm_master'.dta"', clear
merge m:m k using `"`mm_using'.dta"', nogen
save `"`mm_native'"', replace
use `"`mm_master'.dta"', clear
capture noisily parqit mergein m:m k using `"`mm_using'.dta"', nogen
if (_rc) _v58_fail "mergein m:m control was rejected"
else {
    capture cf _all using `"`mm_native'"'
    if (_rc) _v58_fail "mergein m:m no longer matches native merge"
}

capture parqit close _all
if ($V58FAILS == 0) {
    di as result "VERDICT(V58_BRIDGE_LIFETIME_MM): PASS"
}
else {
    di as err "VERDICT(V58_BRIDGE_LIFETIME_MM): FAIL - $V58FAILS differences"
}
