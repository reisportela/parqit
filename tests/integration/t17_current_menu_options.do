* Current menu choices: inherited/explicit integer policy and seven settings.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
tempfile base
python:
import pyarrow as pa
import pyarrow.parquet as pq
from sfi import Macro
table = pa.table({"id":pa.array([9007199254740993],pa.int64()), "payload":[42]})
pq.write_table(table, Macro.getLocal("base")+".parquet")
assert pq.read_table(Macro.getLocal("base")+".parquet").column("id").to_pylist()==[9007199254740993]
end
parqit set int64 string
parqit use `"`base'.parquet"', clear
assert id == "9007199254740993"
quietly datasignature
local before `"`r(datasignature)'"'
capture parqit use `"`base'.parquet"', clear int64(refuse)
assert _rc == 198
quietly datasignature
assert `"`r(datasignature)'"' == `"`before'"'
parqit use using `"`base'.parquet"', name(wide) int64(string)
parqit set int64 refuse
parqit view wide: collect, clear
assert id == "9007199254740993"
capture parqit view wide: collect, clear int64(refuse)
assert _rc == 198
parqit close _all

* Text conversion also works for a key when the master already uses text.
clear
input str16 id
"9007199254740993"
end
parqit mergein 1:1 id using `"`base'.parquet"', int64(string)
assert _N == 1 & _merge == 3 & payload == 42
drop _merge
parqit appendin using `"`base'.parquet"', int64(string)
assert _N == 2 & id == "9007199254740993" & payload == 42

parqit set statamissing on
parqit set int64 refuse
parqit set threads 1
parqit set fill_threads 1
parqit set stream_buffer_mb 0
parqit set memory_limit 128MB
parqit set tempdir `"`c(tmpdir)'"'
parqit version
assert r(cpus) >= 1 & r(threads) == 1
assert `"`r(fill_threads)'"' == "1" & `"`r(stream_buffer_mb)'"' == "0"
parqit set fill_threads auto
parqit set stream_buffer_mb auto
parqit version
assert `"`r(fill_threads)'"' == "auto" & `"`r(stream_buffer_mb)'"' == "auto"
di "VERDICT(T17_CURRENT_MENU_OPTIONS): PASS"
