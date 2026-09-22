* Nanoseconds near INT64_MIN floor safely to the earlier millisecond.
clear all
set more off
set varabbrev off
set linesize 244
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
tempfile source dest
local fails 0
python:
import pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro
ns = [-9223372036854775807,-9223372036854775500,-9223372036854775001,-9223372036854775000,-1000001,-1000000,-1001,-1000,-999,-1,0,1,999,1000,1001,999999,1000000,1000001,9223372036854775806,None]
pq.write_table(pa.table({'id':range(1,len(ns)+1),'ts':pa.array(ns,pa.timestamp('ns'))}),Macro.getLocal('source')+'.parquet')
assert pq.read_table(Macro.getLocal('source')+'.parquet')['ts'].cast(pa.int64()).to_pylist() == ns
end
set obs 2
gen byte sentinel = _n
capture noisily parqit use "`source'.parquet", clear
local rc = _rc
python:
from sfi import Data, Macro, Missing
want = [None if v is None else v//1000000 + 315619200000 for v in ns]
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
if (`rc' != 0 | !`oracle') {
    di as err "FAIL eager nanosecond extremes (rc `rc')"
    local ++fails
}
else di as txt "PASS eager nanosecond extremes"
parqit use using "`source'.parquet"
capture noisily parqit collect, clear
local rc = _rc
python:
from sfi import Data, Macro, Missing
want = [None if v is None else v//1000000 + 315619200000 for v in ns]
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
if (`rc' != 0 | !`oracle') {
    di as err "FAIL lazy collect nanosecond extremes (rc `rc')"
    local ++fails
}
else di as txt "PASS lazy collect nanosecond extremes"
capture noisily parqit save "`dest'.parquet"
local rc = _rc
python:
from sfi import Macro
want = [None if v is None else (v//1000000)*1000 for v in ns]
got = None
try:
    table = pq.read_table(Macro.getLocal('dest')+'.parquet')
    got = table['ts'].cast(pa.timestamp('us')).cast(pa.int64()).to_pylist()
    ok = got == want
except Exception as exc:
    print(f'ORACLE EXCEPTION {type(exc).__name__}: {exc}')
    ok = False
if not ok:
    print(f'ORACLE expected={want!r}; actual={got!r}')
Macro.setLocal('oracle',str(int(ok)))
end
if (`rc' != 0 | !`oracle') {
    di as err "FAIL lazy save nanosecond extremes (rc `rc')"
    local ++fails
}
else di as txt "PASS lazy save nanosecond extremes"
parqit close _all
if (`fails') di as err "VERDICT(V119_NANOSECOND_EXTREMES): FAIL - `fails' checks"
else di as txt "VERDICT(V119_NANOSECOND_EXTREMES): PASS"
