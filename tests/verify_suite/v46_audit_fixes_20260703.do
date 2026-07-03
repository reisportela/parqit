* v46 — adversarial-audit fixes 2026-07-03 (two independent audits, cross-checked)
*
* STR1: the disk/view `parqit save` of a %tc datetime overflowed INT32 and
*       failed with rc 920 for every instant outside ~[1969-12-07, 1970-01-25]
*       — including parqit's own files. The %tc save branch of compile_for_save
*       used `<ms> * INTERVAL 1 MILLISECOND`, whose multiplier DuckDB down-casts
*       to INT32. Now epoch_ms-based; every realistic datetime round-trips.
* N1:   a JSON-logical Parquet column reports its DuckDB type-id as VARCHAR but
*       rejects strlen(JSON)/direct projection (a binder error), so the sizing
*       scan failed and poisoned the WHOLE file (rc 920, sibling good columns
*       lost too); `parqit describe` meanwhile advertised it as loadable. The
*       VARCHAR plan now carries a CAST(... AS VARCHAR) (as ENUM/UUID already
*       did), so JSON loads as its text form and the file is usable.
*
* Oracles are independent (pyarrow reads the raw payload); never a parqit-only
* round-trip.
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

* ============================ STR1 ==================================
* Five instants spanning the old overflow window: 1900, 1960, 1970, 2020, 2035.
clear
set obs 5
gen long id = _n
gen double event = .
replace event = tc(01jan1900 00:00:00) in 1
replace event = tc(01jan1960 00:00:00) in 2
replace event = tc(01jan1970 00:00:01) in 3
replace event = tc(15mar2020 12:34:56) in 4
replace event = tc(01jan2035 00:00:00) in 5
format event %tc

* eager snapshot is the reference; the failing path is lazy-open + disk save
tempfile ebase obase
local efile `"`ebase'.parquet"'
local out   `"`obase'.parquet"'
parqit save `"`efile'"', replace data
assert _rc == 0

parqit use using `"`efile'"'
capture noisily parqit save `"`out'"', replace
assert _rc == 0                              // STR1: was rc 920

* the datetime survived the disk->disk save exactly, format preserved
parqit use using `"`out'"', clear
sort id
assert event[1] == tc(01jan1900 00:00:00)
assert event[2] == tc(01jan1960 00:00:00)
assert event[3] == tc(01jan1970 00:00:01)
assert event[4] == tc(15mar2020 12:34:56)
assert event[5] == tc(01jan2035 00:00:00)
assert "`: format event'" == "%tc"

* independent oracle: pyarrow reads the on-disk timestamps directly
python:
import pyarrow.parquet as pq
from datetime import datetime
from sfi import Macro, SFIToolkit
t = pq.read_table(Macro.getLocal("out"))
d = t.to_pydict()
pairs = dict(zip(d["id"], d["event"]))
want = {1: datetime(1900,1,1,0,0,0), 2: datetime(1960,1,1,0,0,0),
        3: datetime(1970,1,1,0,0,1), 4: datetime(2020,3,15,12,34,56),
        5: datetime(2035,1,1,0,0,0)}
ok = all(pairs[k].replace(tzinfo=None) == v for k, v in want.items())
SFIToolkit.stata('local str1_oracle = ' + ('1' if ok else '0'))
end
assert `str1_oracle' == 1                     // pyarrow confirms the instants

* %td and %tC are on other branches and must stay correct (no regression)
clear
set obs 2
gen double d = td(15mar2020) in 1
replace d = td(01jan1900) in 2
format d %td
tempfile tdbase tdobase
local tdf  `"`tdbase'.parquet"'
local tdout `"`tdobase'.parquet"'
parqit save `"`tdf'"', replace data
parqit use using `"`tdf'"'
capture noisily parqit save `"`tdout'"', replace
assert _rc == 0
parqit use using `"`tdout'"', clear
assert d[1] == td(15mar2020) & d[2] == td(01jan1900)

* ============================ N1 ===================================
tempfile jbase
local jf `"`jbase'.parquet"'
python:
import pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro
t = pa.table({'gi': pa.array([1,2,3], pa.int32()),
              'payload': pa.array(['{"a":1}','{"b":2}','{"c":3}'], pa.json_()),
              'gs': pa.array(['p','q','r'])})
pq.write_table(t, Macro.getLocal("jf"))
end

clear
capture noisily parqit use using `"`jf'"', clear
assert _rc == 0                               // N1: was rc 920
assert c(k) == 3                              // all columns present, none poisoned
assert _N == 3
* the good sibling columns are intact (not lost to the JSON column)
assert gi[1] == 1 & gi[3] == 3
assert gs[1] == "p" & gs[3] == "r"
* the JSON column loaded as its compact text form
assert payload[1] == `"{"a":1}"'
assert payload[2] == `"{"b":2}"'
assert payload[3] == `"{"c":3}"'

* describe must not fail either (it advertised the column as loadable)
capture noisily parqit describe `"`jf'"'
assert _rc == 0

* JSON alongside a dropped-type column still loads the good ones
tempfile j2base
local jf2 `"`j2base'.parquet"'
python:
import pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro
t = pa.table({'k': pa.array([9,8], pa.int64()),
              'j': pa.array(['{"x":1}','{"y":2}'], pa.json_())})
pq.write_table(t, Macro.getLocal("jf2"))
end
clear
capture noisily parqit use using `"`jf2'"', clear
assert _rc == 0
assert c(k) == 2 & k[1] == 9 & j[1] == `"{"x":1}"'

di "VERDICT(V46_AUDIT_20260703): PASS - %tc disk-save round-trips (STR1); JSON-logical column loads as text, whole file usable (N1); %td/%tC unaffected"
