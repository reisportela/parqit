* V153 — the C aggregate bridge under DuckDB's window constant aggregator
*   (found 2026-10-13 while measuring WINDOW-NEST-1): with window_self_join
*   off — as parqit sets it while a view with two or more window stages is
*   current — a whole-partition window such as egen mean(), by() runs through
*   the window constant aggregator, which passes the bridge a CONSTANT state
*   vector and keeps its own pointer into it; the bridge's 0.1.35 correction
*   flattened that vector in place (use after free: a crash on a 20M-row
*   file, or a later partition's rows added to an earlier state), the
*   corrected bridge spreads the pointers without touching the vector. Many
*   partitions per 2048-row chunk and several chunks per partition exercise
*   both orders.
* Oracle: native egen double on the same rows; a count of the groups.
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
parqit set statamissing off

* 200,000 rows, 1,000 groups of 200 rows (several groups per chunk), plus 4
* groups of 40,000 rows (several chunks per group); x exact in double
parqit sql "SELECT i AS id, CASE WHEN i < 200000 THEN i % 1000 ELSE 1000 + (i % 4) END AS g, CAST(i AS DOUBLE) AS x FROM range(360000) t(i)"
parqit egen double m1 = mean(x), by(g)
parqit egen double t1 = total(x), by(g)
parqit egen double s1 = sd(x), by(g)
parqit egen double n1 = count(x), by(g)
parqit egen double mx = max(x), by(g)
parqit collect, clear
assert _N == 360000
sort g id
egen double m2 = mean(x), by(g)
egen double t2 = total(x), by(g)
egen double s2 = sd(x), by(g)
egen double n2 = count(x), by(g)
egen double mx2 = max(x), by(g)
assert m1 == m2 & t1 == t2 & s1 == s2 & n1 == n2 & mx == mx2
* every group got its own rows: the first group holds the multiples of
* 1,000 below 200,000 (200 rows, mean 99,500) and the last four 40,000 rows each
assert n1[1] == 200 & m1[1] == 99500 & n1[_N] == 40000
quietly count if n1 == 200
assert r(N) == 200000
parqit close _all

di "VERDICT(V153_WINDOW_AGGREGATE_PARTITIONS): PASS - whole-partition window aggregates with the planner workaround current equal native egen double across 1,004 groups and 360,000 rows: no partition's rows reach another's state and nothing crashes"
