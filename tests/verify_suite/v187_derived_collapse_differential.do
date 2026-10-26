* B8 (audit of 25 Oct 2026): differential cases for the certified gen prefix of
* the exact compact collapse (DERIVED-COLLAPSE-1), beyond v177: a mean that is
* an exact rounding tie, signed zeros, products that overflow to missing, a
* gen ... if whose condition meets missing values, inlist() over strings, in
* both statamissing modes. The compact path must give the general path's bits
* (PARQIT_DISABLE_COMPACT_AGGREGATES=1; pyarrow, every result column), and with
* statamissing on native Stata's counts and sums exactly and its means within
* the last bits (a native mean need not be correctly rounded). Since the audit
* of the execution (G7): the collapse holds only means, sums and counts, so it
* is eligible, and the dispatch trace (PARQIT_TEST_COMPACT_AGGREGATES) must name
* the compact columns, and nothing when the path is disabled; minima and maxima,
* which the compact path does not take, are a separate collapse checked against
* native Stata.
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
parqit set threads 4
* the compact path is DuckDB 2.0's, on files with strong identities (Unix), as in v177
quietly parqit version
local compact = regexm(`"`r(duckdb_version)'"', "^v?2[.]") & c(os) == "Unix"
local fails 0
tempfile source out native
python:
from sfi import Macro
import numpy as np, pyarrow as pa, pyarrow.parquet as pq
G = 5000   # the compact path's pilot wants 4,096 groups or more
g = np.repeat(np.arange(G, dtype=np.int32), 4)
tie = np.tile([2.0**53, 2.0**53 + 2, 2.0**53, 2.0**53 + 2], G)   # mean 2^53 + 1: a tie, to even 2^53
num = np.tile([3.0, 6.0, 9.0, 12.0], G)
x = np.tile([1.0, np.nan, 7.0, 2.0], G)                            # NaN stands for a missing x
big = np.tile([1e200, -1e200, 3.0, 1e-300], G)
s = np.tile(["a", "é", "b", ""], G).astype(object)
s[np.arange(len(s)) % 9 == 0] = None
pq.write_table(pa.table({"g": g, "tie": tie, "num": num,
    "x": pa.array(x, mask=np.isnan(x)), "big": big, "s": pa.array(list(s))}), Macro.getLocal("source") + ".parquet")
end

program define _v187_run
    args source out disabled
    python: import os; os.environ["PARQIT_DISABLE_COMPACT_AGGREGATES"] = "`disabled'"; os.environ["PARQIT_TEST_COMPACT_AGGREGATES"] = "1"
    parqit use using `"`source'.parquet"'
    parqit gen double t = tie*1
    parqit gen double z = -num*0
    parqit gen double o = big*big
    parqit gen double c = num/2 if x > 5
    parqit gen double w = num if inlist(s, "a", "é")
    parqit collapse (mean) mt=t mz=z mo=o mc=c mw=w (sum) st=t sz=z so=o sc=c sw=w (count) nt=t no=o nc=c nw=w, by(g)
    capture log close _v187
    log using `"`out'_`disabled'_trace.log"', text replace name(_v187)
    parqit collect, clear
    log close _v187
    python: import os; os.environ.pop("PARQIT_TEST_COMPACT_AGGREGATES", None)
    parqit save `"`out'_`disabled'.parquet"', data replace
end

* (python blocks stay at top level — the batch parser breaks on them in a loop)
python:
from sfi import Macro, Scalar
import numpy as np, pyarrow.parquet as pq
# the columns the compact path proves exact: t (multiples of 2), z (signed zeros),
# o (an overflow to missing, 9 and an underflow to 0: 32-bit integers), c (halves),
# w (32-bit integers)
COMPACT = {"t", "z", "o", "c", "w"} if Macro.getLocal("compact") == "1" else set()
def compare(mode):
    stem = Macro.getLocal("out") + "_" + mode
    a, b = pq.read_table(stem + "_1.parquet"), pq.read_table(stem + "_0.parquet")
    ok = a.schema == b.schema and a.num_rows == b.num_rows == 5000
    for name in a.column_names:
        ca, cb = a.column(name).combine_chunks(), b.column(name).combine_chunks()
        if not np.array_equal(ca.is_null().to_numpy(zero_copy_only=False), cb.is_null().to_numpy(zero_copy_only=False)):
            ok = False; print("FAIL: nulls of", name)
            continue
        va, vb = ca.fill_null(0).to_numpy(zero_copy_only=False), cb.fill_null(0).to_numpy(zero_copy_only=False)
        if va.dtype.kind == "f":
            va, vb = va.view(np.uint64), vb.view(np.uint64)
        if not np.array_equal(va, vb):
            ok = False; print("FAIL: the compact path changed the bits of", name)
    if not np.all(b.column("mt").to_numpy() == 2.0**53):
        ok = False; print("FAIL: the tie did not round to even")
    # G7: the compact path is taken, for the columns it can prove exact, and
    # not when disabled
    t0, t1 = (open(stem + "_%s_trace.log" % d, encoding="utf-8").read() for d in ("0", "1"))
    if "PARQIT_COMPACT_AGGREGATES|" in t1:
        ok = False; print("FAIL: the disabled run took the compact path")
    line = [l for l in t0.splitlines() if l.startswith("PARQIT_COMPACT_AGGREGATES|")]
    print("dispatch:", line)
    taken = {f.split(":")[0] for f in line[0].split("|")[1:] if f} if len(line) == 1 else set()
    if taken != COMPACT:
        ok = False; print("FAIL: the compact path took", sorted(taken), "not", sorted(COMPACT))
    Scalar.setValue("same_bits", int(ok))
end
foreach mode in on off {
    parqit set statamissing `mode'
    _v187_run `"`source'"' `"`out'_`mode'"' 1
    _v187_run `"`source'"' `"`out'_`mode'"' 0
    python: compare(Macro.getLocal("mode"))
    if (scalar(same_bits) != 1) {
        di as err "FAIL: statamissing `mode': the compact path differs from the general path"
        local ++fails
    }
}
parqit set statamissing on

* native Stata, statamissing on: exact counts, sums, minima and maxima; means within the last bits
python: import os; os.environ.pop("PARQIT_DISABLE_COMPACT_AGGREGATES", None)
parqit use using `"`source'.parquet"', clear
gen double t = tie*1
gen double z = -num*0
gen double o = big*big
gen double c = num/2 if x > 5
gen double w = num if inlist(s, "a", "é")
preserve
collapse (min) lo=o lc=c (max) ho=o hc=c, by(g)
tempfile native_mm
save `"`native_mm'"'
restore
collapse (mean) mt=t mz=z mo=o mc=c mw=w (sum) st=t sz=z so=o sc=c sw=w (count) nt=t no=o nc=c nw=w, by(g)
merge 1:1 g using `"`native_mm'"', assert(match) nogenerate
save `"`native'"'
* the minima and maxima through parqit, outside the compact path
parqit use using `"`source'.parquet"'
parqit gen double o = big*big
parqit gen double c = num/2 if x > 5
parqit collapse (min) lo=o lc=c (max) ho=o hc=c, by(g)
parqit collect, clear
tempfile pmm
save `"`pmm'"'
parqit use using `"`out'_on_0.parquet"', clear
merge 1:1 g using `"`pmm'"', assert(match) nogenerate
rename * p_*
rename p_g g
merge 1:1 g using `"`native'"', assert(match) nogenerate
foreach v in st sz so sc sw nt no nc nw lo lc ho hc {
    capture assert p_`v' == `v'
    if (_rc) {
        di as err "FAIL: `v' differs from native Stata"
        local ++fails
    }
}
foreach v in mt mz mo mc mw {
    capture assert (missing(p_`v') & missing(`v')) | reldif(p_`v', `v') < 1e-15
    if (_rc) {
        di as err "FAIL: the mean `v' is not native Stata's"
        local ++fails
    }
}

if (`fails' == 0) di as result "VERDICT(v187_derived_collapse_differential): PASS"
else di as err "VERDICT(v187_derived_collapse_differential): FAIL (`fails')"
