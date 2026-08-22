* Repro (D1, ROWCTX-1): _n/_N in the read-only filters used to reach DuckDB.
*
* The translator emits __PARQIT_ROW__/__PARQIT_NROWS__ placeholders that only
* the view compiler resolves, and it resolves them where a plan STAGE is
* appended (keep if/drop if, View::gen). `count if` and the list/head preview
* apply their filter to an ALREADY-COMPILED SELECT, so the placeholder used to
* survive into the query and come back as:
*
*   parqit count: Binder Error: Referenced column "__PARQIT_ROW__" not found
*   in FROM clause! ... rc 920
*
* — an internal name in a user-facing message (charter §5/§6.12) for a
* limitation parqit.sthlp already documents. Both call sites now check
* ExprResult::uses_rowctx and refuse with parqit's own message and rc 198.
* Pinned by tests/verify_suite/v67_runtime_message_contract.do.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

local fails 0
tempfile stem
local src `"`stem'_src.parquet"'

clear
set obs 10
gen long id = _n
gen double x = _n * 2
parqit save `"`src'"', replace data

parqit use using `"`src'"'
parqit sort id

* --- the two refused read-only filters --------------------------------------
capture noisily parqit count if _n <= 5
if (_rc != 198) {
    di as err "FAIL: count if _n expected rc 198, got `=_rc'"
    local fails = `fails' + 1
}
capture noisily parqit list if _N > 1
if (_rc != 198) {
    di as err "FAIL: list if _N expected rc 198, got `=_rc'"
    local fails = `fails' + 1
}

* --- the refusal must not have touched the view -----------------------------
parqit count
if (r(N) != 10) {
    di as err "FAIL: a refused filter changed the view (N = `=r(N)')"
    local fails = `fails' + 1
}

* --- the supported contexts still work --------------------------------------
parqit keep if _n <= 8
if (_rc) {
    di as err "FAIL: keep if _n should still work (rc = `=_rc')"
    local fails = `fails' + 1
}
parqit gen double rn = _n
if (_rc) {
    di as err "FAIL: gen = _n should still work (rc = `=_rc')"
    local fails = `fails' + 1
}
parqit count
if (r(N) != 8) {
    di as err "FAIL: keep if _n did not slice (N = `=r(N)')"
    local fails = `fails' + 1
}
parqit close _all

if (`fails' == 0) di as result "VERDICT(REPRO_COUNTIF_ROWCTX): PASS - read-only _n/_N filters refuse with rc 198, no engine internals leak"
else              di as error  "VERDICT(REPRO_COUNTIF_ROWCTX): FAIL - `fails' problem(s)"
