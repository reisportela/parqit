* CODEC-DEFAULT-1 (2026-09-19): an option-less parqit save writes zstd — on
* the data path, the view path and a partitioned tree — compression() still
* selects any other codec, and a tree that mixes codecs (an older snappy tree
* extended with partitions(append) under the new default) reads back whole.
* The codec is read from the written files' column-chunk metadata by pyarrow
* (independent oracle), never from parqit.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

set obs 200
gen long x = _n
gen double y = _n / 7
gen byte g = mod(_n, 3)
gen str12 s = "row" + string(_n)

tempfile base
local d_default `"`base'_data_default.parquet"'
local d_snappy  `"`base'_data_snappy.parquet"'
local d_gzip    `"`base'_data_gzip.parquet"'
local v_default `"`base'_view_default.parquet"'
local p_default `"`base'_tree_default"'
local p_mixed   `"`base'_tree_mixed"'

parqit save `"`d_default'"', replace data
parqit save `"`d_snappy'"', replace data compression(snappy)
parqit save `"`d_gzip'"', replace data compression(gzip)
parqit save `"`p_default'"', data partition_by(g)
* an older tree written with snappy, extended under the new default
parqit save `"`p_mixed'"', data partition_by(g) compression(snappy)
parqit save `"`p_mixed'"', data partition_by(g) partitions(append)

parqit use using `"`d_default'"'
parqit keep if x > 100
parqit save `"`v_default'"', replace
parqit close _all

python:
import glob, os
import pyarrow.parquet as pq
from sfi import Macro
def codec(path):
    m = pq.ParquetFile(path).metadata
    cs = {m.row_group(r).column(c).compression for r in range(m.num_row_groups) for c in range(m.num_columns)}
    return "|".join(sorted(cs))
def tree(path):
    leaves = sorted(glob.glob(os.path.join(path, "**", "*.parquet"), recursive=True))
    return "|".join(sorted({codec(f) for f in leaves})), str(len(leaves))
Macro.setLocal("c_d_default", codec(Macro.getLocal("d_default")))
Macro.setLocal("c_d_snappy", codec(Macro.getLocal("d_snappy")))
Macro.setLocal("c_d_gzip", codec(Macro.getLocal("d_gzip")))
Macro.setLocal("c_v_default", codec(Macro.getLocal("v_default")))
c, n = tree(Macro.getLocal("p_default")); Macro.setLocal("c_tree", c); Macro.setLocal("n_tree", n)
c, n = tree(Macro.getLocal("p_mixed")); Macro.setLocal("c_mixed", c); Macro.setLocal("n_mixed", n)
end

local fails 0
foreach pair in "c_d_default ZSTD" "c_d_snappy SNAPPY" "c_d_gzip GZIP" "c_v_default ZSTD" "c_tree ZSTD" "c_mixed SNAPPY|ZSTD" "n_tree 3" "n_mixed 6" {
    gettoken what expect : pair
    local expect = strtrim("`expect'")
    if ("``what''" == "`expect'") di as txt "  [ok] `what' = ``what''"
    else {
        di as err "  [FAIL] `what' = ``what'' (expected `expect')"
        local fails = `fails' + 1
    }
}

* the zstd default reads back exactly (values, not just the codec label)
parqit use `"`d_default'"', clear
assert _N == 200
assert x == _n & s == "row" + string(_n)
capture assert reldif(y, _n / 7) < 1e-15
if (_rc) local fails = `fails' + 1
parqit use `"`v_default'"', clear
assert _N == 100 & x[1] == 101
* the mixed-codec tree reads as one dataset: the 200 rows twice
parqit use `"`p_mixed'"', clear
capture assert _N == 400
if (_rc) {
    di as err "  [FAIL] mixed-codec tree read back `=_N' rows (expected 400)"
    local fails = `fails' + 1
}
quietly summarize x
capture assert r(sum) == 2 * 200 * 201 / 2
if (_rc) local fails = `fails' + 1

if (`fails' == 0) di "VERDICT(V101_SAVE_DEFAULT_CODEC): PASS"
else di "VERDICT(V101_SAVE_DEFAULT_CODEC): FAIL - `fails' check(s)"
