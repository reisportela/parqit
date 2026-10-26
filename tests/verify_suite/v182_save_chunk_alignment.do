* F1 (2026-10-25): parqit save, data cuts the dataset into blocks of whole
* requested row groups AND whole bytes of the validity bitmap. With chunk() not
* a multiple of 8 the blocks started inside a byte, and the missing values of
* every later block landed on the wrong rows, silently (98,499 wrong cells in
* 1.2 million). No block-size hook here: 2,500,003 observations give every
* chunk() below at least two blocks, and the last bitmap byte is partial. The
* missing values sit periodically, over whole blocks and on the rows around the
* old and the new block boundaries; pyarrow checks every null position, every
* value bit and every extended-missing code against the rule that made them,
* for 1, 4 and automatic fill workers, through the int32-offset fallback, and
* without xmissing; a trace gives the blocks and the workers that ran.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
local fails 0
tempfile base out l1
local tracelog `"`l1'.log"'
local outstem `"`out'"'

* ---------- the data: boundary rows of the old and the new blocks ----------
clear
set obs 2500003
gen byte bnd = 0
python:
from sfi import Data, Macro
from math import gcd
N = 2500003
cdiv = lambda a, b: -(-a // b)
old_rows = lambda c: c if c >= 491520 else c * cdiv(491520, c)
def new_rows(c):
    u = c // gcd(c, 8) * 8
    return u * cdiv(491520, u)
bnd = set()
for c in (1001, 100001, 250002, 500010, 2048, 122880):
    for B in (old_rows(c), new_rows(c)):
        k = 1
        while k * B < N:
            bnd.update(k * B + d for d in (-1, 0, 1, 2))
            k += 1
bnd = sorted(n for n in bnd if 1 <= n <= N)
Data.store("bnd", [n - 1 for n in bnd], [1] * len(bnd))
Macro.setGlobal("V182_NBND", str(len(bnd)))
end
gen byte b = mod(_n, 200) - 100
replace b = . if mod(_n, 13) == 0 | bnd
gen int i = mod(_n * 7, 60000) - 30000
replace i = . if mod(_n, 17) == 5 | bnd
gen long l = _n * 7 - 1000
replace l = . if mod(_n, 1000) == 7 | bnd
gen float f = mod(_n, 1000) + 0.5
replace f = . if mod(_n, 7) == 3 | bnd
gen double d = _n + 0.25
replace d = . if mod(_n, 10) == 0 | bnd
gen double dt = 20000 + mod(_n, 5000)
replace dt = . if mod(_n, 9) == 4 | bnd
format dt %td
gen double tc = 1893456000000 + _n * 1000
replace tc = . if mod(_n, 11) == 6 | bnd
format tc %tc
gen double xm = _n / 4
replace xm = .a if mod(_n, 11) == 5
replace xm = .z if mod(_n, 17) == 3
replace xm = . if mod(_n, 19) == 0 | bnd
gen double v = _n * 2
gen double w = cond(_n > 491520 & _n <= 983040, ., _n)
gen str12 s = "r" + string(_n, "%9.0f")
gen strL sl = cond(mod(_n, 99991) == 0, 300 * "y", "s" + string(_n, "%9.0f"))
drop bnd
if (${V182_NBND} < 40) {
    di as err "FAIL: only ${V182_NBND} boundary rows"
    local ++fails
}
qui save `"`base'"'

* ---------- the saves: chunk() x fill workers, traced ------------------------
python:
import os
for k in ("PARQIT_TEST_SAVE_BLOCK_ROWS", "PARQIT_TEST_ARROW_OFFSET_LIMIT", "PARQIT_FILL_THREADS"):
    os.environ.pop(k, None)
os.environ["PARQIT_TEST_SAVE_TRACE"] = "1"
end
capture log close _v182
log using `"`tracelog'"', text replace name(_v182)
local saved
foreach t in 1 4 auto {
    qui parqit set fill_threads `t'
    foreach c in 1001 100001 250002 500010 2048 0 {
        local opt = cond(`c' > 0, "chunk(`c')", "")
        parqit save `"`outstem'_`t'_`c'.parquet"', data replace xmissing `opt'
        local saved `saved' `t'_`c'
    }
}
qui parqit set fill_threads 4
parqit save `"`outstem'_plain.parquet"', data replace chunk(1001)
local saved `saved' plain
log close _v182
python:
import os
os.environ["PARQIT_TEST_ARROW_OFFSET_LIMIT"] = "65536"
end
qui parqit save `"`outstem'_fallback.parquet"', data replace xmissing chunk(1001)
python:
import os
for k in ("PARQIT_TEST_ARROW_OFFSET_LIMIT", "PARQIT_TEST_SAVE_TRACE"):
    os.environ.pop(k, None)
end
qui parqit set fill_threads auto
global V182_SAVED `"`saved' fallback"'

* ---------- the oracle: the generating rule, never another parqit file ------
python:
from sfi import Macro, Scalar
import os, re
import numpy as np
import pyarrow as pa, pyarrow.parquet as pq
ok = 1
def bad(msg):
    global ok
    ok = 0
    print("FAIL: " + msg)
n = np.arange(1, N + 1, dtype=np.int64)
bm = np.zeros(N, dtype=bool)
bm[np.array(bnd) - 1] = True
plain_xm = (n % 19 == 0) | bm
code_xm = np.where(plain_xm, 0, np.where(n % 17 == 3, 26, np.where(n % 11 == 5, 1, 0))).astype(np.int8)
spec = {
    "b": ((n % 13 == 0) | bm, (n % 200 - 100).astype(np.int8), None),
    "i": ((n % 17 == 5) | bm, ((n * 7) % 60000 - 30000).astype(np.int16), None),
    "l": ((n % 1000 == 7) | bm, (n * 7 - 1000).astype(np.int32), None),
    "f": ((n % 7 == 3) | bm, (n % 1000 + 0.5).astype(np.float32), None),
    "d": ((n % 10 == 0) | bm, n + 0.25, None),
    "dt": ((n % 9 == 4) | bm, (20000 + n % 5000 - 3653).astype(np.int32), pa.int32()),
    "tc": ((n % 11 == 6) | bm, (1893456000000 + n * 1000 - 315619200000) * 1000, pa.int64()),
    "xm": (plain_xm | (code_xm != 0), n / 4, None),
    "v": (np.zeros(N, dtype=bool), (n * 2).astype(np.float64), None),
    "w": ((n > 491520) & (n <= 983040), n.astype(np.float64), None),
}
want_s = pa.array(["r" + str(m) for m in range(1, N + 1)], pa.string())
want_sl = pa.array(["y" * 300 if m % 99991 == 0 else "s" + str(m) for m in range(1, N + 1)], pa.string())
def bits(a):
    return a.view({1: np.uint8, 2: np.uint16, 4: np.uint32, 8: np.uint64}[a.dtype.itemsize])
def check(tag, path, xmissing):
    t = pq.read_table(path)
    if t.num_rows != N: return bad(tag + ": rows " + str(t.num_rows))
    if not pa.types.is_date32(t.schema.field("dt").type): bad(tag + ": dt type " + str(t.schema.field("dt").type))
    tct = t.schema.field("tc").type
    if not (pa.types.is_timestamp(tct) and tct.unit == "us"): bad(tag + ": tc type " + str(tct))
    for name, (mask, vals, cast) in spec.items():
        col = t.column(name).combine_chunks()
        if cast is not None: col = col.cast(cast)
        isnull = col.is_null().to_numpy(zero_copy_only=False)
        if not np.array_equal(isnull, mask):
            where = np.flatnonzero(isnull != mask)
            bad("%s: %s null positions differ at %d rows, first obs %d" % (tag, name, where.size, where[0] + 1))
            continue
        got = col.fill_null(0).to_numpy(zero_copy_only=False)
        keep = ~mask
        if not np.array_equal(bits(got[keep].astype(vals.dtype)), bits(vals[keep])):
            bad(tag + ": " + name + " values differ")
    for name, want in (("s", want_s), ("sl", want_sl)):
        if not t.column(name).combine_chunks().cast(pa.string()).equals(want):
            bad(tag + ": " + name + " strings differ")
    comp = "_parqit_xm_xm"
    if xmissing:
        if comp not in t.column_names: return bad(tag + ": no xmissing companion")
        got = t.column(comp).combine_chunks().to_numpy(zero_copy_only=False)
        if not np.array_equal(got, code_xm): bad(tag + ": extended-missing codes differ")
        others = [c for c in t.column_names if c.startswith("_parqit_xm_") and c != comp]
        if others: bad(tag + ": unexpected companions " + str(others))
    elif comp in t.column_names:
        bad(tag + ": a companion without xmissing")
    return pq.ParquetFile(path).metadata
stem = Macro.getLocal("outstem")
saved = Macro.getGlobal("V182_SAVED").split()
meta = {}
groups = {}
for tag in saved:
    md = check(tag, stem + "_" + tag + ".parquet", tag != "plain")
    if md is None: continue
    meta[tag] = md.metadata
    groups[tag] = sorted({md.row_group(g).num_rows for g in range(md.num_row_groups)})
ref = meta.get("1_0")
for tag, m in meta.items():
    if tag != "plain" and m != ref: bad(tag + ": key-value metadata differ from the serial default save")
for c in ("1001", "100001", "250002", "500010", "2048", "0"):
    print("v182 row groups, chunk %s: %s" % (c, groups.get("4_" + c)))
# the trace: blocks of the new rule, workers that ran
trace = [l for l in open(Macro.getLocal("tracelog"), encoding="utf-8") if "save cell pass:" in l]
order = [t for t in saved if t != "fallback"]
if len(trace) != len(order): bad("trace lines %d for %d saves" % (len(trace), len(order)))
try:
    cpus = len(os.sched_getaffinity(0))
except AttributeError:
    cpus = os.cpu_count() or 1
for tag, line in zip(order, trace):
    m = re.search(r"save cell pass: blocks (\d+), workers (\d+)", line)
    t, c = ("4", "1001") if tag == "plain" else tag.split("_")
    rows = new_rows(int(c)) if c != "0" else 491520
    nb = cdiv(N, rows)
    if not m or int(m.group(1)) != nb:
        bad("%s: trace %r, want %d blocks" % (tag, line.strip(), nb))
        continue
    w = int(m.group(2))
    want_w = {"1": 1, "4": min(4, nb, cpus)}.get(t)
    if (want_w is not None and w != want_w) or not 1 <= w <= nb:
        bad("%s: %d workers for %d blocks" % (tag, w, nb))
Scalar.setValue("pyok", ok)
end
if (scalar(pyok) != 1) {
    di as err "FAIL: a save misplaced missing values, changed a value, a code or the metadata, or ran in other blocks"
    local ++fails
}

* ---------- supplementary: the dataset round-trips through parqit use --------
capture noisily parqit use `"`outstem'_4_1001.parquet"', clear
if (_rc == 0) capture noisily cf _all using `"`base'"'
if (_rc) {
    di as err "FAIL: parqit use of the chunk(1001) save differs from the dataset"
    local ++fails
}

if (`fails' == 0) di as result "VERDICT(v182_save_chunk_alignment): PASS"
else di as err "VERDICT(v182_save_chunk_alignment): FAIL (`fails')"
