* V86: statistics reject invalid deferred slices and incomplete varlists.
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
set obs 2
gen long sentinel = 99
quietly datasignature
local signature `"`r(datasignature)'"'

foreach verb in keep drop {
    parqit sql "SELECT i AS x, i*2 AS y FROM range(1,4) t(i)"
    parqit `verb' in 1/10
    foreach cmd in "count" "count if x>0" "summarize x" "summarize x, detail" "tabulate x" "tabulate x y" "misstable x" "misstable patterns x" "levelsof x" "codebook x" "distinct x" "duplicates report x" "duplicates list x" "tabstat x" "correlate x y" "pwcorr x y" "histogram x, nodraw" {
        capture noisily parqit `cmd'
        assert _rc == 198
    }
    parqit describe
    assert r(n_steps) == 1
}

parqit sql "SELECT 1 AS x, NULL::DOUBLE AS y"
foreach cmd in "misstable x absent" "misstable patterns x absent" "codebook x absent" {
    capture noisily parqit `cmd'
    assert _rc == 111
}
parqit count
assert r(N) == 1
capture noisily parqit egen z = max(_n)
assert _rc == 198
parqit describe
assert r(n_steps) == 0
quietly datasignature
assert `"`r(datasignature)'"' == `"`signature'"'

* A valid slice still gives the same moments as the native selected data.
parqit sql "SELECT i AS x FROM range(1,6) t(i)"
parqit keep in 2/4
parqit summarize x
assert r(N) == 3 & r(mean) == 3 & r(sd) == 1
parqit close _all
di "VERDICT(V86_STATS_EXECUTION_CONTRACTS): PASS"
