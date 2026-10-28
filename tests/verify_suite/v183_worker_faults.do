* A5 (audit of 25 Oct 2026): a worker of a parallel pass that cannot allocate,
* a worker thread that cannot be created and a worker whose own buffers cannot
* be made end the command with an error, or with the right file, and never
* kill or hang Stata. PARQIT_TEST_FAULT=<site>:<index> throws std::bad_alloc
* at one place. In parqit save, data a thread not created and a worker
* without buffers leave their blocks to the others (the file is the serial
* one, pyarrow), and the calling thread's worker fails the save loudly. In the
* SPSS and R conversions (forced onto the parallel route) a failed batch, a
* producer that cannot start and a worker not created fail the command loudly
* and publish nothing; the session
* converts the same files afterwards. Needs pyarrow from Stata's Python.
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
quietly parqit version
local expected_two = min(2,r(cpus))
local expected_four = min(4,r(cpus))
local fails 0
local spss `"`repo'/tests/fixtures/spss/survey.sav"'
local rds `"`repo'/tests/fixtures/r/frame.rds"'
tempfile ref thr wrk main sv1 sv2 sv3 rd1 rd2 rd3 l1
local tracelog `"`l1'.log"'

program define _v183_build
    clear
    set obs 1003
    gen long id = _n
    gen double x = _n / 3
    replace x = . in 17
    replace x = .a in 600
    gen str10 s = "r" + string(_n)
    gen strL sl = cond(mod(_n, 97) == 0, 2000 * "x", "s" + string(_n))
end

* ---------- parqit save, data: the serial file, then three faults -----------
python:
import os
for k in ("PARQIT_TEST_FAULT", "PARQIT_TEST_SAVE_TRACE", "PARQIT_TEST_SAVE_BLOCK_ROWS"):
    os.environ.pop(k, None)
os.environ["PARQIT_FILL_THREADS"] = "1"
end
_v183_build
qui parqit save `"`ref'.parquet"', data replace
python:
import os
os.environ["PARQIT_FILL_THREADS"] = "4"
os.environ["PARQIT_TEST_SAVE_BLOCK_ROWS"] = "8"
os.environ["PARQIT_TEST_SAVE_TRACE"] = "1"
os.environ["PARQIT_TEST_FAULT"] = "save_thread:2"
end
capture log close _v183
log using `"`tracelog'"', text replace name(_v183)
capture noisily parqit save `"`thr'.parquet"', data replace
local rc_thr = _rc
python: import os; os.environ["PARQIT_TEST_FAULT"] = "save_worker:2"
capture noisily parqit save `"`wrk'.parquet"', data replace
local rc_wrk = _rc
python: import os; os.environ["PARQIT_TEST_FAULT"] = "save_worker:0"
capture noisily parqit save `"`main'.parquet"', data replace
local rc_main = _rc
log close _v183
python:
import os
for k in ("PARQIT_TEST_FAULT", "PARQIT_TEST_SAVE_TRACE", "PARQIT_TEST_SAVE_BLOCK_ROWS", "PARQIT_FILL_THREADS"):
    os.environ.pop(k, None)
end
if (`rc_thr' | `rc_wrk') {
    di as err "FAIL: a thread not created (rc `rc_thr') or a worker without buffers (rc `rc_wrk') failed the save"
    local ++fails
}
if (`rc_main' == 0) {
    di as err "FAIL: the calling thread's worker could not start, and the save succeeded"
    local ++fails
}
capture confirm file `"`main'.parquet"'
if (_rc == 0) {
    di as err "FAIL: a failed save published its file"
    local ++fails
}
python:
from sfi import Macro, Scalar
import pyarrow.parquet as pq
ok = 1
ref = pq.read_table(Macro.getLocal("ref") + ".parquet")
for tag in ("thr", "wrk"):
    t = pq.read_table(Macro.getLocal(tag) + ".parquet")
    if not t.equals(ref) or t.schema.metadata != ref.schema.metadata:
        ok = 0
        print("FAIL: the " + tag + " save differs from the serial one")
lines = [l for l in open(Macro.getLocal("tracelog"), encoding="utf-8") if "save cell pass:" in l]
two = "blocks 126, workers " + Macro.getLocal("expected_two")
four = "blocks 126, workers " + Macro.getLocal("expected_four")
if len(lines) < 2 or two not in lines[0] or four not in lines[1]:
    ok = 0
    print("FAIL: trace", lines)
if not any("reading the dataset failed" in l for l in open(Macro.getLocal("tracelog"), encoding="utf-8")):
    ok = 0
    print("FAIL: the failed save did not say why")
Scalar.setValue("pyok", ok)
end
if (scalar(pyok) != 1) {
    di as err "FAIL: parqit save, data under faults wrote another file, ran in other workers, or did not say why"
    local ++fails
}

* ---------- the SPSS and R conversions: loud, nothing published -------------
python:
import os
os.environ["PARQIT_TEST_SPSS_BLOCK_CASES"] = "7"
os.environ["PARQIT_TEST_RDATA_BATCH_ROWS"] = "3"
os.environ["PARQIT_SPSS_ROUTE"] = "parallel"
os.environ["PARQIT_RDATA_ROUTE"] = "parallel"
os.environ["PARQIT_TEST_FAULT"] = "spss_batch:3"
end
capture noisily parqit save `"`sv1'.parquet"' using `"`spss'"', replace
local rc_sv1 = _rc
python: import os; os.environ["PARQIT_TEST_FAULT"] = "spss_driver:0"
capture noisily parqit save `"`sv2'.parquet"' using `"`spss'"', replace
local rc_sv2 = _rc
python: import os; os.environ["PARQIT_TEST_FAULT"] = "rdata_ready:0"
capture noisily parqit save `"`rd1'.parquet"' using `"`rds'"', replace
local rc_rd1 = _rc
python: import os; os.environ["PARQIT_TEST_FAULT"] = "rdata_thread:1"
capture noisily parqit save `"`rd2'.parquet"' using `"`rds'"', replace
local rc_rd2 = _rc
python: import os; os.environ.pop("PARQIT_TEST_FAULT", None)
foreach r in sv1 sv2 rd1 rd2 {
    if (`rc_`r'' == 0) {
        di as err "FAIL: `r': the conversion succeeded under a fault"
        local ++fails
    }
    capture confirm file `"``r''.parquet"'
    if (_rc == 0) {
        di as err "FAIL: `r': a failed conversion published its file"
        local ++fails
    }
}
capture noisily parqit save `"`sv3'.parquet"' using `"`spss'"', replace
local rc_sv3 = _rc
capture noisily parqit save `"`rd3'.parquet"' using `"`rds'"', replace
local rc_rd3 = _rc
python:
import os
for k in ("PARQIT_TEST_SPSS_BLOCK_CASES", "PARQIT_TEST_RDATA_BATCH_ROWS", "PARQIT_SPSS_ROUTE", "PARQIT_RDATA_ROUTE"):
    os.environ.pop(k, None)
end
if (`rc_sv3' | `rc_rd3') {
    di as err "FAIL: after the faults the session could not convert the files (rc `rc_sv3' `rc_rd3')"
    local ++fails
}

* ---------- the use fill's producer (Codex audit, round 2, finding 1) -------
* a producer that fails (fill_produce), the same with its message failing too
* (fill_word), and a worker not created whose message fails: the read fails,
* the data in memory stay, Stata goes on and reads the file afterwards
parqit set fill_threads 4
_v183_build
gen byte marker = 7
python: import os; os.environ["PARQIT_TEST_FAULT"] = "fill_produce:0"
capture noisily parqit use using `"`ref'.parquet"', clear
local rc_f1 = _rc
python: import os; os.environ["PARQIT_TEST_FAULT"] = "fill_produce:0,fill_word:0"
capture noisily parqit use using `"`ref'.parquet"', clear
local rc_f2 = _rc
python: import os; os.environ["PARQIT_TEST_FAULT"] = "fill_word:0"; os.environ["PARQIT_TEST_FAIL_THREAD_AT"] = "1"
capture noisily parqit use using `"`ref'.parquet"', clear
local rc_f3 = _rc
python: import os; os.environ.pop("PARQIT_TEST_FAULT", None); os.environ.pop("PARQIT_TEST_FAIL_THREAD_AT", None)
capture confirm variable marker
if (_rc | _N != 1003 | `rc_f1' == 0 | `rc_f2' == 0 | `rc_f3' == 0) {
    di as err "FAIL: a read under a fill fault succeeded (rc `rc_f1' `rc_f2' `rc_f3') or changed the data in memory"
    local ++fails
}
capture noisily parqit use using `"`ref'.parquet"', clear
if (_rc | _N != 1003) {
    di as err "FAIL: after the fill faults the session could not read the file (rc `=_rc')"
    local ++fails
}

if (`fails' == 0) di as result "VERDICT(v183_worker_faults): PASS"
else di as err "VERDICT(v183_worker_faults): FAIL (`fails')"
