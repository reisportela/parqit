* N8: a lazy sample is repeatable within its plan and spans the population.
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
set obs 1000000
gen long x = _n
parqit set threads 4
parqit open _data
parqit sample 2, count
quietly parqit summarize x, detail
assert r(N)==2 & r(min)==r(p1) & r(max)==r(p99) & r(mean)==r(p50)
scalar lo = r(min)
scalar hi = r(max)
quietly parqit summarize x, detail
assert r(min)==lo & r(max)==hi
parqit close _all
scalar highest = 0
forvalues seed = 1/16 {
    parqit open _data
    parqit sample 2, count seed(`seed')
    quietly parqit summarize x, detail
    assert r(min)==r(p1) & r(max)==r(p99) & r(mean)==r(p50)
    scalar highest = max(highest,r(max))
    parqit close _all
}
assert highest>500000
parqit open _data
parqit sample .1, seed(42)
quietly parqit count
assert r(N)==1000
capture noisily parqit sample 1e30, count
assert _rc==198
quietly parqit count
assert r(N)==1000
parqit close _all
di "VERDICT(V95_SAMPLE_INTEGRITY): PASS"
