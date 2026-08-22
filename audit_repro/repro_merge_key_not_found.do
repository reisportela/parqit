* Repro (D7, JOINKEY-1): a join key absent from one side used to reach DuckDB.
*
* merge validates its uniqueness contracts BEFORE mutating the plan — the right
* order — but those queries GROUP BY the keys, so a key that exists on neither
* side (or only on one) was first seen by the binder, not by parqit:
*
*   parqit merge: Binder Error: Referenced column "nosuchkey" not found in
*   FROM clause!  Candidate bindings: "id"
*   LINE 1: ...])) SELECT * FROM __parqit_s0) GROUP BY (CASE WHEN isnan(...
*   ... rc 920
*
* View::merge_with already had the right message; it just ran too late.
* View::join_keys_error now holds that check once, and cmd_view_twotable calls
* it before anything else touches the keys — so both merge and joinby name the
* key and side, with native rc 111 for an absent name and rc 106 for incompatible
* types.
* Pinned by tests/verify_suite/v67_runtime_message_contract.do.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

local fails 0
tempfile stem
local master `"`stem'_master.parquet"'
local using  `"`stem'_using.parquet"'
local strkey `"`stem'_strkey.parquet"'

clear
set obs 10
gen long id = _n
gen double wage = _n
parqit save `"`master'"', replace data

clear
set obs 10
gen long id = _n
gen double rate = _n / 2
gen long other = _n
parqit save `"`using'"', replace data

clear
set obs 10
gen str6 id = "k" + string(_n)
gen double z = _n
parqit save `"`strkey'"', replace data

* --- absent from both sides, in every lazy merge kind -----------------------
foreach k in 1:1 m:1 1:m {
    parqit use using `"`master'"'
    capture noisily parqit merge `k' nosuchkey using `"`using'"'
    if (_rc != 111) {
        di as err "FAIL: merge `k' with an unknown key expected rc 111, got `=_rc'"
        local fails = `fails' + 1
    }
    parqit count
    if (r(N) != 10) {
        di as err "FAIL: a refused merge `k' changed the view (N = `=r(N)')"
        local fails = `fails' + 1
    }
    parqit close _all
}

* --- present on one side only, and a kind mismatch --------------------------
parqit use using `"`master'"'
capture noisily parqit merge 1:1 wage using `"`using'"'      /* master only */
if (_rc != 111) {
    di as err "FAIL: master-only key expected rc 111, got `=_rc'"
    local fails = `fails' + 1
}
capture noisily parqit merge 1:1 other using `"`using'"'     /* using only  */
if (_rc != 111) {
    di as err "FAIL: using-only key expected rc 111, got `=_rc'"
    local fails = `fails' + 1
}
capture noisily parqit merge 1:1 id using `"`strkey'"'       /* num vs str  */
if (_rc != 106) {
    di as err "FAIL: kind-mismatched key expected rc 106, got `=_rc'"
    local fails = `fails' + 1
}
parqit close _all

* --- joinby answers the same way -------------------------------------------
parqit use using `"`master'"'
capture noisily parqit joinby nosuchkey using `"`using'"'
if (_rc != 111) {
    di as err "FAIL: joinby with an unknown key expected rc 111, got `=_rc'"
    local fails = `fails' + 1
}
capture noisily parqit joinby id using `"`strkey'"'
if (_rc != 106) {
    di as err "FAIL: joinby with mismatched key types expected rc 106, got `=_rc'"
    local fails = `fails' + 1
}
parqit close _all

* --- the good path is untouched ---------------------------------------------
parqit use using `"`master'"'
parqit merge 1:1 id using `"`using'"', keepusing(rate)
if (_rc) {
    di as err "FAIL: a valid merge should still work (rc = `=_rc')"
    local fails = `fails' + 1
}
parqit collect, clear
if (_N != 10) {
    di as err "FAIL: valid merge collected `=_N' rows, expected 10"
    local fails = `fails' + 1
}
parqit close _all

if (`fails' == 0) di as result "VERDICT(REPRO_MERGE_KEY_NOT_FOUND): PASS - unknown join keys use rc 111, mismatched types use rc 106, and neither leaks a Binder Error"
else              di as error  "VERDICT(REPRO_MERGE_KEY_NOT_FOUND): FAIL - `fails' problem(s)"
