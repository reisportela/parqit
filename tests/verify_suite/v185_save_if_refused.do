* F6 (audit of 25 Oct 2026): the direct writer behind parqit save, data never
* receives if/in from the ado (SAVE-RANGE-1 refuses in). An observation an if
* excludes, reaching the plugin all the same, used to be written as a valid 0;
* the save is now refused, loudly, with nothing published. The plugin is called
* through its private interface with the request the ado builds, and an if.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
* the ado's Mata functions and the plugin, as v57 loads them, to build the
* request by hand
quietly do `"`repo'/src/ado/p/parqit.ado"'
program parqit_plugin, plugin using(`"`plugin'"')
local fails 0
tempfile out req
clear
set obs 10
gen long id = _n
gen double x = _n / 2
* the request the ado builds for this dataset (parqit save, data replace)
local _sq_dest `"`out'.parquet"'
local _sq_replace = 1
local _sq_comp ""
local _sq_complevel = -1
local _sq_partition ""
local _sq_pmode ""
local _sq_chunk = -1
local _sq_encoding ""
local _sq_encall = 0
local _sq_dtalabel ""
local _sq_sortedby ""
local _sq_direct = 0
local _sq_xmissing = 0
qui label dir
local _sq_vallabs_all `"`r(names)'"'
mata: _parqit_wr_save_request("`req'")
capture noisily plugin call parqit_plugin id x if mod(id, 2), save_data `reqhex'
local rc = _rc
if (`rc' == 0) {
    di as err "FAIL: a save_data call with an if succeeded"
    local ++fails
}
capture confirm file `"`out'.parquet"'
if (_rc == 0) {
    di as err "FAIL: the refused save published its file"
    local ++fails
}
* the same request without the if writes every observation
capture noisily plugin call parqit_plugin id x, save_data `reqhex'
if (_rc) {
    di as err "FAIL: the same request without an if failed (rc `=_rc')"
    local ++fails
}
python:
from sfi import Macro, Scalar
import pyarrow.parquet as pq
t = pq.read_table(Macro.getLocal("out") + ".parquet")
Scalar.setValue("pyok", int(t.column("id").to_pylist() == list(range(1, 11)) and
                            t.column("x").to_pylist() == [n / 2 for n in range(1, 11)]))
end
if (scalar(pyok) != 1) {
    di as err "FAIL: the save without an if wrote other values"
    local ++fails
}

if (`fails' == 0) di as result "VERDICT(v185_save_if_refused): PASS"
else di as err "VERDICT(v185_save_if_refused): FAIL (`fails')"
