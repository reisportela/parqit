* A2 (F2, audit of 25 Oct 2026): a wide SPSS file is converted in blocks of at
* most 64 MB of raw cases, down to one case, and a block reserves only the
* cases left. A floor of 2048 cases made one block of 2.7 GB, zeroed, for a
* file of 60 cases with 40 strings of width 32767; Stata peaked near 3 GB. The
* test writes that file (pandas and pyreadstat in any Python on the path; the
* checks are skipped with a note when there is none), converts it through the
* parallel route and through the serial one (one-thread profile and table
* function: the 0.5.1 code), each forced (PARQIT_SPSS_ROUTE),
* and requires the same Parquet file (pyarrow), the same data loaded (cf), two
* blocks in the trace, and on Linux a peak resident size below 500 MB.
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
parqit set threads 4
local fails 0
tempfile w tf ar nat l1
local wide `"`w'.sav"'
local tracelog `"`l1'.log"'

python:
import os, shutil, subprocess, sys
from sfi import Macro
gen = r'''
import sys
import pandas as pd, pyreadstat
ncol, nrow, width = 40, 60, 32767
cols = {}
for c in range(ncol):
    vals = []
    for r in range(nrow):
        if r == c % nrow:
            vals.append(("w%02d" % c) * (width // 3) + "x" * (width - 3 * (width // 3)))
        else:
            vals.append("r%d c%d" % (r, c))
    cols["s%02d" % c] = vals
pyreadstat.write_sav(pd.DataFrame(cols), sys.argv[1])
'''
out = Macro.getLocal("wide")
have = "0"
for exe in (sys.executable, shutil.which("python3"), shutil.which("python")):
    if not exe:
        continue
    try:
        r = subprocess.run([exe, "-c", gen, out], capture_output=True, timeout=600)
    except Exception:
        continue
    if r.returncode == 0 and os.path.isfile(out) and os.path.getsize(out) > 80000000:
        have = "1"
        break
Macro.setLocal("have_wide", have)
end
if ("`have_wide'" != "1") {
    di "VERDICT(v184_spss_wide_blocks): PASS (skipped: no Python on the path has pandas and pyreadstat to write the wide file)"
    exit
}

* ---------- the peak resident size from here on (Linux) ----------------------
* (the block size and route under test are this file's: a suite's hooks go)
python:
import os
from sfi import Macro
for k in ("PARQIT_TEST_SPSS_BLOCK_CASES", "PARQIT_SPSS_ROUTE", "PARQIT_SPSS_PROFILE", "PARQIT_SPSS_TABLE_FUNCTION"):
    os.environ.pop(k, None)
try:
    open("/proc/self/clear_refs", "w").write("5")
    Macro.setLocal("rss_check", "1")
except Exception:
    Macro.setLocal("rss_check", "0")
end

python: import os; os.environ["PARQIT_SPSS_ROUTE"] = "serial"
qui parqit save `"`tf'.parquet"' using `"`wide'"', replace
qui parqit use using `"`wide'"', clear
qui save `"`nat'"'
python: import os; os.environ["PARQIT_SPSS_ROUTE"] = "parallel"; os.environ["PARQIT_TEST_SPSS_TRACE"] = "1"
capture log close _v184
log using `"`tracelog'"', text replace name(_v184)
parqit save `"`ar'.parquet"' using `"`wide'"', replace
parqit use using `"`wide'"', clear
log close _v184
python: import os; os.environ.pop("PARQIT_SPSS_ROUTE", None); os.environ.pop("PARQIT_TEST_SPSS_TRACE", None)
capture noisily cf _all using `"`nat'"', all
if (_rc) {
    di as err "FAIL: the parallel route loads other data than the table function"
    local ++fails
}
if (_N != 60 | c(k) != 40) {
    di as err "FAIL: loaded `=_N' x `=c(k)', want 60 x 40"
    local ++fails
}

python:
from sfi import Macro, Scalar
import pyarrow.parquet as pq
ok = 1
a = pq.read_table(Macro.getLocal("ar") + ".parquet")
b = pq.read_table(Macro.getLocal("tf") + ".parquet")
if not a.equals(b) or a.schema.metadata != b.schema.metadata:
    ok = 0
    print("FAIL: the parallel route wrote another Parquet file")
trace = [l for l in open(Macro.getLocal("tracelog"), encoding="utf-8") if "spss conversion: route" in l]
if len(trace) != 2 or any("route parallel, profile parallel, batches 2, workers 2" not in l for l in trace):
    ok = 0
    print("FAIL: trace", trace)
if Macro.getLocal("rss_check") == "1":
    peak = [int(l.split()[1]) for l in open("/proc/self/status") if l.startswith("VmHWM:")][0]
    print("v184 peak resident size: %d MB" % (peak // 1024))
    if peak > 500 * 1024:
        ok = 0
        print("FAIL: the conversions peaked at %d MB" % (peak // 1024))
Scalar.setValue("pyok", ok)
end
if (scalar(pyok) != 1) {
    di as err "FAIL: another file, other blocks or too much memory"
    local ++fails
}

if (`fails' == 0) di as result "VERDICT(v184_spss_wide_blocks): PASS"
else di as err "VERDICT(v184_spss_wide_blocks): FAIL (`fails')"
