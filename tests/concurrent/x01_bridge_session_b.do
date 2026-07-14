* Session B for x01_bridge_xproc.sh; see the session-A companion.
clear all
set more off
set varabbrev off
args repo plugin scratch
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

program define _x01_wait
    version 16.0
    args path
    local tries 0
    capture confirm file `"`path'"'
    while (_rc & `tries' < 400) {
        sleep 50
        local ++tries
        capture confirm file `"`path'"'
    }
    if (_rc) {
        di as err `"x01 session B: timed out waiting for `path'"'
        exit 603
    }
end

program define _x01_mark
    version 16.0
    args path payload
    tempname fh
    file open `fh' using `"`path'"', write text replace
    file write `fh' `"`payload'"' _n
    file close `fh'
end

_x01_wait `"`scratch'/a_open.marker"'
clear
set obs 1
gen long id = 222
gen str8 owner = "sessionB"
parqit open _data, name(masterB)
local open_bridge `"`r(bridge)'"'
if (`"`open_bridge'"' == "") {
    local old : dir `"`c(tmpdir)'"' files "_parqit_opendata_*.parquet"
    local open_bridge `"`c(tmpdir)'/`old'"'
}
_x01_mark `"`scratch'/b_open.marker"' `"`open_bridge'"'
_x01_wait `"`scratch'/a_open_checked.marker"'

parqit view masterB
parqit collect, clear
assert _N == 1 & id[1] == 222 & owner[1] == "sessionB"

preserve
clear
set obs 1
gen long id = 1
gen long payload = 222
save `"`scratch'/lookup_b.dta"', replace
restore

_x01_wait `"`scratch'/a_adapter.marker"'
parqit use using `"`scratch'/lookup_b.dta"', name(adapterB)
local adapter_bridge `"`r(bridge)'"'
if (`"`adapter_bridge'"' == "") {
    local old : dir `"`c(tmpdir)'"' files "_parqit_imp_*.parquet"
    local adapter_bridge `"`c(tmpdir)'/`old'"'
}
_x01_mark `"`scratch'/b_adapter.marker"' `"`adapter_bridge'"'

parqit view adapterB
parqit collect, clear
assert _N == 1 & id[1] == 1 & payload[1] == 222
_x01_mark `"`scratch'/b_done.marker"' "ok"
_x01_wait `"`scratch'/a_done.marker"'
parqit close _all

di as result "VERDICT(X01_BRIDGE_SESSION_B): PASS"
