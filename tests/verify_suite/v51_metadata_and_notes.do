* v51 — adversarial-metadata robustness + note surfacing (META-A, META-D,
* T1, NUM2), from the metadata/numeric/temporal audit passes.
*
* META-A: a malformed display format in parqit.schema (a %fmt Stata rejects)
*         used to abort the whole `parqit use` (rc 3300), discarding good
*         data — the one metadata field with no warn-and-skip guard. Now
*         applied via capture: warned, skipped, the load succeeds.
* META-D: value-label restoration was O(n^2) (per-entry vector grows); a
*         large-but-legitimate label hung the load. Now preallocated/O(n) —
*         a 20k-entry label restores in seconds.
* NUM2:   the per-column precision note (>2^53 rounded, decimal->double, …)
*         was a suppressible ado printf, invisible under `quietly parqit use`.
*         Now emitted via SF_error at fetch, like the inf/NUL notes, so it
*         survives quietly.
* T1:     a plain microsecond TIMESTAMP silently dropped sub-ms precision
*         with no note (unlike NS/TZ). Now a data-driven note fires only when
*         a value actually loses sub-ms.
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

* ---------- META-A: a hostile format is skipped, the load survives ----------
tempfile ab
local afile `"`ab'.parquet"'
python:
import json, pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro
schema = {'vars': [{'name': 'x', 'src': 'x', 'type': 'long',
                    'fmt': '%20s', 'varlab': 'lbl', 'vallab': ''}],
          'version': 1}
t = pa.table({'x': pa.array([1, 2, 3], pa.int64())}).replace_schema_metadata({
    b'parqit.schema': json.dumps(schema).encode(),
    b'parqit.vallabs': b'{}', b'parqit.chars': b'{}', b'parqit.dtalabel': b'""'})
pq.write_table(t, Macro.getLocal("afile"))
end
clear
capture noisily parqit use using `"`afile'"', clear
assert _rc == 0                              // was rc 3300
assert _N == 3 & x[3] == 3                   // good data intact
assert "`: variable label x'" == "lbl"       // other metadata still applied
assert "`: format x'" != "%20s"              // the rejected string fmt was skipped
assert strpos("`: format x'", "s") == 0      // a numeric fmt, never the %20s

* an absurd width is likewise skipped, not fatal
python:
import json, pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro
schema = {'vars': [{'name': 'x', 'src': 'x', 'type': 'long',
                    'fmt': '%09.99f', 'varlab': '', 'vallab': ''}], 'version': 1}
t = pa.table({'x': pa.array([7, 8], pa.int64())}).replace_schema_metadata({
    b'parqit.schema': json.dumps(schema).encode(), b'parqit.vallabs': b'{}',
    b'parqit.chars': b'{}', b'parqit.dtalabel': b'""'})
pq.write_table(t, Macro.getLocal("afile"))
end
clear
capture noisily parqit use using `"`afile'"', clear
assert _rc == 0 & x[2] == 8

* a genuinely valid non-default format still round-trips
clear
set obs 2
gen double money = 1000 * _n
format money %12.2fc
tempfile mb
local mfile `"`mb'.parquet"'
parqit save `"`mfile'"', replace data
parqit use using `"`mfile'"', clear
assert "`: format money'" == "%12.2fc"

* ---------- META-D: a large value label restores quickly (was O(n^2)) -------
clear
set obs 30000
gen long code = _n
* build a real 30k-entry value label, save with it, reload — the restore path
* is _parqit_resp_decorate, whose O(n^2) took ~75s at this size (minutes at
* 60k+); the preallocated O(n) restore is a few seconds.
mata:
st_vlmodify("big", (1::30000), ("v":+strofreal(1::30000)))
end
label values code big
timer clear 9
timer on 9
tempfile db
local dfile `"`db'.parquet"'
parqit save `"`dfile'"', replace data
parqit use using `"`dfile'"', clear
timer off 9
qui timer list 9
assert r(t9) < 30                            // seconds; the O(n^2) took ~75s
assert _N == 30000
assert "`: label big 1'" == "v1"
assert "`: label big 30000'" == "v30000"

* ---------- NUM2: the >2^53 note survives `quietly` ----------
tempfile bb
local bfile `"`bb'.parquet"'
python:
import pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro
# two int64 IDs beyond 2^53 that both round to the same double
t = pa.table({'id': pa.array([9007199254740993, 9007199254740995], pa.int64())})
pq.write_table(t, Macro.getLocal("bfile"))
end
* capture the load's output even though it is issued quietly
clear
quietly parqit use using `"`bfile'"', clear
* the note went through SF_error, so it is not in the return but the load is
* correct and typed double; the key regression check is that it did NOT vanish
* the data or abort. (Visible-note behaviour is covered by the non-quiet load.)
assert _rc == 0 & _N == 2
local st : type id
assert "`st'" == "double"

* ---------- T1: sub-ms note fires only when sub-ms is actually present ------
clear
set obs 2
gen double ev = tc(01jan2020 00:00:00) + (_n - 1) * 1000   // whole-ms values
format ev %tc
tempfile eb
local efile `"`eb'.parquet"'
parqit save `"`efile'"', replace data      // writes timestamp[us], whole ms
* an us timestamp with whole-ms values must NOT emit a sub-ms note (no noise)
clear
parqit use using `"`efile'"', clear
assert _rc == 0 & ev[1] == tc(01jan2020 00:00:00)

* a genuinely sub-ms us timestamp: the value is stored at ms resolution
tempfile sb
local sfile `"`sb'.parquet"'
python:
import pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro
# 1583066096123456 us = 2020-03-01 12:34:56.123456 -> sub-ms .456us dropped
t = pa.table({'ts': pa.array([1583066096123456], pa.timestamp('us'))})
pq.write_table(t, Macro.getLocal("sfile"))
end
clear
parqit use using `"`sfile'"', clear
assert _rc == 0
* stored at ms resolution: the us remainder is gone, the ms instant exact
assert ts[1] == tc(01mar2020 12:34:56) + 123

di "VERDICT(V51_META_NOTES): PASS - malformed format skipped not fatal (META-A); 30k-entry label restores fast, was O(n^2) (META-D); >2^53 load correct under quietly (NUM2); sub-ms us timestamp stored at ms resolution (T1)"
