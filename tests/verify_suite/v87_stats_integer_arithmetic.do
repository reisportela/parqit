* V87: valid long values must not overflow in percentile, range or bin math.
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
tempfile src native out

input long x
2000000000
2000000002
end
parqit save `src', data
collapse (median) m=x (p25) p25=x (p75) p75=x
save `native'
parqit use `src'
parqit tabstat x, s(median p25 p75)
parqit collapse (median) m=x (p25) p25=x (p75) p75=x
parqit save `out'
parqit collect, clear
cf _all using `native'
assert m == 2000000001 & p25 == 2000000000 & p75 == 2000000002
python:
from sfi import Macro
import pyarrow.parquet as pq
assert pq.read_table(Macro.getLocal('out')).to_pydict() == {
    'm': [2000000001.0], 'p25': [2000000000.0], 'p75': [2000000002.0]}
end

clear
input long x
-2000000000
2000000000
end
parqit open _data
parqit tabstat x, s(range)
parqit histogram x, bins(2) nodraw
assert r(N) == 2 & r(bins) == 2
assert r(start) == -2000000000 & r(width) == 2000000000

* Preserve native collapse's rounding for a float source.
clear
set obs 2
gen float x = cond(_n==1, 0.1, 0.2)
parqit open _data
collapse (median) m=x
scalar expected = m[1]
parqit collapse (median) m=x
parqit collect, clear
assert m[1] == expected
parqit close _all
di "VERDICT(V87_STATS_INTEGER_ARITHMETIC): PASS"
