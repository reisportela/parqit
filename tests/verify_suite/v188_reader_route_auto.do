* V188 — A3 in the automatic route (audit of the execution, 25 Oct 2026: G4, G2).
* The SPSS and R conversions choose their route from the dictionary or the
* parsed rows by the measured rule (ASSUMPTIONS #229), and the test drives the
* rule, not a forced route: just below, at and just above a threshold, on both
* sides of the 8-thread regime. Uncompressed SPSS files of exact sizes come from
* export spss; the compressed ones are the committed fixtures with the block
* hook placing them below and above their threshold; R files of exact rows come
* from Rscript. A file that declares no case count plans no more workers than
* its blocks (G2) and loads the same data as its declared twin; truncated files
* fail in the automatic route with the serial route's rc; a one-case file loads
* what pyreadstat reads (Stata's import spss cannot read a file under
* c(tmpdir), where the suite keeps this one). The trace line names each route
* taken. Every parallel output equals the serial route's (pyarrow: cells,
* types, metadata). Needs pyarrow and pyreadstat importable from Stata's Python,
* and Rscript.
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
quietly parqit version
local allocated_cpus = r(cpus)
local fix `"`repo'/tests/fixtures/spss"'
local fails 0
tempfile stem
* the automatic route only: no forced route, block or batch hook from the suite
python: import os; [os.environ.pop(k, None) for k in ("PARQIT_SPSS_ROUTE", "PARQIT_SPSS_PROFILE", "PARQIT_RDATA_ROUTE", "PARQIT_RDATA_PROFILE", "PARQIT_TEST_SPSS_BLOCK_CASES", "PARQIT_TEST_RDATA_BATCH_ROWS", "PARQIT_TEST_SPSS_TRACE", "PARQIT_TEST_RDATA_TRACE")]

python:
from sfi import Macro
import os, shutil, struct, subprocess
stem = Macro.getLocal("stem")
ok = "1"
try:
    import pyarrow, pyreadstat
except Exception as e:
    print("oracle unavailable:", e); ok = "0"
if shutil.which("Rscript") is None:
    print("Rscript unavailable"); ok = "0"
Macro.setLocal("have_oracle", ok)
end
if ("`have_oracle'" != "1") {
    di "VERDICT(V188_READER_ROUTE_AUTO): FAIL - pyarrow, pyreadstat or Rscript unavailable"
    exit
}

* uncompressed SPSS files of 56, 57 and 64 cases (8, 9 and 10 blocks of 7)
foreach n in 56 57 64 {
    clear
    set obs `n'
    gen double x = _n / 3
    gen str5 s = "c" + string(mod(_n, 7))
    quietly export spss using `"`stem'_u`n'.sav"', replace
}
* one case, and copies that declare no count or are cut short
clear
set obs 1
gen double x = 1.5
gen str3 s = "ab"
quietly export spss using `"`stem'_one.sav"', replace
python:
fix = Macro.getLocal("fix")
pat716 = struct.pack("<iiii", 7, 16, 8, 2)
for name in ("survey.sav", "survey_bc.sav", "survey.zsav"):
    b = bytearray(open(os.path.join(fix, name), "rb").read())
    struct.pack_into("<i", b, 80, -1)                  # the header's case count
    at = b.find(pat716, 176, min(len(b), 1 << 22))      # the 64-bit count record 7/16
    if at >= 0:
        struct.pack_into("<q", b, at + 24, -1)
    base, ext = os.path.splitext(name)
    open(stem + "_" + base + "_nocount" + ext, "wb").write(bytes(b))
b = open(os.path.join(fix, "survey.sav"), "rb").read()
open(stem + "_half.sav", "wb").write(b[:len(b) // 2])
open(stem + "_m1.sav", "wb").write(b[:len(b) - 1])
# R frames of exact rows: uncompressed and gzip
def rds(path, n, compress):
    code = 'saveRDS(data.frame(x = seq_len(%d) / 3, s = rep(c("a", "bb", "c"), length.out = %d)), "%s", compress = %s)' % (n, n, path, compress)
    subprocess.run(["Rscript", "-e", code], check=True)
for n in (499999, 500000, 500001, 1999999):
    rds(stem + "_none%d.rds" % n, n, "FALSE")
for n in (999999, 1000000):
    rds(stem + "_gz%d.rds" % n, n, "TRUE")
end

program define _v188_spss
    args in out threads block
    parqit set threads `threads'
    python: import os; os.environ["PARQIT_TEST_SPSS_BLOCK_CASES"] = "`block'"; os.environ["PARQIT_TEST_SPSS_TRACE"] = "1"
    parqit save `"`out'"' using `"`in'"', replace
    python: import os; os.environ.pop("PARQIT_TEST_SPSS_BLOCK_CASES", None); os.environ.pop("PARQIT_TEST_SPSS_TRACE", None)
end
program define _v188_r
    args in out threads
    parqit set threads `threads'
    python: import os; os.environ["PARQIT_TEST_RDATA_TRACE"] = "1"
    parqit save `"`out'"' using `"`in'"', replace
    python: import os; os.environ.pop("PARQIT_TEST_RDATA_TRACE", None)
end
* the serial route's file, the oracle of every parallel one
program define _v188_serial
    args in out kind
    python: import os; os.environ["PARQIT_`kind'_ROUTE"] = "serial"
    quietly parqit save `"`out'"' using `"`in'"', replace
    python: import os; os.environ.pop("PARQIT_`kind'_ROUTE", None)
end

capture log close _v188
tempfile tl
log using `"`tl'.log"', text replace name(_v188)
* uncompressed: 9 blocks at 8 threads or more; never below 8
_v188_spss `"`stem'_u56.sav"' `"`stem'_a1.parquet"' 8 7
_v188_spss `"`stem'_u57.sav"' `"`stem'_a2.parquet"' 8 7
_v188_spss `"`stem'_u64.sav"' `"`stem'_a3.parquet"' 8 7
_v188_spss `"`stem'_u64.sav"' `"`stem'_a4.parquet"' 7 7
* compressed, 60 cases: blocks of 4 (15) and 3 (20); 17 blocks at 8 threads or
* more; below 8, bytecode needs 41 and ZLIB still 17
_v188_spss `"`fix'/survey_bc.sav"' `"`stem'_a5.parquet"' 8 4
_v188_spss `"`fix'/survey_bc.sav"' `"`stem'_a6.parquet"' 8 3
_v188_spss `"`fix'/survey.zsav"' `"`stem'_a7.parquet"' 8 4
_v188_spss `"`fix'/survey.zsav"' `"`stem'_a8.parquet"' 8 3
_v188_spss `"`fix'/survey_bc.sav"' `"`stem'_a9.parquet"' 7 3
_v188_spss `"`fix'/survey.zsav"' `"`stem'_a10.parquet"' 7 3
* no declared count (G2): 60 cases planned on the payload (plain) or on the
* profile's count (compressed, forced onto the parallel route): 9 workers of 48
_v188_spss `"`stem'_survey_nocount.sav"' `"`stem'_n1.parquet"' 48 7
python: import os; os.environ["PARQIT_SPSS_ROUTE"] = "parallel"
_v188_spss `"`stem'_survey_bc_nocount.sav"' `"`stem'_n2.parquet"' 48 7
_v188_spss `"`stem'_survey_nocount.zsav"' `"`stem'_n3.parquet"' 48 7
python: import os; os.environ.pop("PARQIT_SPSS_ROUTE", None)
* R: 500,000 rows uncompressed and 1,000,000 gzip at 8 threads or more; 2,000,000 below
_v188_r `"`stem'_none499999.rds"' `"`stem'_r1.parquet"' 8
_v188_r `"`stem'_none500000.rds"' `"`stem'_r2.parquet"' 8
_v188_r `"`stem'_none500001.rds"' `"`stem'_r3.parquet"' 8
_v188_r `"`stem'_gz999999.rds"' `"`stem'_r4.parquet"' 8
_v188_r `"`stem'_gz1000000.rds"' `"`stem'_r5.parquet"' 8
_v188_r `"`stem'_none1999999.rds"' `"`stem'_r6.parquet"' 7
log close _v188

* the serial route's files
local k 0
foreach f in `"`stem'_u56.sav"' `"`stem'_u57.sav"' `"`stem'_u64.sav"' `"`stem'_u64.sav"' `"`fix'/survey_bc.sav"' `"`fix'/survey_bc.sav"' `"`fix'/survey.zsav"' `"`fix'/survey.zsav"' `"`fix'/survey_bc.sav"' `"`fix'/survey.zsav"' {
    local ++k
    _v188_serial `"`f'"' `"`stem'_s`k'.parquet"' SPSS
}
_v188_serial `"`fix'/survey.sav"' `"`stem'_sn1.parquet"' SPSS
_v188_serial `"`fix'/survey_bc.sav"' `"`stem'_sn2.parquet"' SPSS
_v188_serial `"`fix'/survey.zsav"' `"`stem'_sn3.parquet"' SPSS
local k 0
foreach f in none499999 none500000 none500001 gz999999 gz1000000 none1999999 {
    local ++k
    _v188_serial `"`stem'_`f'.rds"' `"`stem'_rs`k'.parquet"' RDATA
}

python:
from sfi import Macro, Scalar
import numpy as np, pyarrow as pa, pyarrow.parquet as pq
stem = Macro.getLocal("stem")
ok = 1
trace = open(Macro.getLocal("tl") + ".log", encoding="utf-8").read().splitlines()
spss = [l.split(", ")[0].split()[-1] for l in trace if l.startswith("spss conversion: route ")]
cpus = int(Macro.getLocal("allocated_cpus"))
# Public AUTO policy: the high-thread regime starts at eight effective workers.
high = "parallel" if cpus >= 8 else "serial"
zlib = "parallel" if cpus > 1 else "serial"
want = ["serial", high, high, "serial", "serial", high, "serial", zlib, "serial", zlib,
        high, zlib, zlib]
if spss != want:
    print("FAIL: SPSS routes", spss, "not", want); ok = 0
nocount = [l for l in trace if l.startswith("spss conversion: route ")][-3:]
planned = "batches 9, workers " + str(min(9,cpus))
serial = "batches 0, workers 1"
expected_nocount = [planned if cpus >= 8 else serial,
                   planned if cpus > 1 else serial, planned if cpus > 1 else serial]
if len(nocount) != 3 or not all(l.endswith(want) for l,want in zip(nocount,expected_nocount)):
    print("FAIL: a file without a declared count planned other workers:", nocount); ok = 0
rdata = [l.split(", ")[0].split()[-1] for l in trace if l.startswith("rdata conversion: route ")]
want_r = ["serial", high, high, "serial", high, "serial"]
if rdata != want_r:
    print("FAIL: R routes", rdata, "not", want_r); ok = 0
def same(a, b):
    fa, fb = pq.ParquetFile(a), pq.ParquetFile(b)
    ta, tb = fa.read(), fb.read()
    if ta.schema != tb.schema or fa.schema_arrow.metadata != fb.schema_arrow.metadata or ta.num_rows != tb.num_rows:
        return False
    for n in ta.schema.names:
        ca, cb = ta[n].combine_chunks(), tb[n].combine_chunks()
        if not ca.is_valid().equals(cb.is_valid()): return False
        if pa.types.is_floating(ca.type):
            if not np.array_equal(ca.fill_null(0).to_numpy(zero_copy_only=False).view(np.uint64),
                                  cb.fill_null(0).to_numpy(zero_copy_only=False).view(np.uint64)): return False
        elif not ca.equals(cb): return False
    return True
for i in range(1, 11):
    if not same(stem + "_a%d.parquet" % i, stem + "_s%d.parquet" % i):
        print("FAIL: SPSS case", i, "differs from the serial route"); ok = 0
for i in range(1, 4):
    if not same(stem + "_n%d.parquet" % i, stem + "_sn%d.parquet" % i):
        print("FAIL: the copy without a count", i, "differs from its declared twin"); ok = 0
for i in range(1, 7):
    if not same(stem + "_r%d.parquet" % i, stem + "_rs%d.parquet" % i):
        print("FAIL: R case", i, "differs from the serial route"); ok = 0
Scalar.setValue("pyok", ok)
end
if (scalar(pyok) != 1) {
    di as err "FAIL: a route was not the rule's, or a parallel file differs from the serial one"
    local ++fails
}

* truncated files fail in the automatic route (here parallel) as in the serial one
parqit set threads 8
python: import os; os.environ["PARQIT_TEST_SPSS_BLOCK_CASES"] = "7"
foreach t in half m1 {
    capture noisily parqit save `"`stem'_t.parquet"' using `"`stem'_`t'.sav"', replace
    local rc_auto = _rc
    python: import os; os.environ["PARQIT_SPSS_ROUTE"] = "serial"
    capture noisily parqit save `"`stem'_t.parquet"' using `"`stem'_`t'.sav"', replace
    local rc_ser = _rc
    python: import os; os.environ.pop("PARQIT_SPSS_ROUTE", None)
    if (`rc_auto' == 0 | `rc_auto' != `rc_ser') {
        di as err "FAIL: `t': the automatic route ended with rc `rc_auto', the serial one with `rc_ser'"
        local ++fails
    }
}
python: import os; os.environ.pop("PARQIT_TEST_SPSS_BLOCK_CASES", None)

* one case: what pyreadstat reads
parqit use using `"`stem'_one.sav"', clear
python:
from sfi import Data, Macro, Scalar
import pyreadstat
want, meta = pyreadstat.read_sav(Macro.getLocal("stem") + "_one.sav", output_format="dict")
got = Data.get(["x", "s"])
Scalar.setValue("one_ok", int(Data.getObsTotal() == 1 and [got[0][0]] == want["x"] and [got[0][1]] == want["s"]))
end
if (scalar(one_ok) != 1) {
    di as err "FAIL: a one-case file loads other data than pyreadstat reads"
    local ++fails
}

if (`fails' == 0) di "VERDICT(V188_READER_ROUTE_AUTO): PASS"
else di "VERDICT(V188_READER_ROUTE_AUTO): FAIL (`fails')"
