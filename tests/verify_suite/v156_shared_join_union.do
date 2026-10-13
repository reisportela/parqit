* V156 — DuckDB 1.5.6 (DUCKDB-156, Codex round 5): UNION ALL arms that share
*   a join subtree (1.5.6 fixed wrong results in common_subplan for that
*   shape) with overlapping arms, missing keys, duplicated keys and
*   many-to-many multiplicities, through both materialisers.
* Oracles: native merge/append on the same rows; pyarrow on the saved file.
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
tempfile stem nat keys
parqit set statamissing off

clear
input long id byte k double x
1 1 10
2 1 11
3 2 12
4 . 13
5 3 14
6 2 15
7 . 16
end
parqit save `"`stem'_m.parquet"', data replace
save `"`stem'_m.dta"', replace
clear
input byte k str4 tag double w
1 "a" 100
1 "aa" 101
2 "b" 200
. "n" 300
4 "d" 400
end
parqit save `"`stem'_k.parquet"', data replace
save `"`keys'"', replace
* native: joinby gives every pairing of a matched key, a missing key matching
* the using row whose key is missing, as in Stata; two overlapping arms
use `"`stem'_m.dta"', clear
joinby k using `"`keys'"'
preserve
keep if x >= 12
tempfile hi
save `"`hi'"'
restore
keep if x <= 13
append using `"`hi'"'
sort id tag x
save `"`nat'"', replace
* parqit: two views over the same join, overlapping, appended
parqit use using `"`stem'_m.parquet"', name(a)
parqit joinby k using `"`stem'_k.parquet"'
parqit keep if x <= 13
parqit use using `"`stem'_m.parquet"', name(b)
parqit joinby k using `"`stem'_k.parquet"'
parqit keep if x >= 12
parqit use using view:a, name(c)
parqit append using view:b
parqit save `"`stem'_u.parquet"', view replace
parqit collect, clear
sort id tag x
cf _all using `"`nat'"', verbose
* the overlap (12 <= x <= 13: ids 3 and 4) is in both arms, so those pairings
* appear twice; a key with two using rows (k = 1) pairs twice; the missing
* keys (ids 4 and 7) match the using row with the missing key, as native;
* k = 3 (id 5) matches nothing
assert _N == 10
quietly count if x == 12
assert r(N) == 2
quietly count if id == 1
assert r(N) == 2
quietly count if id == 4
assert r(N) == 2
quietly count if id == 7
assert r(N) == 1
quietly count if id == 5
assert r(N) == 0
local u `"`stem'_u.parquet"'
python:
from sfi import Macro
import pyarrow.parquet as pq
t = pq.read_table(Macro.getLocal("u")).to_pydict()
ids = t["id"]
assert len(ids) == 10 and ids.count(3) == 2 and ids.count(1) == 2 and ids.count(4) == 2 and ids.count(5) == 0
end
parqit close _all

di "VERDICT(V156_SHARED_JOIN_UNION): PASS - two overlapping views over one joinby, appended, equal native joinby and append with missing and duplicated keys, through collect and save"
