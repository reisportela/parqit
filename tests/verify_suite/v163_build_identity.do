* V163 — numerical capability and the identity of the loaded plugin.
clear all
set more off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
parqit version
assert r(numeric_contract) == 4
local identity `"`r(build_id)'"'
assert strlen("`identity'") == 64
assert regexm("`identity'", "^[0-9a-f]+$")
display "VERDICT(V163_BUILD_IDENTITY): PASS"

* Ado subprograms are private. Load the actual validator body under a test
* name, so its refusal cases do not depend on Stata's private namespace.
tempfile validator
tempname source target
file open `source' using `"`repo'/src/ado/p/parqit.ado"', read text
file open `target' using `"`validator'"', write text
local copying 0
local done 0
file read `source' line
while (r(eof) == 0 & !`done') {
    if (`"`macval(line)'"' == "program define _parqit_check_numeric_contract") {
        file write `target' "program define _v163_contract" _n
        local copying 1
    }
    else if (`copying') {
        file write `target' `"`macval(line)'"' _n
        if (`"`macval(line)'"' == "end") local done 1
    }
    file read `source' line
}
file close `source'
file close `target'
assert `done'
quietly run `"`validator'"'

sysuse auto, clear
tempfile original
quietly save `"`original'"'
parqit open _data
capture noisily _v163_contract 3 "`identity'"
assert _rc == 198
capture noisily _v163_contract 4 ""
assert _rc == 198
capture noisily _v163_contract 4 "not-a-source-id"
assert _rc == 198
_v163_contract 4 "`identity'"
cf _all using `"`original'"'
parqit views
assert r(n_views) == 1
display "VERDICT(V163_OLD_CONTRACT_REFUSED): PASS"

* The independent engine used by selftest must not alter an existing view.
parqit selftest
assert "`r(selftest)'" == "ok"
cf _all using `"`original'"'
parqit count
assert r(N) == 74
parqit version
assert "`r(build_id)'" == "`identity'" & r(numeric_contract) == 4
parqit close _all
display "VERDICT(V163_SELFTEST_PRESERVES_STATE): PASS"
