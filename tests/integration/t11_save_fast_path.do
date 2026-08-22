* t11: `parqit save ..., copysource` — the explicit source-copy path.
* COPYSOURCE-1 (audit 2026-08-22): the source copy never runs automatically
* (c(changed) cannot prove the dataset equals the file); it is an opt-in that
* copies the unchanged Parquet file loaded by the last `parqit use ..., clear`,
* and every failed proof is a loud refusal. The default save always reads the
* dataset in memory.
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
local out3 `"`base'_out3.parquet"'

parqit save `"`src'"', replace data
parqit use using `"`src'"', clear
assert c(changed) == 0

* opt-in copy of the unchanged source
parqit save `"`out1'"', replace data copysource
assert r(N) == 10000 & r(k) == 4
assert `"`r(copysource)'"' != ""

* after an edit the copy is refused loudly; the default save writes memory
replace wage = 999 in 1
capture noisily parqit save `"`out2'"', replace data copysource
assert _rc == 198
capture confirm file `"`out2'"'
assert _rc != 0
parqit save `"`out2'"', replace data
assert r(N) == 10000 & r(k) == 4

* a sort leaves c(changed) at 0 but the rows are not in the file's order: the
* default save writes MEMORY (sorted) and copysource refuses (sortedby differs)
parqit use using `"`src'"', clear
gsort -id
assert c(changed) == 0
capture noisily parqit save `"`out3'"', replace data copysource
assert _rc == 198
parqit save `"`out3'"', replace data
assert r(N) == 10000

python:
from sfi import Macro
import json
import pyarrow.parquet as pq

out1 = Macro.getLocal("out1")
out2 = Macro.getLocal("out2")
out3 = Macro.getLocal("out3")
t1 = pq.read_table(out1)
t2 = pq.read_table(out2)
t3 = pq.read_table(out3)
d1 = t1.to_pydict()
d2 = t2.to_pydict()
d3 = t3.to_pydict()
md = t1.schema.metadata or {}
chars = json.loads(md.get(b"parqit.chars", b"{}").decode())
schema = {f.name: str(f.type) for f in t1.schema}
ok = (
    len(d1["id"]) == 10000
    and d1["wage"][0] != 999
    and d2["wage"][0] == 999
    and d3["id"][0] == 10000 and d3["id"][-1] == 1
    and schema["hire_date"] == "date32[day]"
    and b"parqit.schema" in md
    and chars.get("wage", {}).get("origin") == "fast-path-test"
    and "_parqit_fast_source_nonce" not in chars.get("_dta", {})
)
Macro.setLocal("ok", "1" if ok else "0")
end
assert "`ok'" == "1"

di "VERDICT(T11_SAVE_FAST_PATH): PASS - copysource copies the unchanged source on request only; edits and sorts are refused loudly; the default save writes memory"
