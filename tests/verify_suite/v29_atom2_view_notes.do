* V29 — ATOM-2: the view-save paths emit the same lossy-conversion notes as the
* in-memory save_data path, and the on-disk payload matches.
*
*   A  parqit open _data surfaces the bridge snapshot's extended-missing (.a-.z ->
*      null) and fractional-date (rounded) notes that `qui' used to swallow.
*   B  a save through a view produces the byte-identical payload the in-memory
*      save would (same lossy conversions), proven with pyarrow.
*   C  a PURE view save (no open _data) emits the fractional date/period note
*      when compile_for_save's round() changes a value — e.g. a %td column made
*      fractional by parqit replace — and does NOT invent an extended-missing note
*      (a view over a Parquet source has none); the value is rounded on disk.
*   D  a clean integer-date view save stays silent: no spurious note, and the
*      loss already reported at open _data is never double-warned.
*
* Every on-disk check uses an independent oracle (pyarrow), never parqit alone.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

* Does a text log contain a (literal, non-regex) substring? r(found)=1/0.
capture program drop _v29_loghas
program define _v29_loghas, rclass
    version 16.0
    gettoken lf 0 : 0
    local pat = strtrim(`"`0'"')
    tempname fh
    local found 0
    file open `fh' using `"`lf'"', read text
    file read `fh' line
    while (r(eof)==0) {
        if (strpos(`"`line'"', `"`pat'"')) local found 1
        file read `fh' line
    }
    file close `fh'
    return scalar found = `found'
end

local fails 0
tempfile t

* ---- A) parqit open _data surfaces BOTH notes -------------------------------
clear
set obs 3
gen long id = _n
gen double d = 100.5          // fractional %td value
format d %td
replace d = .a in 2           // extended missing .a
log using `"`t'_A.log"', replace text name(v29A)
parqit open _data
log close v29A
parqit close _all
_v29_loghas `"`t'_A.log"' extended missing values
local A_ext = r(found)
_v29_loghas `"`t'_A.log"' non-integer date/period values
local A_frac = r(found)
capture assert `A_ext'==1 & `A_frac'==1
if (_rc) di as err "FAIL ATOM-2(open _data): notes missing (ext=`A_ext' frac=`A_frac')"
local fails = `fails' + (_rc!=0)

* ---- B) view save payload == in-memory save payload -----------------------
clear
set obs 3
gen long id = _n
gen double d = 100.5
format d %td
replace d = .a in 2
parqit save `"`t'_mem.parquet"', replace data      // in-memory reference
clear
set obs 3
gen long id = _n
gen double d = 100.5
format d %td
replace d = .a in 2
parqit open _data
parqit save `"`t'_view.parquet"', replace           // through the view
parqit close _all
python:
import pyarrow.parquet as pq
from sfi import Macro
b = Macro.getLocal("t")
mem  = pq.read_table(b+"_mem.parquet").to_pydict()
view = pq.read_table(b+"_view.parquet").to_pydict()
ok = (mem["d"]==view["d"]) and (view["d"][1] is None) and (view["d"][0]==view["d"][2])
Macro.setLocal("Bok", "1" if ok else "0")
end
if ("`Bok'"!="1") di as err "FAIL ATOM-2(payload): view save != in-memory save"
local fails = `fails' + ("`Bok'"!="1")

* ---- C) pure view save: frac note fires, no ext note, value rounded on disk -
clear
set obs 3
gen long id = _n
gen double d = 100            // integer %td value
format d %td
parqit save `"`t'_src.parquet"', replace data
parqit use using `"`t'_src.parquet"'
parqit replace d = d + 0.3      // d now fractional, still %td-formatted
log using `"`t'_C.log"', replace text name(v29C)
parqit save `"`t'_out.parquet"', replace
log close v29C
parqit close _all
_v29_loghas `"`t'_C.log"' non-integer date/period values
local C_frac = r(found)
_v29_loghas `"`t'_C.log"' extended missing values
local C_ext = r(found)
python:
import datetime, pyarrow.parquet as pq
from sfi import Macro
b = Macro.getLocal("t")
out = pq.read_table(b+"_out.parquet").to_pydict()
ok = all(x==datetime.date(1960,4,10) for x in out["d"])   # 100.3 rounds to 100
Macro.setLocal("Cok", "1" if ok else "0")
end
capture assert `C_frac'==1 & `C_ext'==0 & "`Cok'"=="1"
if (_rc) di as err "FAIL ATOM-2(view frac): frac=`C_frac' ext=`C_ext' disk=`Cok'"
local fails = `fails' + (_rc!=0)

* ---- D) clean integer-date view save stays silent (no spurious note) ------
clear
set obs 3
gen long id = _n
gen double d = 100
format d %td
parqit save `"`t'_clean.parquet"', replace data
parqit use using `"`t'_clean.parquet"'
log using `"`t'_D.log"', replace text name(v29D)
parqit save `"`t'_cleanout.parquet"', replace
log close v29D
parqit close _all
_v29_loghas `"`t'_D.log"' non-integer date/period values
local D_frac = r(found)
_v29_loghas `"`t'_D.log"' extended missing values
local D_ext = r(found)
capture assert `D_frac'==0 & `D_ext'==0
if (_rc) di as err "FAIL ATOM-2(spurious): clean integer-date view save emitted a note"
local fails = `fails' + (_rc!=0)

di as txt "VERDICT(V29_ATOM2_VIEW_NOTES): " cond(`fails'==0, "PASS", "FAIL — `fails' failures") ///
    " - open_data notes / view==memory payload / view frac note (no ext) / no spurious note"
