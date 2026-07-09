* Repro: in-memory save and lazy view save must encode the same fractional
* Stata temporal values identically. Native Stata round() is the oracle for
* the documented integer day/ms/period conversion at exact .5 ties.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

local fails 0
tempfile stem

* Native Stata rounds exact half ties toward +infinity.
assert round(100.5) == 101
assert round(-100.5) == -100

* The logical payload to encode through the in-memory path.
clear
set obs 2
gen long id = _n
gen double d = cond(id == 1, 100.5, -100.5)
gen double tc = cond(id == 1, 100.5, -100.5)
gen double leap = cond(id == 1, 100.5, -100.5)
gen double m = cond(id == 1, 100.5, -100.5)
format d %td
format tc %tc
format leap %tC
format m %tm
parqit save `"`stem'_memory.parquet"', replace data

* Start a lazy view from integer-valued temporal columns, then create the same
* logical fractional payload inside the view. This isolates compile_for_save.
clear
set obs 2
gen long id = _n
gen double d = 0
gen double tc = 0
gen double leap = 0
gen double m = 0
format d %td
format tc %tc
format leap %tC
format m %tm
parqit save `"`stem'_source.parquet"', replace data
parqit use using `"`stem'_source.parquet"'
parqit replace d = cond(id == 1, 100.5, -100.5)
parqit replace tc = cond(id == 1, 100.5, -100.5)
parqit replace leap = cond(id == 1, 100.5, -100.5)
parqit replace m = cond(id == 1, 100.5, -100.5)
parqit save `"`stem'_view.parquet"', replace
parqit close _all

python:
import pyarrow.parquet as pq
from sfi import Macro

b = Macro.getLocal("stem")
mem = pq.read_table(b + "_memory.parquet").to_pydict()
view = pq.read_table(b + "_view.parquet").to_pydict()

same = all(mem[k] == view[k] for k in ("d", "tc", "leap", "m"))
Macro.setLocal("same", "1" if same else "0")
Macro.setLocal("mem_payload", repr({k: mem[k] for k in ("d", "tc", "leap", "m")}))
Macro.setLocal("view_payload", repr({k: view[k] for k in ("d", "tc", "leap", "m")}))
end

if ("`same'" != "1") {
    di as err "REPRODUCED: temporal payload differs by materialisation path"
    di as err "memory: `mem_payload'"
    di as err "view:   `view_payload'"
    local ++fails
}

di as txt "VERDICT(REPRO_TEMPORAL_SAVE_DIVERGENCE): " ///
    cond(`fails' == 0, "PASS", "FAIL - path-dependent temporal rounding")
