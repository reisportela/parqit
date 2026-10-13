* V152 — WINDOW-NEST-1, the two-table verbs (Codex Astra review, 2026-10-13):
*   a using view whose plan DuckDB 1.5.3's window_self_join optimizer breaks
*   (a mode over a mode: an INTERNAL error on the full result) must still
*   serve joinby, merge and append — the session takes the optimizers both
*   sides need before the probes and the bind, not only after the commit —
*   and the result carries the flag, so its own collect runs too. The modal
*   values are the ones the WINDOW-NEST-1 unit test pins: 3 overall, 3 in
*   group 1 and 5 in group 2.
* Oracle: the join's arithmetic by hand; the unit test's modes.
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
parqit set statamissing off

parqit sql "SELECT * FROM (VALUES (1,3),(1,3),(1,4),(1,NULL),(2,5),(2,5),(2,3)) t(g,r)", name(u)
parqit egen m1 = mode(r)
parqit egen m2 = mode(r), by(g)
* joinby on the modal value: m2 = 3 matches the four rows of group 1, 5 the three of group 2
parqit sql "SELECT * FROM (VALUES (3),(5)) t(m2)", name(m)
parqit joinby m2 using view:u
parqit collect, clear
sort g r
assert _N == 7
assert m1 == 3
assert (m2 == 3) == (g == 1) & (m2 == 5) == (g == 2)
parqit close _all

* merge 1:m on the group: the master's uniqueness probe and the using plan's bind
parqit sql "SELECT * FROM (VALUES (1,3),(1,3),(1,4),(1,NULL),(2,5),(2,5),(2,3)) t(g,r)", name(u)
parqit egen m1 = mode(r)
parqit egen m2 = mode(r), by(g)
parqit sql "SELECT * FROM (VALUES (1, 'a'), (2, 'b'), (3, 'c')) t(g, tag)", name(k)
parqit merge 1:m g using view:u
parqit collect, clear
sort g r
assert _N == 8
assert _merge[8] == 1 & tag[8] == "c"
assert m2[1] == 3 & m2[5] == 5 & m1[1] == 3
parqit close _all

* append: the using view's windows run inside the appended plan
parqit sql "SELECT * FROM (VALUES (1,3),(1,3),(1,4),(1,NULL),(2,5),(2,5),(2,3)) t(g,r)", name(u)
parqit egen m1 = mode(r)
parqit egen m2 = mode(r), by(g)
parqit sql "SELECT 9 AS g, 9 AS r, 9 AS m1, 9 AS m2", name(a)
parqit append using view:u
parqit collect, clear
sort g r
assert _N == 8 & m1[8] == 9 & m1[1] == 3 & m2[1] == 3 & m2[7] == 5
parqit close _all

di "VERDICT(V152_WINDOW_NEST_TWO_TABLE): PASS - joinby, merge and append take a using view whose stacked windows need the planner workaround: the probes, the bind and the collect all run, with the modal values the unit test pins"
