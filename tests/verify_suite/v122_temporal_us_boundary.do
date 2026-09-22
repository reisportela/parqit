* Finite timestamp-us extremes: exact collect, explicit save range refusal.
clear all
set more off
set varabbrev off
set linesize 244
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
global V122_fails 0
program define _v122_check
    args ok message
    if (`ok') di as txt "PASS `message'"
    else {
        di as err "FAIL `message'"
        global V122_fails = $V122_fails + 1
    }
end
program define _v122_sentinel
    clear
    set obs 1
    gen byte sentinel = 7
end
tempfile source odd control destination saved_control save_log odd_log
python:
from pathlib import Path
import hashlib
import pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro
us = [-9223372036854775806,-9223372036854775500,-1001,-1000,-1,0,1,999,1000,1001,None]
controls = us[2:]
for name, values in [('source',us),('odd',[-9223372036854775000]),('control',controls)]:
    path = Macro.getLocal(name)+'.parquet'
    pq.write_table(pa.table({'id':range(1,len(values)+1),'ts':pa.array(values,pa.timestamp('us'))}),path)
    assert pq.read_table(path)['ts'].cast(pa.int64()).to_pylist() == values
out = Path(Macro.getLocal('destination')+'.parquet')
pq.write_table(pa.table({'sentinel':[7]}),out)
old_hash = hashlib.sha256(out.read_bytes()).hexdigest()
end
_v122_sentinel
capture noisily parqit use "`source'.parquet", clear
local rc = _rc
python:
from sfi import Data, Macro, Missing
want = [None if v is None else v//1000 + 315619200000 for v in us]
got = None
try:
    got = Data.get('ts')
    ok = len(got)==len(want) and all(Missing.isMissing(g) if w is None else g == w for g,w in zip(got,want))
except Exception as exc:
    print(f'ORACLE EXCEPTION {type(exc).__name__}: {exc}')
    ok = False
if not ok:
    print(f'ORACLE expected={want!r}; actual={got!r}')
Macro.setLocal('oracle',str(int(ok)))
end
_v122_check `=`rc' == 0 & `oracle'' "eager finite microseconds and controls"
_v122_sentinel
parqit use using "`source'.parquet"
capture noisily parqit collect, clear
local rc = _rc
capture assert `rc' == 198 & _N == 1 & sentinel[1] == 7
_v122_check `=_rc == 0' "default wide integer policy refuses atomically"
capture noisily parqit collect, clear int64(round)
local rc = _rc
python:
from sfi import Data, Macro, Missing
want = [None if v is None else v//1000 + 315619200000 for v in us]
got = None
try:
    got = Data.get('ts')
    ok = len(got)==len(want) and all(Missing.isMissing(g) if w is None else g == w for g,w in zip(got,want))
except Exception as exc:
    print(f'ORACLE EXCEPTION {type(exc).__name__}: {exc}')
    ok = False
if not ok:
    print(f'ORACLE expected={want!r}; actual={got!r}')
Macro.setLocal('oracle',str(int(ok)))
end
_v122_check `=`rc' == 0 & `oracle'' "lazy finite microseconds match Python floor"
* Flooring the earliest finite us to milliseconds exceeds Parquet's us range.
log using "`save_log'", text name(v122save)
capture noisily parqit save "`destination'.parquet", replace
local rc = _rc
log close v122save
python:
from sfi import Macro
text = Path(Macro.getLocal('save_log')).read_text(errors='replace')
range_error = 'Could not convert Timestamp(MS) to Timestamp(US)' in text
preserved = hashlib.sha256(out.read_bytes()).hexdigest() == old_hash
Macro.setLocal('range_error',str(int(range_error)))
Macro.setLocal('preserved',str(int(preserved)))
assert (us[0]//1000)*1000 < -(2**63)
end
_v122_check `=`rc' == 920 & `range_error' & `preserved'' "save range refusal preserves existing output"
parqit close _all
* Odd millisecond counts above 2^53 must still refuse, including int64(round).
_v122_sentinel
log using "`odd_log'", text name(v122odd)
capture noisily parqit use "`odd'.parquet", clear
local eager_rc = _rc
parqit use using "`odd'.parquet"
capture noisily parqit collect, clear int64(round)
local lazy_rc = _rc
log close v122odd
capture assert `eager_rc' == 920 & `lazy_rc' == 920 & _N == 1 & sentinel[1] == 7
local atomic = _rc == 0
python:
from sfi import Macro
text = Path(Macro.getLocal('odd_log')).read_text(errors='replace')
Macro.setLocal('precision_errors',str(int(text.count('not exactly representable') == 2)))
end
_v122_check `=`atomic' & `precision_errors'' "odd binary64 millisecond refusal stays atomic"
parqit close _all
parqit use using "`control'.parquet"
parqit save "`saved_control'.parquet"
python:
from sfi import Macro
want = [None if v is None else (v//1000)*1000 for v in controls]
got = pq.read_table(Macro.getLocal('saved_control')+'.parquet')['ts'].cast(pa.int64()).to_pylist()
if got != want:
    print(f'ORACLE expected={want!r}; actual={got!r}')
Macro.setLocal('oracle',str(int(got == want)))
end
_v122_check `oracle' "save null and pre/postepoch controls exactly"
parqit close _all
if ($V122_fails) di as err "VERDICT(V122_TEMPORAL_US_BOUNDARY): FAIL - $V122_fails checks"
else di as txt "VERDICT(V122_TEMPORAL_US_BOUNDARY): PASS"
macro drop V122_fails
