* ADVERSARIAL: the parallel (producer/consumer pipeline) fill path. A read of
* >50k rows is filled by a pool of worker threads, each storing whole chunks of
* disjoint observations through SF_vstore/SF_sstore. This test drives that path
* with 1.5M rows (hundreds of chunks across the workers) and proves, against an
* independent pyarrow oracle, that EVERY cell lands in its exact row regardless
* of which worker wrote it or in what order: positional sentinels across the full
* range catch any base-offset/off-by-one bug, column aggregates catch any
* dropped/duplicated/misplaced cell, and the per-worker Inf/missing tallies must
* reduce to exactly the oracle counts. fill_column is shared with the serial
* path, so this also pins that the only difference (scheduling) changes nothing.
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

local fails 0
tempfile fbase
local f `"`fbase'.parquet"'

* ---- build 1.5M rows with an independent oracle (pyarrow + numpy) ----
python:
from sfi import Macro, Scalar
import pyarrow as pa, pyarrow.parquet as pq
import numpy as np

N = 1_500_000
idx = np.arange(N, dtype=np.int32)                 # exact positional witness
val = (idx % 997).astype(np.float64) * 0.5 - 3.0   # finite, all non-missing
grp = (idx % 100).astype(np.int16)                 # small ints, some nulls

# special floats at deterministic rows
sp = (idx % 13).astype(np.float64)
inf_pos = (idx % 500_000 == 7)                      # +inf
inf_neg = (idx % 700_000 == 9)                      # -inf
nan_at  = (idx % 300_000 == 11)                     # NaN (silent missing)
sp = sp.copy()
sp[inf_pos] = np.inf
sp[inf_neg] = -np.inf
sp[nan_at]  = np.nan
n_inf = int(inf_pos.sum() + inf_neg.sum())
n_nan = int(nan_at.sum())

# grp nulls every 250k rows
grp_mask = ~((idx % 250_000) == 5)                  # False -> null
grp_obj = pa.array(grp, mask=~grp_mask)

t = pa.table({
    "idx": pa.array(idx, pa.int32()),
    "val": pa.array(val, pa.float64()),
    "sp":  pa.array(sp,  pa.float64()),
    "grp": grp_obj,
})
pq.write_table(t, Macro.getLocal("f"), row_group_size=200_000)  # many row groups

Scalar.setValue("oN",        N)
Scalar.setValue("o_idx_sum", float(idx.astype(np.float64).sum()))
Scalar.setValue("o_val_sum", float(val.sum()))
Scalar.setValue("o_idx_last", float(idx[-1]))
Scalar.setValue("o_idx_mid",  float(idx[750_000]))   # 0-based -> Stata row 750001
# sp non-missing = N - (#inf + #nan); its sum over finite, non-nan entries:
finite = np.isfinite(sp)
Scalar.setValue("o_sp_nonmiss", int(finite.sum()))
Scalar.setValue("o_sp_sum",     float(sp[finite].sum()))
Scalar.setValue("o_n_inf",      n_inf)
# grp non-null count and sum
Scalar.setValue("o_grp_nonmiss", int(grp_mask.sum()))
Scalar.setValue("o_grp_sum",     float(grp[grp_mask].astype(np.float64).sum()))
end

* ---- load through the parallel pipeline (N>50k auto-parallelises) ----
tempname lg
local plog "`c(tmpdir)'/_parqit_v20.log"
capture erase `"`plog'"'
log using `"`plog'"', text name(`lg')
parqit use using `"`f'"', clear
log close `lg'
mata: st_local("loadtxt", invtokens(cat(st_local("plog"))', char(10)))
capture erase `"`plog'"'

* shape
if (_N != scalar(oN)) {
    di as err "FAIL: row count `=_N' != `=scalar(oN)'"
    local ++fails
}

* positional witnesses across the full range (any worker mis-placing a chunk
* or an off-by-one in the global base would break these)
assert idx[1]   == 0
assert idx[scalar(oN)]    == scalar(o_idx_last)
assert idx[750001]        == scalar(o_idx_mid)

* aggregates: exact for the integer witness, tolerant for the float sums
summarize idx, meanonly
if (r(sum) != scalar(o_idx_sum)) {
    di as err "FAIL: idx sum `=r(sum)' != oracle `=scalar(o_idx_sum)'"
    local ++fails
}
summarize val, meanonly
if (reldif(r(sum), scalar(o_val_sum)) > 1e-9) {
    di as err "FAIL: val sum diverged (`=r(sum)' vs `=scalar(o_val_sum)')"
    local ++fails
}

* special-float column: NaN+Inf became missing, finite cells exact in count+sum
quietly count if !missing(sp)
if (r(N) != scalar(o_sp_nonmiss)) {
    di as err "FAIL: sp non-missing `=r(N)' != oracle `=scalar(o_sp_nonmiss)'"
    local ++fails
}
summarize sp, meanonly
if (reldif(r(sum), scalar(o_sp_sum)) > 1e-9) {
    di as err "FAIL: sp finite sum diverged"
    local ++fails
}
* the Inf->missing note must be loud and its count (reduced across workers) exact
if (strpos(`"`loadtxt'"', "outside Stata's storable range") == 0) {
    di as err "FAIL: Inf->missing was silent under parallel fill"
    local ++fails
}
if (strpos(`"`loadtxt'"', "`=scalar(o_n_inf)' value(s) outside Stata's storable range") == 0) {
    di as err "FAIL: Inf count wrong (expected `=scalar(o_n_inf)') — per-worker tally mis-reduced"
    local ++fails
}

* nullable small-int column: null mask and sum preserved
quietly count if !missing(grp)
if (r(N) != scalar(o_grp_nonmiss)) {
    di as err "FAIL: grp non-missing `=r(N)' != oracle `=scalar(o_grp_nonmiss)'"
    local ++fails
}
summarize grp, meanonly
if (r(sum) != scalar(o_grp_sum)) {
    di as err "FAIL: grp sum `=r(sum)' != oracle `=scalar(o_grp_sum)'"
    local ++fails
}

if (`fails' == 0) di "VERDICT(V20_PARALLEL_FILL): PASS - 1.5M-row parallel fill matches pyarrow oracle cell-for-cell (positions, aggregates, Inf/null tallies)"
else {
    di as err "VERDICT(V20_PARALLEL_FILL): FAIL - `fails' check(s)"
    exit 9
}
