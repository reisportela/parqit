* V41 — TYPE-1: storage types must round-trip exactly through parqit.
* A byte variable saved by parqit came back as int on every read path: the
* refine_plan period-format guard ("period/date formats keep their integer
* storage wide enough") fired for ANY non-empty display format, and
* parqit-written files always carry the fmt in parqit.schema. Foreign files
* (no fmt recorded) masked the bug because range refinement sized them
* correctly. Fixed: the guard now widens only genuine %t* format classes.
*
* Pins: (a) byte/int/long/float/double/str round-trip storage types on the
* eager-use, lazy-collect and view-save->use paths; (b) a plain display
* format (%9.2f) never widens byte; (c) a period format (%tq) still keeps
* integer storage >= int (design, charter 6.3); (d) an all-missing byte
* stays byte; (e) the on-disk physical types are the narrow ones (pyarrow
* oracle) so the file is honest for third-party readers too.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

local fails 0
tempfile f
tempfile g

* fixture: every numeric storage type + strings, labels, formats
program define mkfix
    clear
    set obs 5
    gen byte   b     = _n - 2
    gen int    i     = _n * 300
    gen long   l     = _n * 100000
    gen float  fl    = _n / 8
    gen double d     = _n / 8
    gen str4   s     = "ab" + string(_n)
    gen byte   bmiss = .
    gen byte   blab  = _n
    label define v41lbl 1 "one" 2 "two" 3 "three" 4 "four" 5 "five", replace
    label values blab v41lbl
    gen byte   bfmt  = _n
    format bfmt %9.2f
    gen byte   bq    = _n
    format bq %tq
end

program define checktypes, rclass
    * expected: b byte, i int, l long, fl float, d double, s str4,
    *           bmiss byte, blab byte, bfmt byte, bq int (%tq keeps >= int)
    local bad ""
    foreach spec in "b byte" "i int" "l long" "fl float" "d double" ///
                    "s str4" "bmiss byte" "blab byte" "bfmt byte" "bq int" {
        gettoken v want : spec
        local want = strtrim("`want'")
        if "`:type `v''" != "`want'" local bad "`bad' `v'(`:type `v''!=`want')"
    }
    return local bad "`strtrim("`bad'")'"
end

mkfix
parqit save `"`f'.parquet"', replace data

* ---- ORACLE : physical parquet types must be the narrow ones ----------------
local phys_ok 0
python:
import pyarrow.parquet as pq
from sfi import Macro
sch = pq.ParquetFile(Macro.getLocal("f") + ".parquet").schema_arrow
want = {"b": "int8", "i": "int16", "l": "int32", "fl": "float",
        "d": "double", "s": "string", "bmiss": "int8", "blab": "int8",
        "bfmt": "int8", "bq": "int32"}
got = {fld.name: str(fld.type) for fld in sch}
Macro.setLocal("phys_ok", "1" if all(got.get(k) == v for k, v in want.items()) else "0")
Macro.setLocal("phys_got", "; ".join(f"{k}={v}" for k, v in sorted(got.items())))
end
capture assert "`phys_ok'" == "1"
if (_rc) di as err "FAIL oracle: physical types wrong: `phys_got'"
local fails = `fails' + (_rc!=0)

* ---- CASE 1 : eager use ------------------------------------------------------
parqit use using `"`f'.parquet"', clear
checktypes
if `"`r(bad)'"' != "" di as err `"FAIL c1 (eager use): `r(bad)'"'
local fails = `fails' + (`"`r(bad)'"' != "")

* ---- CASE 2 : lazy use -> collect --------------------------------------------
clear
parqit use using `"`f'.parquet"'
parqit collect, clear
checktypes
if `"`r(bad)'"' != "" di as err `"FAIL c2 (lazy collect): `r(bad)'"'
local fails = `fails' + (`"`r(bad)'"' != "")
parqit close

* ---- CASE 3 : view -> save (disk-to-disk) -> eager use ------------------------
parqit use using `"`f'.parquet"'
parqit save `"`g'.parquet"', replace
parqit close
parqit use using `"`g'.parquet"', clear
checktypes
if `"`r(bad)'"' != "" di as err `"FAIL c3 (view save->use): `r(bad)'"'
local fails = `fails' + (`"`r(bad)'"' != "")

* ---- CASE 4 : repeated save/use cycles must not drift -------------------------
mkfix
forvalues k = 1/3 {
    parqit save `"`f'.parquet"', replace data
    parqit use using `"`f'.parquet"', clear
}
checktypes
if `"`r(bad)'"' != "" di as err `"FAIL c4 (3x cycle drift): `r(bad)'"'
local fails = `fails' + (`"`r(bad)'"' != "")

* ---- CASE 5 : payload identical after round-trip (not just the types) ---------
mkfix
tempfile native
save `"`native'"'
parqit save `"`f'.parquet"', replace data
parqit use using `"`f'.parquet"', clear
capture cf _all using `"`native'"'
if (_rc) di as err "FAIL c5: payload differs after round-trip (rc=`=_rc')"
local fails = `fails' + (_rc!=0)

* ---- verdict -------------------------------------------------------------------
if (`fails' == 0) di as res "VERDICT(V41_TYPE_FIDELITY): PASS - storage types round-trip exactly; display formats never widen"
else di as err "VERDICT(V41_TYPE_FIDELITY): FAIL - `fails' case(s)"
