* v52 — read-side hardening from the LOW tier of the 2026-07-03 audits:
*
* ENC1:    invalid UTF-8 on READ must refuse loudly. The read relies on
*          DuckDB's decoder (parqit's fill never re-validates), which today
*          rejects every malformed form — this pins that external line of
*          defense so a future engine relaxation cannot silently let raw
*          bytes into Stata (v32 pins the SAVE side only).
* META-B:  one binary (non-UTF8) sidecar KV key from a third-party writer
*          used to kill the whole parqit.* metadata read (strict decode()
*          threw; the query failure read as "no metadata"), silently losing
*          every label. try(decode()) skips just the binary key.
* META-C:  a non-numeric value-label key ("abc") strtoreal'd to missing and
*          slipped past the finite-only guard, polluting the label with a
*          `.`-keyed entry. Now skipped with a note like the other bad keys.
* N2/SCH5: the dup-name recovery aligned parquet_schema leaves positionally
*          against the scan columns, stamping bogus "duplicate column name"
*          warnings and src_name chars when nested columns (leaf "element")
*          or Hive partition layouts shifted positions. Only a genuine
*          DuckDB dedup shape (`leaf` -> `leaf_<digits>`) is recovered now.
* STR2:    a failed disk-save COPY stranded a 0-byte <dest>.parqit_tmp
*          orphan on the non-partition branch (the partition branch already
*          cleaned up).
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

* ---------- ENC1: invalid UTF-8 payloads refuse on READ ----------
tempfile ub cb
local utf `"`ub'.parquet"'
local lat1 `"`cb'.csv"'
python:
import pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro
# raw Latin-1 'café' byte (0xE9) smuggled into a string column
bad = pa.array([b'caf\xe9'], pa.binary()).cast(pa.string(), safe=False)
pq.write_table(pa.table({'s': bad}), Macro.getLocal("utf"))
with open(Macro.getLocal("lat1"), 'wb') as fh:
    fh.write(b's,v\ncaf\xe9,1\n')
end
clear
set obs 2
gen sentinel = _n
capture noisily parqit use using `"`utf'"', clear
assert _rc != 0
assert _N == 2 & sentinel[2] == 2            // memory intact
capture noisily parqit use using `"`lat1'"', clear
assert _rc != 0                              // Latin-1 CSV refused too

* ---------- META-B: a binary sidecar KV key no longer drops the labels ----
tempfile mb
local metab `"`mb'.parquet"'
python:
import json, pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro
schema = {'vars': [{'name': 'x', 'src': 'x', 'type': 'long', 'fmt': '',
                    'varlab': 'kept label', 'vallab': ''}], 'version': 1}
t = pa.table({'x': pa.array([1, 2], pa.int64())}).replace_schema_metadata({
    b'parqit.schema': json.dumps(schema).encode(),
    b'parqit.vallabs': b'{}', b'parqit.chars': b'{}',
    b'parqit.dtalabel': b'""',
    b'\xff\xfebadkey': b'\xde\xad\xbe\xef'})   # the binary intruder
pq.write_table(t, Macro.getLocal("metab"))
end
clear
parqit use using `"`metab'"', clear
assert _rc == 0
assert "`: variable label x'" == "kept label"   // was silently empty

* ---------- META-C: a non-numeric value-label key is skipped, not `.` ----
tempfile cb2
local metac `"`cb2'.parquet"'
python:
import json, pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro
schema = {'vars': [{'name': 'x', 'src': 'x', 'type': 'long', 'fmt': '',
                    'varlab': '', 'vallab': 'lab'}], 'version': 1}
vall = {'lab': {'entries': [["abc", "word"], ["2", "good"]]}}
t = pa.table({'x': pa.array([2, 2], pa.int64())}).replace_schema_metadata({
    b'parqit.schema': json.dumps(schema).encode(),
    b'parqit.vallabs': json.dumps(vall).encode(),
    b'parqit.chars': b'{}', b'parqit.dtalabel': b'""'})
pq.write_table(t, Macro.getLocal("metac"))
end
clear
parqit use using `"`metac'"', clear
assert _rc == 0
assert "`: label lab 2'" == "good"           // the good key applied
* the "abc" key must NOT have become a `.`-keyed entry: an unlabeled `.`
* renders as "." through the extended macro; the old bug rendered "word"
local dl : label lab .
assert `"`dl'"' == "."

* ---------- N2: nested/dropped columns no longer draw bogus dup warnings ----
tempfile nb
local nested `"`nb'.parquet"'
python:
import pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro
t = pa.table({'gi': pa.array([1, 2, 3], pa.int32()),
              'lst': pa.array([[1], [2], [3]], pa.list_(pa.int32())),
              'st': pa.array([{'a': 1}, {'a': 2}, {'a': 3}],
                             pa.struct([('a', pa.int32())])),
              'gs': pa.array(['p', 'q', 'r'])})
pq.write_table(t, Macro.getLocal("nested"))
end
clear
parqit use using `"`nested'"', clear
assert _rc == 0
assert c(k) == 2 & _N == 3                   // gi + gs load; lst/st drop loudly
* no bogus provenance stamped by the positional misalignment
assert `"`: char gi[src_name]'"' == ""
assert `"`: char gs[src_name]'"' == ""

* genuine duplicate names still recover with real src_name (v10 must hold)
tempfile db2
local dup `"`db2'.parquet"'
python:
import pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro
t = pa.Table.from_arrays(
    [pa.array([1, 2], pa.int32()), pa.array([3, 4], pa.int32())],
    names=['dup', 'dup'])
pq.write_table(t, Macro.getLocal("dup"))
end
clear
parqit use using `"`dup'"', clear
assert c(k) == 2
assert dup[1] == 1 & dup_1[1] == 3
assert `"`: char dup_1[src_name]'"' == "dup"

* ---------- STR2: a failed COPY leaves no .parqit_tmp orphan ----------
clear
set obs 3
gen x = _n
parqit open _data
tempfile ob
local out `"`ob'.parquet"'
capture noisily parqit save `"`out'"', replace compression(bogus)
assert _rc != 0
capture confirm file `"`out'.parqit_tmp"'
assert _rc != 0                              // the orphan is gone
capture confirm file `"`out'"'
assert _rc != 0                              // and the target was never created
parqit close _all

di "VERDICT(V52_READ_HARDENING): PASS - invalid-UTF8 read refuses (ENC1); binary KV key no longer drops labels (META-B); non-numeric vlab key skipped (META-C); no bogus dup warnings on nested files, real dups still recover (N2); no .parqit_tmp orphan (STR2)"
