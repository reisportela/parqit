* t11: unchanged-source `parqit save ..., data` fast path.
* The fast path may copy from the original Parquet source only while the
* in-memory dataset is still unchanged; after a data edit it must fall back to
* the general Stata-memory writer.
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

set obs 10000
gen long id = _n
gen double wage = sqrt(_n) * 100
gen long hire_date = td(01jan2020) + mod(_n, 31)
format hire_date %td
gen str12 note = cond(mod(_n, 2), "odd", "even")
label variable wage "wage label"
char wage[origin] "fast-path-test"
label data "save fast path fixture"

tempfile base
local src  `"`base'.parquet"'
local out1 `"`base'_out1.parquet"'
local out2 `"`base'_out2.parquet"'

parqit save `"`src'"', replace data
parqit use using `"`src'"', clear
assert c(changed) == 0

parqit save `"`out1'"', replace data
assert r(N) == 10000 & r(k) == 4

replace wage = 999 in 1
parqit save `"`out2'"', replace data
assert r(N) == 10000 & r(k) == 4

python:
from sfi import Macro
import json
import pyarrow.parquet as pq

out1 = Macro.getLocal("out1")
out2 = Macro.getLocal("out2")
t1 = pq.read_table(out1)
t2 = pq.read_table(out2)
d1 = t1.to_pydict()
d2 = t2.to_pydict()
md = t1.schema.metadata or {}
chars = json.loads(md.get(b"parqit.chars", b"{}").decode())
schema = {f.name: str(f.type) for f in t1.schema}
ok = (
    len(d1["id"]) == 10000
    and d1["wage"][0] != 999
    and d2["wage"][0] == 999
    and schema["hire_date"] == "date32[day]"
    and b"parqit.schema" in md
    and chars.get("wage", {}).get("origin") == "fast-path-test"
    and "_parqit_fast_source_nonce" not in chars.get("_dta", {})
)
Macro.setLocal("ok", "1" if ok else "0")
end
assert "`ok'" == "1"

di "VERDICT(T11_SAVE_FAST_PATH): PASS - unchanged-source save fast path and edited-data fallback"
