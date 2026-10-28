* SAVE-PARALLEL-1: parqit save, data reads the dataset in memory on worker
* threads over blocks of observations. With the blocks forced to 8 rows and four
* workers the parallel pass must write the payload and metadata of the serial
* pass (pyarrow, and an independent value oracle), return the same notes in the
* order of each variable's first such cell, and refuse the first bad cell in
* observation order, with the same message and code, when later blocks hold
* others; a trace line proves the pass ran in blocks.
* (python: blocks stay at top level — Stata's batch parser breaks on them
* inside foreach — so the passes are unrolled around helper programs.)
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
quietly parqit version
local expected_workers = min(4,r(cpus))
local fails 0
tempfile s1 s2 x1 x2 e1 e2 l1 l2 l3
local serial `"`s1'.parquet"'
local parallel `"`s2'.parquet"'
local xserial `"`x1'.parquet"'
local xparallel `"`x2'.parquet"'
local eserial `"`e1'.parquet"'
local eparallel `"`e2'.parquet"'
local errlog1 `"`l1'.log"'
local errlog2 `"`l2'.log"'
local tracelog `"`l3'.log"'

program define _v179_build
    clear
    set obs 1003
    gen byte b = mod(_n, 200) - 100
    gen int i = _n * 7 - 3000
    gen long l = _n * 100003
    gen float f = _n / 7
    gen double d1 = _n / 3
    replace d1 = . in 11
    replace d1 = .a in 523
    gen double d2 = sqrt(_n)
    replace d2 = .b in 37
    replace d2 = .z in 900
    gen double p = tm(2000m1) + _n
    replace p = p + 0.5 in 3
    format p %tm
    gen double tc = 1893456000000 + _n * 1000
    replace tc = tc + 0.25 in 40
    format tc %tc
    gen double dt = td(1jan2020) + _n
    replace dt = dt + 0.5 in 600
    format dt %td
    gen str12 s = "row" + string(_n)
    replace s = "caf" + char(233) in 100
    replace s = char(233) + "t" + char(233) in 701
    gen strL sl = cond(mod(_n, 97) == 0, 3000 * "x", "short" + string(_n))
    replace sl = "" in 5
    label variable d1 "first double"
    notes d2: a note on d2
end

* ---------- serial reference: one block, one worker ------------------------
python:
import os
for k in ("PARQIT_TEST_SAVE_BLOCK_ROWS", "PARQIT_TEST_SAVE_TRACE"):
    os.environ.pop(k, None)
os.environ["PARQIT_FILL_THREADS"] = "1"
end
_v179_build
qui parqit save `"`serial'"', data replace
local ext1 `"`r(ext_missing)'"'
local frac1 `"`r(frac_dates)'"'
local tc1 = r(transcoded_cells)
qui parqit save `"`xserial'"', data replace xmissing

* ---------- parallel: 8-row blocks, four workers, traced --------------------
python:
import os
os.environ["PARQIT_FILL_THREADS"] = "4"
os.environ["PARQIT_TEST_SAVE_BLOCK_ROWS"] = "8"
os.environ["PARQIT_TEST_SAVE_TRACE"] = "1"
end
capture log close _v179
log using `"`tracelog'"', text replace name(_v179)
parqit save `"`parallel'"', data replace
local ext2 `"`r(ext_missing)'"'
local frac2 `"`r(frac_dates)'"'
local tc2 = r(transcoded_cells)
log close _v179
qui parqit save `"`xparallel'"', data replace xmissing

if (`"`ext1'"' != "d2 d1" | `"`ext2'"' != `"`ext1'"') {
    di as err "FAIL: extended-missing note order: serial [`ext1'] parallel [`ext2'] (want d2 d1)"
    local ++fails
}
if (`"`frac1'"' != "p tc dt" | `"`frac2'"' != `"`frac1'"') {
    di as err "FAIL: rounded-value note order: serial [`frac1'] parallel [`frac2'] (want p tc dt)"
    local ++fails
}
if (`tc1' != 2 | `tc2' != `tc1') {
    di as err "FAIL: transcoded cells: serial `tc1' parallel `tc2' (want 2)"
    local ++fails
}

python:
from sfi import Macro, Scalar
import math, pyarrow.parquet as pq
ok = 1
trace = open(Macro.getLocal("tracelog"), encoding="utf-8").read()
if "save cell pass: blocks 126, workers " + Macro.getLocal("expected_workers") not in trace: ok = 0
pairs = (("serial", "parallel"), ("xserial", "xparallel"))
for a, b in pairs:
    ta = pq.read_table(Macro.getLocal(a)); tb = pq.read_table(Macro.getLocal(b))
    if not ta.equals(tb): ok = 0
    if ta.schema.metadata != tb.schema.metadata: ok = 0
    if pq.ParquetFile(Macro.getLocal(a)).metadata.num_rows != 1003: ok = 0
t = pq.read_table(Macro.getLocal("parallel")).to_pydict()
n = range(1, 1004)
if t["b"] != [m % 200 - 100 for m in n]: ok = 0
if t["i"] != [m * 7 - 3000 for m in n]: ok = 0
if t["l"] != [m * 100003 for m in n]: ok = 0
want_d1 = [None if m in (11, 523) else m / 3 for m in n]
if t["d1"] != want_d1: ok = 0
want_d2 = [None if m in (37, 900) else math.sqrt(m) for m in n]
if t["d2"] != want_d2: ok = 0
want_s = ["row" + str(m) for m in n]
want_s[99] = "café"
want_s[700] = "été"
if t["s"] != want_s: ok = 0
want_sl = ["x" * 3000 if m % 97 == 0 else "short" + str(m) for m in n]
want_sl[4] = ""
if t["sl"] != want_sl: ok = 0
x = pq.read_table(Macro.getLocal("xparallel"))
names = x.column_names
if "_parqit_xm_d1" not in names or "_parqit_xm_d2" not in names or "_parqit_xm_b" in names: ok = 0
xd1 = x.column("_parqit_xm_d1").to_pylist(); xd2 = x.column("_parqit_xm_d2").to_pylist()
if [i for i, c in enumerate(xd1) if c] != [522] or [i for i, c in enumerate(xd2) if c] != [36, 899]: ok = 0
if xd1[522] != 1 or xd2[36] != 2 or xd2[899] != 26: ok = 0
Scalar.setValue("pyok", ok)
end
if (scalar(pyok) != 1) {
    di as err "FAIL: parallel save diverged from the serial pass or the value oracle, or ran in one block"
    local ++fails
}

* ---------- the first bad cell in observation order wins --------------------
program define _v179_bad
    _v179_build
    gen double bad1 = td(1jan2020)
    format bad1 %td
    replace bad1 = 1e12 in 900
    gen double bad2 = tm(2000m1)
    format bad2 %tm
    replace bad2 = 1e12 in 50
    replace bad2 = 1e12 in 51
    mata: st_sstore(70, "sl", "bin" + char(0) + "ary")
end
python:
import os
os.environ["PARQIT_FILL_THREADS"] = "1"
for k in ("PARQIT_TEST_SAVE_BLOCK_ROWS", "PARQIT_TEST_SAVE_TRACE"):
    os.environ.pop(k, None)
end
_v179_bad
capture log close _v179
log using `"`errlog1'"', text replace name(_v179)
capture noisily parqit save `"`eserial'"', data replace
local rc1 = _rc
log close _v179
python:
import os
os.environ["PARQIT_FILL_THREADS"] = "4"
os.environ["PARQIT_TEST_SAVE_BLOCK_ROWS"] = "8"
end
capture log close _v179
log using `"`errlog2'"', text replace name(_v179)
capture noisily parqit save `"`eparallel'"', data replace
local rc2 = _rc
log close _v179
python:
import os
for k in ("PARQIT_FILL_THREADS", "PARQIT_TEST_SAVE_BLOCK_ROWS", "PARQIT_TEST_SAVE_TRACE"):
    os.environ.pop(k, None)
end
if (`rc1' == 0 | `rc2' != `rc1') {
    di as err "FAIL: bad cells: serial rc `rc1', parallel rc `rc2'"
    local ++fails
}
foreach f in eserial eparallel {
    capture confirm file `"``f''"'
    if (_rc == 0) {
        di as err "FAIL: a refused save published `f'"
        local ++fails
    }
}
python:
from sfi import Macro, Scalar
def errors(path):
    return [l.strip() for l in open(path, encoding="utf-8") if "parqit save:" in l]
a = errors(Macro.getLocal("errlog1")); b = errors(Macro.getLocal("errlog2"))
ok = int(len(a) == 1 and a == b and "bad2" in a[0] and "period" in a[0])
Scalar.setValue("pyerr", ok)
end
if (scalar(pyerr) != 1) {
    di as err "FAIL: the parallel save did not refuse the same first bad cell as the serial pass"
    local ++fails
}

if (`fails' == 0) di "VERDICT(v179 save parallel cells): PASS"
else di "VERDICT(v179 save parallel cells): FAIL (`fails')"
