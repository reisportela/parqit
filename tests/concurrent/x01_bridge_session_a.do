* Session A for x01_bridge_xproc.sh.  It deliberately keeps each lazy bridge
* open while session B performs the same first operation in the same TMPDIR.
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
        di as err `"x01 session A: timed out waiting for `path'"'
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

* First collision surface: parqit open _data.
clear
set obs 1
gen long id = 111
gen str8 owner = "sessionA"
parqit open _data, name(masterA)
local open_bridge `"`r(bridge)'"'
* Baseline compatibility: before BRIDGE-XPROC-1 is fixed, open did not return
* its path.  Resolve the one old-style file so this test reproduces the actual
* cross-process collision rather than failing merely on a missing r() result.
if (`"`open_bridge'"' == "") {
    local old : dir `"`c(tmpdir)'"' files "_parqit_opendata_*.parquet"
    local open_bridge `"`c(tmpdir)'/`old'"'
}
_x01_mark `"`scratch'/a_open.marker"' `"`open_bridge'"'
_x01_wait `"`scratch'/b_open.marker"'

parqit view masterA
parqit collect, clear
assert _N == 1 & id[1] == 111 & owner[1] == "sessionA"
_x01_mark `"`scratch'/a_open_checked.marker"' "ok"

* Second collision surface: a lazy .dta adapter bridge.
preserve
clear
set obs 1
gen long id = 1
gen long payload = 111
save `"`scratch'/lookup_a.dta"', replace
restore

parqit use using `"`scratch'/lookup_a.dta"', name(adapterA)
local adapter_bridge `"`r(bridge)'"'
if (`"`adapter_bridge'"' == "") {
    local old : dir `"`c(tmpdir)'"' files "_parqit_imp_*.parquet"
    local adapter_bridge `"`c(tmpdir)'/`old'"'
}
_x01_mark `"`scratch'/a_adapter.marker"' `"`adapter_bridge'"'
_x01_wait `"`scratch'/b_adapter.marker"'

parqit view adapterA
parqit collect, clear
assert _N == 1 & id[1] == 1 & payload[1] == 111
_x01_mark `"`scratch'/a_done.marker"' "ok"
_x01_wait `"`scratch'/b_done.marker"'
parqit close _all

di as result "VERDICT(X01_BRIDGE_SESSION_A): PASS"
