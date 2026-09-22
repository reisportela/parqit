* DuckDB DATE/TIMESTAMP infinities must refuse atomically at Stata boundaries.
clear all
set more off
set varabbrev off
set linesize 244
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
global V123_fails 0
program define _v123_check
    args ok message
    if (`ok') di as txt "PASS `message'"
    else {
        di as err "FAIL `message'"
        global V123_fails = $V123_fails + 1
    }
end
program define _v123_sentinel
    clear
    set obs 1
    gen byte sentinel = 7
end
program define _v123_logcount, rclass
    args path
    tempname fh
    local found 0
    file open `fh' using "`path'", read text
    file read `fh' line
    while (r(eof) == 0) {
        if (strpos(`"`line'"', "cannot be represented in Stata")) local ++found
        file read `fh' line
    }
    file close `fh'
    return scalar found = `found'
end
tempfile source destination control saved_control
python:
from pathlib import Path
import hashlib
import pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro
stem = Macro.getLocal('source')
for kind,arrow,limit in [('date',pa.date32(),2**31-1),('timestamp',pa.timestamp('us'),2**63-1),('nanosecond',pa.timestamp('ns'),2**63-1)]:
    for sign,value in [('negative',-limit),('positive',limit)]:
        pq.write_table(pa.table({'x':pa.array([0,value,None],arrow)}),stem+'_'+kind+'_'+sign+'.parquet')
out=Path(Macro.getLocal('destination')+'.parquet')
pq.write_table(pa.table({'sentinel':[7]}),out)
old_hash=hashlib.sha256(out.read_bytes()).hexdigest()
pq.write_table(pa.table({'d':pa.array([-1,0,1,None],pa.date32()),'ts':pa.array([-1,0,1,None],pa.timestamp('us'))}),Macro.getLocal('control')+'.parquet')
end
foreach workers in 1 2 {
    parqit set fill_threads `workers'
    foreach kind in date timestamp nanosecond {
        foreach sign in negative positive {
            tempfile errors
            log using "`errors'", text name(v123errors)
            _v123_sentinel
            capture noisily parqit use "`source'_`kind'_`sign'.parquet", clear
            local rc = _rc
            capture assert `rc' == 920 & _N == 1 & c(k) == 1 & sentinel[1] == 7
            local eager_ok = _rc == 0
            _v123_sentinel
            parqit use using "`source'_`kind'_`sign'.parquet"
            capture noisily parqit collect, clear int64(round)
            local rc = _rc
            capture assert `rc' == 920 & _N == 1 & c(k) == 1 & sentinel[1] == 7
            local lazy_ok = _rc == 0
            capture noisily parqit save "`destination'.parquet", replace
            local save_rc = _rc
            parqit close _all
            log close v123errors
            _v123_logcount "`errors'"
            local messages_ok = r(found) == 3
            _v123_check `eager_ok' "`kind' `sign' eager atomic, workers=`workers'"
            _v123_check `lazy_ok' "`kind' `sign' lazy atomic, workers=`workers'"
            _v123_check `=`save_rc' == 920 & `messages_ok'' "`kind' `sign' save refuses; all errors name infinity"
        }
    }
}
python:
from sfi import Macro
preserved=hashlib.sha256(out.read_bytes()).hexdigest()==old_hash
Macro.setLocal('preserved',str(int(preserved)))
end
_v123_check `preserved' "all failed saves preserve destination bytes"
parqit set fill_threads auto
parqit use "`control'.parquet", clear
capture assert _N == 4 & d[1] == 3652 & d[2] == 3653 & d[3] == 3654 & missing(d[4]) & ts[1] == 315619199999 & ts[2] == 315619200000 & ts[3] == 315619200000 & missing(ts[4])
_v123_check `=_rc == 0' "finite and null temporal controls eager"
parqit use using "`control'.parquet"
parqit collect, clear
capture assert _N == 4 & d[1] == 3652 & d[2] == 3653 & d[3] == 3654 & missing(d[4]) & ts[1] == 315619199999 & ts[2] == 315619200000 & ts[3] == 315619200000 & missing(ts[4])
_v123_check `=_rc == 0' "finite and null temporal controls lazy"
parqit save "`saved_control'.parquet"
python:
from sfi import Macro
result=pq.read_table(Macro.getLocal('saved_control')+'.parquet')
actual_d=result['d'].cast(pa.int32()).to_pylist()
actual_ts=result['ts'].cast(pa.int64()).to_pylist()
ok=actual_d==[-1,0,1,None] and actual_ts==[-1000,0,0,None]
if not ok:
    print('ORACLE expected date=',[-1,0,1,None],'actual=',actual_d)
    print('ORACLE expected timestamp us=',[-1000,0,0,None],'actual=',actual_ts)
Macro.setLocal('oracle',str(int(ok)))
end
_v123_check `oracle' "finite and null temporal save controls match PyArrow"
parqit close _all
if ($V123_fails) di as err "VERDICT(V123_TEMPORAL_INFINITY): FAIL - $V123_fails checks"
else di as txt "VERDICT(V123_TEMPORAL_INFINITY): PASS"
macro drop V123_fails
